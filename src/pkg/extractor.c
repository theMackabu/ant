#include "extractor.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

typedef struct __attribute__((packed)) {
  char name[100];
  char mode[8];
  char uid[8];
  char gid[8];
  char size[12];
  char mtime[12];
  char checksum[8];
  char typeflag;
  char linkname[100];
  char magic[6];
  char version[2];
  char uname[32];
  char gname[32];
  char devmajor[8];
  char devminor[8];
  char prefix[155];
  char _padding[12];
} tar_header_t;

_Static_assert(sizeof(tar_header_t) == 512,
               "tar_header_t must be exactly 512 bytes");

typedef enum {
  STATE_READ_HEADER,
  STATE_READ_FILE_DATA,
  STATE_SKIP_PADDING
} parser_state_t;

struct extractor {
  char *output_path;

  // parser state
  parser_state_t state;
  tar_header_t header;
  size_t header_bytes_read;
  uint64_t current_file_remaining;
  size_t skip_bytes;

  // strip prefix configuration
  char strip_prefix[128];
  size_t strip_prefix_len;
  bool prefix_detected;

  // gzip decompression state
  z_stream decompress_stream;
  bool decompress_initialized;

  // active file state
  FILE *current_file;
  char current_file_path[256];
  size_t current_file_path_len;
  uint32_t current_file_mode;

  // stats
  uint32_t files_extracted;
  uint64_t bytes_extracted;
};

static extract_error_t validate_path(const char *path) {
  size_t len = strlen(path);
  if (len == 0 || len > 4096)
    return EXTRACT_INVALID_PATH;
  if (path[0] == '/')
    return EXTRACT_INVALID_PATH;

  size_t segment_start = 0;
  for (size_t i = 0; i < len; i++) {
    unsigned char ch = (unsigned char)path[i];
    if (ch == '\0' || ch == '\\' || ch < 0x20)
      return EXTRACT_INVALID_PATH;
    if (ch == '/') {
      size_t seg_len = i - segment_start;
      if (seg_len == 2) {
        if (path[segment_start] == '.' && path[segment_start + 1] == '.') {
          return EXTRACT_INVALID_PATH;
        }
      }
      segment_start = i + 1;
    }
  }

  size_t final_len = len - segment_start;
  if (final_len == 2) {
    if (path[segment_start] == '.' && path[segment_start + 1] == '.') {
      return EXTRACT_INVALID_PATH;
    }
  }

#ifdef _WIN32
  const char *slash = strrchr(path, '/');
  const char *basename = slash ? slash + 1 : path;
  size_t base_len = strlen(basename);
  if (base_len == 0)
    return EXTRACT_INVALID_PATH;

  static const char *reserved[] = {
      "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
      "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
      "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
  for (size_t r = 0; r < sizeof(reserved) / sizeof(reserved[0]); r++) {
    size_t r_len = strlen(reserved[r]);
    if (base_len >= r_len) {
      if (strncasecmp(basename, reserved[r], r_len) == 0) {
        if (base_len == r_len || basename[r_len] == '.') {
          return EXTRACT_INVALID_PATH;
        }
      }
    }
  }
#endif

  return EXTRACT_OK;
}

static extract_error_t parse_octal(const char *str, size_t max_len,
                                   uint64_t *out_val) {
  size_t len = max_len;
  while (len > 0 && (str[len - 1] == '\0' || str[len - 1] == ' ')) {
    len--;
  }
  size_t start = 0;
  while (start < len && (str[start] == '\0' || str[start] == ' ')) {
    start++;
  }
  if (start == len) {
    *out_val = 0;
    return EXTRACT_OK;
  }
  uint64_t val = 0;
  for (size_t i = start; i < len; i++) {
    char c = str[i];
    if (c < '0' || c > '7')
      return EXTRACT_INVALID_TAR_HEADER;
    val = (val << 3) | (c - '0');
  }
  *out_val = val;
  return EXTRACT_OK;
}

static extract_error_t tar_header_get_name(const tar_header_t *self, char *buf,
                                           size_t buf_len, size_t *out_len) {
  size_t prefix_len = 0;
  while (prefix_len < sizeof(self->prefix) &&
         self->prefix[prefix_len] != '\0') {
    prefix_len++;
  }
  size_t name_len = 0;
  while (name_len < sizeof(self->name) && self->name[name_len] != '\0') {
    name_len++;
  }

  if (prefix_len > 0) {
    size_t total_len = prefix_len + 1 + name_len;
    if (total_len >= buf_len)
      return EXTRACT_INVALID_PATH;
    memcpy(buf, self->prefix, prefix_len);
    buf[prefix_len] = '/';
    memcpy(buf + prefix_len + 1, self->name, name_len);
    buf[total_len] = '\0';
    if (out_len)
      *out_len = total_len;
  } else {
    if (name_len >= buf_len)
      return EXTRACT_INVALID_PATH;
    memcpy(buf, self->name, name_len);
    buf[name_len] = '\0';
    if (out_len)
      *out_len = name_len;
  }
  return EXTRACT_OK;
}

static extract_error_t tar_header_get_size(const tar_header_t *self,
                                           uint64_t *out_size) {
  return parse_octal(self->size, sizeof(self->size), out_size);
}

static extract_error_t tar_header_get_mode(const tar_header_t *self,
                                           uint32_t *out_mode) {
  uint64_t val = 0;
  extract_error_t err = parse_octal(self->mode, sizeof(self->mode), &val);
  if (err == EXTRACT_OK) {
    *out_mode = (uint32_t)val;
  }
  return err;
}

static bool tar_header_is_file(const tar_header_t *self) {
  return self->typeflag == '0' || self->typeflag == 0;
}

static bool tar_header_is_directory(const tar_header_t *self) {
  return self->typeflag == '5';
}

static bool tar_header_is_symlink(const tar_header_t *self) {
  return self->typeflag == '2';
}

static void make_path(const char *base, const char *rel_path) {
  char full_path[4096];
  snprintf(full_path, sizeof(full_path), "%s/%s", base, rel_path);

  char *p = strchr(full_path + strlen(base) + 1, '/');
  while (p) {
    *p = '\0';
    mkdir(full_path, 0755);
    *p = '/';
    p = strchr(p + 1, '/');
  }
  mkdir(full_path, 0755);
}

static void apply_file_mode(extractor_t *self) {
  if (self->current_file_path_len == 0)
    return;
#ifndef _WIN32
  if (self->current_file_mode & 0111) {
    char full_path[4096];
    snprintf(full_path, sizeof(full_path), "%s/%s", self->output_path,
             self->current_file_path);
    chmod(full_path, self->current_file_mode & 0777);
  }
#endif
  self->current_file_path_len = 0;
}

static extract_error_t create_file(extractor_t *self, const char *path,
                                   uint32_t mode) {
  if (self->current_file) {
    fclose(self->current_file);
    apply_file_mode(self);
    self->current_file = NULL;
  }

  const char *slash = strrchr(path, '/');
  if (slash) {
    char *dir_path = strdup(path);
    dir_path[slash - path] = '\0';
    make_path(self->output_path, dir_path);
    free(dir_path);
  }

  char full_path[4096];
  snprintf(full_path, sizeof(full_path), "%s/%s", self->output_path, path);

  self->current_file = fopen(full_path, "wb");
  if (!self->current_file) {
    return EXTRACT_IO_ERROR;
  }

  size_t len = strlen(path);
  if (len >= sizeof(self->current_file_path))
    len = sizeof(self->current_file_path) - 1;
  memcpy(self->current_file_path, path, len);
  self->current_file_path[len] = '\0';
  self->current_file_path_len = len;
  self->current_file_mode = mode;
  self->files_extracted++;
  return EXTRACT_OK;
}

static void handle_directory(extractor_t *self, const char *path) {
  make_path(self->output_path, path);
}

static extract_error_t create_symlink(extractor_t *self, const char *path,
                                      const char *target) {
  if (self->current_file) {
    fclose(self->current_file);
    apply_file_mode(self);
    self->current_file = NULL;
  }

  extract_error_t err = validate_path(target);
  if (err != EXTRACT_OK)
    return err;

  const char *slash = strrchr(path, '/');
  if (slash) {
    char *dir_path = strdup(path);
    dir_path[slash - path] = '\0';
    make_path(self->output_path, dir_path);
    free(dir_path);
  }

  char full_path[4096];
  snprintf(full_path, sizeof(full_path), "%s/%s", self->output_path, path);

#ifdef _WIN32
  DeleteFileA(full_path);
  CreateSymbolicLinkA(full_path, target, 0);
#else
  unlink(full_path);
  if (symlink(target, full_path) != 0) {
    // optional debug logging
  }
#endif
  return EXTRACT_OK;
}

extractor_t *extractor_init(const char *output_path) {
  extractor_t *self = calloc(1, sizeof(extractor_t));
  if (!self)
    return NULL;

  self->output_path = strdup(output_path);
  if (!self->output_path) {
    free(self);
    return NULL;
  }

  // Create base output path
#ifdef _WIN32
  _mkdir(output_path);
#else
  mkdir(output_path, 0755);
#endif

  // Initialize TarParser settings
  self->state = STATE_READ_HEADER;
  strcpy(self->strip_prefix, "package/");
  self->strip_prefix_len = 8;
  self->prefix_detected = false;

  return self;
}

void extractor_deinit(extractor_t *self) {
  if (!self)
    return;
  if (self->current_file) {
    fclose(self->current_file);
    apply_file_mode(self);
  }
  if (self->decompress_initialized) {
    inflateEnd(&self->decompress_stream);
  }
  free(self->output_path);
  free(self);
}

extract_error_t extractor_feed_compressed(extractor_t *self,
                                          const uint8_t *data, size_t len) {
  if (!self->decompress_initialized) {
    memset(&self->decompress_stream, 0, sizeof(self->decompress_stream));
    int ret = inflateInit2(&self->decompress_stream, 15 + 32);
    if (ret != Z_OK)
      return EXTRACT_DECOMPRESSION_FAILED;
    self->decompress_initialized = true;
  }

  self->decompress_stream.next_in = (Bytef *)data;
  self->decompress_stream.avail_in = (uInt)len;

  uint8_t output_buf[256 * 1024];
  while (self->decompress_stream.avail_in > 0) {
    self->decompress_stream.next_out = output_buf;
    self->decompress_stream.avail_out = sizeof(output_buf);

    int ret = inflate(&self->decompress_stream, Z_NO_FLUSH);
    size_t produced = sizeof(output_buf) - self->decompress_stream.avail_out;
    if (produced > 0) {
      extract_error_t err = extractor_feed_tar(self, output_buf, produced);
      if (err != EXTRACT_OK)
        return err;
    }

    if (ret == Z_STREAM_END) {
      break;
    }
    if (ret != Z_OK && ret != Z_BUF_ERROR) {
      return EXTRACT_DECOMPRESSION_FAILED;
    }
  }

  return EXTRACT_OK;
}

extract_error_t extractor_feed_tar(extractor_t *self, const uint8_t *data,
                                   size_t len) {
  size_t offset = 0;
  while (offset < len) {
    switch (self->state) {
    case STATE_READ_HEADER: {
      size_t needed = sizeof(tar_header_t) - self->header_bytes_read;
      size_t to_copy = (len - offset < needed) ? (len - offset) : needed;
      memcpy((uint8_t *)&self->header + self->header_bytes_read, data + offset,
             to_copy);
      self->header_bytes_read += to_copy;
      offset += to_copy;

      if (self->header_bytes_read < sizeof(tar_header_t)) {
        return EXTRACT_OK;
      }
      self->header_bytes_read = 0;

      bool is_zero = true;
      uint8_t *header_bytes = (uint8_t *)&self->header;
      for (size_t i = 0; i < sizeof(tar_header_t); i++) {
        if (header_bytes[i] != 0) {
          is_zero = false;
          break;
        }
      }
      if (is_zero) {
        if (self->current_file) {
          fclose(self->current_file);
          apply_file_mode(self);
          self->current_file = NULL;
        }
        return EXTRACT_OK;
      }

      char path_buf[256];
      size_t name_len = 0;
      extract_error_t err = tar_header_get_name(&self->header, path_buf,
                                                sizeof(path_buf), &name_len);
      if (err != EXTRACT_OK)
        return err;

      if (!self->prefix_detected && tar_header_is_directory(&self->header)) {
        size_t prefix_len = (name_len < 127) ? name_len : 127;
        memcpy(self->strip_prefix, path_buf, prefix_len);
        if (prefix_len > 0 && self->strip_prefix[prefix_len - 1] != '/') {
          self->strip_prefix[prefix_len] = '/';
          prefix_len++;
        }
        self->strip_prefix[prefix_len] = '\0';
        self->strip_prefix_len = prefix_len;
        self->prefix_detected = true;
      }

      const char *stripped_path = path_buf;
      if (self->strip_prefix_len > 0) {
        if (strncmp(path_buf, self->strip_prefix, self->strip_prefix_len) ==
            0) {
          stripped_path = path_buf + self->strip_prefix_len;
        }
      }

      size_t stripped_len = strlen(stripped_path);
      if (stripped_len > 0) {
        err = validate_path(stripped_path);
        if (err != EXTRACT_OK)
          return err;
      }

      uint64_t size = 0;
      err = tar_header_get_size(&self->header, &size);
      if (err != EXTRACT_OK)
        return err;

      uint32_t mode = 0;
      err = tar_header_get_mode(&self->header, &mode);
      if (err != EXTRACT_OK)
        return err;

      if (tar_header_is_directory(&self->header)) {
        if (stripped_len > 0) {
          handle_directory(self, stripped_path);
        }
        self->current_file_remaining = 0;
        self->state = STATE_READ_HEADER;
      } else if (tar_header_is_symlink(&self->header)) {
        if (stripped_len > 0) {
          size_t link_len = 0;
          while (link_len < sizeof(self->header.linkname) &&
                 self->header.linkname[link_len] != '\0') {
            link_len++;
          }
          char link_target[100 + 1];
          memcpy(link_target, self->header.linkname, link_len);
          link_target[link_len] = '\0';

          err = create_symlink(self, stripped_path, link_target);
          if (err != EXTRACT_OK)
            return err;
        }
        self->current_file_remaining = 0;
        self->state = STATE_READ_HEADER;
      } else {
        if (stripped_len > 0) {
          err = create_file(self, stripped_path, mode);
          if (err != EXTRACT_OK)
            return err;
        }
        self->current_file_remaining = size;
        if (size > 0) {
          self->state = STATE_READ_FILE_DATA;
        } else {
          self->state = STATE_READ_HEADER;
        }
      }
      break;
    }

    case STATE_READ_FILE_DATA: {
      uint64_t needed = self->current_file_remaining;
      size_t to_write =
          (len - offset < needed) ? (len - offset) : (size_t)needed;
      if (self->current_file && to_write > 0) {
        size_t written = fwrite(data + offset, 1, to_write, self->current_file);
        if (written < to_write)
          return EXTRACT_IO_ERROR;
        self->bytes_extracted += to_write;
      }
      self->current_file_remaining -= to_write;
      offset += to_write;

      if (self->current_file_remaining == 0) {
        uint64_t size = 0;
        extract_error_t err = tar_header_get_size(&self->header, &size);
        if (err != EXTRACT_OK)
          return err;

        uint64_t padding = (512 - (size % 512)) % 512;
        if (padding > 0) {
          self->skip_bytes = (size_t)padding;
          self->state = STATE_SKIP_PADDING;
        } else {
          if (self->current_file) {
            fclose(self->current_file);
            apply_file_mode(self);
            self->current_file = NULL;
          }
          self->state = STATE_READ_HEADER;
        }
      }
      break;
    }

    case STATE_SKIP_PADDING: {
      size_t to_skip =
          (len - offset < self->skip_bytes) ? (len - offset) : self->skip_bytes;
      self->skip_bytes -= to_skip;
      offset += to_skip;

      if (self->skip_bytes == 0) {
        if (self->current_file) {
          fclose(self->current_file);
          apply_file_mode(self);
          self->current_file = NULL;
        }
        self->state = STATE_READ_HEADER;
      }
      break;
    }
    }
  }
  return EXTRACT_OK;
}

extractor_stats_t extractor_stats(const extractor_t *self) {
  extractor_stats_t s = {.files = self->files_extracted,
                         .bytes = self->bytes_extracted};
  return s;
}

extract_error_t gzip_decompress(const uint8_t *input, size_t input_len,
                                void (*output_fn)(const uint8_t *data,
                                                  size_t len, void *user_data),
                                void *user_data) {
  z_stream stream = {0};
  int ret = inflateInit2(&stream, 15 + 32);
  if (ret != Z_OK)
    return EXTRACT_DECOMPRESSION_FAILED;

  stream.next_in = (Bytef *)input;
  stream.avail_in = (uInt)input_len;

  uint8_t output_buf[256 * 1024];
  while (stream.avail_in > 0) {
    stream.next_out = output_buf;
    stream.avail_out = sizeof(output_buf);

    ret = inflate(&stream, Z_NO_FLUSH);
    size_t produced = sizeof(output_buf) - stream.avail_out;
    if (produced > 0) {
      output_fn(output_buf, produced, user_data);
    }

    if (ret == Z_STREAM_END) {
      inflateEnd(&stream);
      return EXTRACT_OK;
    }

    if (ret != Z_OK && ret != Z_BUF_ERROR) {
      inflateEnd(&stream);
      return EXTRACT_DECOMPRESSION_FAILED;
    }
  }

  inflateEnd(&stream);
  return EXTRACT_OK;
}
