/*
 * ant compile — single-file executables via prebuilt-runtime + append-payload.
 * See include/compile.h for the model and issue #54 for the design.
 *
 * On-disk layout of a fused binary:
 *
 *   [ ant runtime base (an unmodified copy of `ant`)          ]
 *   [ payload:  u32 entry_len | entry_rel_bytes               ]
 *   [           ANTAPP01 archive (u32 count | entries...)     ]
 *   [ trailer:  "ANTFUSE1" | u64 payload_offset | u64 payload_size ]
 *
 * All integers are little-endian. The archive body is byte-compatible with the
 * ANTAPP01 container produced by packages/desktop, so the two share a format.
 * The trailer sits at EOF; the runtime seeks backwards from the end to find it,
 * which keeps detection O(1) and independent of the base binary's size.
 */

#include "compile.h"

#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#endif

static const unsigned char archive_magic[8] = {'A', 'N', 'T', 'A', 'P', 'P', '0', '1'};
static const unsigned char trailer_magic[8] = {'A', 'N', 'T', 'F', 'U', 'S', 'E', '1'};

#define TRAILER_SIZE 24 /* 8 magic + 8 offset + 8 size */
#define ENTRY_FILE 1
#define ENTRY_SYMLINK 2
#define ANT_COMPILE_ENTRY_MAX (1024u * 1024u)

/* --- little-endian primitives ------------------------------------------- */

static void put_u32(unsigned char *out, uint32_t value) {
  for (int i = 0; i < 4; i++) out[i] = (unsigned char)(value >> (8 * i));
}

static void put_u64(unsigned char *out, uint64_t value) {
  for (int i = 0; i < 8; i++) out[i] = (unsigned char)(value >> (8 * i));
}

static uint32_t get_u32(const unsigned char *in) {
  return (uint32_t)in[0] | (uint32_t)in[1] << 8 | (uint32_t)in[2] << 16 | (uint32_t)in[3] << 24;
}

static uint64_t get_u64(const unsigned char *in) {
  uint64_t result = 0;
  for (int i = 7; i >= 0; i--) result = result << 8 | in[i];
  return result;
}

static int read_exact(FILE *file, void *data, size_t size) {
  return size == 0 || fread(data, 1, size, file) == size;
}

static int write_all(FILE *file, const void *data, size_t size) {
  return size == 0 || fwrite(data, 1, size, file) == size;
}

/* --- self executable path ----------------------------------------------- */

static char *self_exe_path(void) {
#if defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(NULL, &size);
  char *buffer = malloc(size);
  if (!buffer) return NULL;
  if (_NSGetExecutablePath(buffer, &size) != 0) {
    free(buffer);
    return NULL;
  }
  char *resolved = realpath(buffer, NULL);
  free(buffer);
  return resolved;
#elif defined(_WIN32)
  char buffer[MAX_PATH];
  DWORD length = GetModuleFileNameA(NULL, buffer, (DWORD)sizeof(buffer));
  if (length == 0 || length >= sizeof(buffer)) return NULL;
  return strdup(buffer);
#else
  char buffer[4096];
  ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (length < 0) return NULL;
  buffer[length] = '\0';
  return strdup(buffer);
#endif
}

/* --- shared path helpers (mirrors packages/desktop archive reader) ------- */

static int safe_relative_path(const char *value) {
  if (!value[0] || value[0] == '/') return 0;
  const char *part = value;
  while (*part) {
    const char *end = strchr(part, '/');
    size_t length = end ? (size_t)(end - part) : strlen(part);
    if (!length || (length == 1 && part[0] == '.') || (length == 2 && part[0] == '.' && part[1] == '.')) return 0;
    if (!end) break;
    part = end + 1;
  }
  return 1;
}

static int make_parents(char *path) {
  for (char *cursor = path + 1; *cursor; cursor++) {
    if (*cursor != '/') continue;
    *cursor = '\0';
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
      *cursor = '/';
      return 0;
    }
    *cursor = '/';
  }
  return 1;
}

static int copy_stream(FILE *source, FILE *destination, uint64_t length) {
  unsigned char buffer[64 * 1024];
  while (length) {
    size_t chunk = length > sizeof(buffer) ? sizeof(buffer) : (size_t)length;
    if (!read_exact(source, buffer, chunk) || !write_all(destination, buffer, chunk)) return 0;
    length -= chunk;
  }
  return 1;
}

/* ======================================================================== *
 * Packaging: `ant compile`                                                 *
 * ======================================================================== */

typedef struct {
  char *absolute; /* on-disk source path */
  char *relative; /* archive-relative path (forward slashes) */
  uint64_t size;
  uint32_t mode;
  unsigned char type;
} archive_entry_t;

typedef struct {
  archive_entry_t *items;
  size_t count;
  size_t capacity;
} archive_list_t;

static int list_push(archive_list_t *list, const char *absolute, const char *relative, const struct stat *info,
                     unsigned char type) {
  if (list->count == list->capacity) {
    size_t next = list->capacity ? list->capacity * 2 : 32;
    archive_entry_t *grown = realloc(list->items, next * sizeof(*grown));
    if (!grown) return 0;
    list->items = grown;
    list->capacity = next;
  }
  archive_entry_t *entry = &list->items[list->count];
  entry->absolute = strdup(absolute);
  entry->relative = strdup(relative);
  entry->size = (uint64_t)info->st_size;
  entry->mode = (uint32_t)(info->st_mode & 0777);
  entry->type = type;
  if (!entry->absolute || !entry->relative) return 0;
  list->count++;
  return 1;
}

static void list_free(archive_list_t *list) {
  for (size_t i = 0; i < list->count; i++) {
    free(list->items[i].absolute);
    free(list->items[i].relative);
  }
  free(list->items);
}

/* Recursively collect regular files and symlinks under root, skipping the
 * output binary itself and version-control noise. */
static int collect_entries(archive_list_t *list, const char *root, const char *prefix, const char *skip_absolute) {
  DIR *dir = opendir(root);
  if (!dir) {
    fprintf(stderr, "ant compile: cannot read directory %s: %s\n", root, strerror(errno));
    return 0;
  }
  int ok = 1;
  struct dirent *item;
  while (ok && (item = readdir(dir))) {
    const char *name = item->d_name;
    if (!strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, ".git")) continue;

    char child[4096];
    snprintf(child, sizeof(child), "%s/%s", root, name);
    char relative[4096];
    if (prefix[0]) snprintf(relative, sizeof(relative), "%s/%s", prefix, name);
    else snprintf(relative, sizeof(relative), "%s", name);

    char *child_real = realpath(child, NULL);
    if (skip_absolute && child_real && !strcmp(child_real, skip_absolute)) {
      free(child_real);
      continue;
    }
    free(child_real);

    struct stat info;
    if (lstat(child, &info) != 0) {
      fprintf(stderr, "ant compile: cannot stat %s: %s\n", child, strerror(errno));
      ok = 0;
      break;
    }

    if (S_ISDIR(info.st_mode)) {
      ok = collect_entries(list, child, relative, skip_absolute);
    } else if (S_ISLNK(info.st_mode)) {
      ok = list_push(list, child, relative, &info, ENTRY_SYMLINK);
    } else if (S_ISREG(info.st_mode)) {
      ok = list_push(list, child, relative, &info, ENTRY_FILE);
    }
  }
  closedir(dir);
  return ok;
}

static int write_archive_entry(FILE *out, const archive_entry_t *entry) {
  unsigned char header[20];
  size_t name_length = strlen(entry->relative);
  put_u32(header, (uint32_t)name_length);
  put_u64(header + 4, entry->size);
  put_u32(header + 12, entry->mode);
  header[16] = entry->type;
  header[17] = header[18] = header[19] = 0;
  if (!write_all(out, header, sizeof(header)) || !write_all(out, entry->relative, name_length)) return 0;

  if (entry->type == ENTRY_SYMLINK) {
    char target[4096];
    ssize_t length = readlink(entry->absolute, target, sizeof(target));
    if (length < 0 || (uint64_t)length != entry->size) return 0;
    return write_all(out, target, (size_t)length);
  }

  FILE *source = fopen(entry->absolute, "rb");
  if (!source) return 0;
  int ok = copy_stream(source, out, entry->size);
  fclose(source);
  return ok;
}

int ant_compile_cmd(int argc, char **argv) {
  const char *entry_arg = NULL;
  const char *output_arg = NULL;
  const char *root_arg = NULL;
  const char *runtime_arg = NULL;

  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    if ((!strcmp(arg, "-o") || !strcmp(arg, "--out")) && i + 1 < argc) output_arg = argv[++i];
    else if (!strcmp(arg, "--root") && i + 1 < argc) root_arg = argv[++i];
    else if (!strcmp(arg, "--runtime") && i + 1 < argc) runtime_arg = argv[++i];
    else if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
      printf("Usage: ant compile <entry> [-o out] [--root dir] [--runtime base]\n\n"
             "  Produce a single self-contained executable from a JS/TS app.\n\n"
             "  <entry>          Application entry module.\n"
             "  -o, --out        Output binary path (default: entry basename).\n"
             "  --root           Directory embedded into the binary (default: entry's dir).\n"
             "  --runtime        Base runtime to append to (default: the running ant).\n");
      return EXIT_SUCCESS;
    } else if (arg[0] == '-') {
      fprintf(stderr, "ant compile: unknown option %s\n", arg);
      return EXIT_FAILURE;
    } else if (!entry_arg) {
      entry_arg = arg;
    } else {
      fprintf(stderr, "ant compile: unexpected argument %s\n", arg);
      return EXIT_FAILURE;
    }
  }

  if (!entry_arg) {
    fprintf(stderr, "ant compile: missing entry module\nUsage: ant compile <entry> [-o out]\n");
    return EXIT_FAILURE;
  }

  int status = EXIT_FAILURE;
  char *entry_real = NULL;
  char *root_real = NULL;
  char *base_path = NULL;
  char *output = NULL;
  char *entry_relative = NULL;
  archive_list_t list = {0};
  FILE *out = NULL;

  entry_real = realpath(entry_arg, NULL);
  if (!entry_real) {
    fprintf(stderr, "ant compile: cannot find entry module %s: %s\n", entry_arg, strerror(errno));
    goto done;
  }

  if (root_arg) {
    root_real = realpath(root_arg, NULL);
  } else {
    char *copy = strdup(entry_real);
    root_real = copy ? realpath(dirname(copy), NULL) : NULL;
    free(copy);
  }
  if (!root_real) {
    fprintf(stderr, "ant compile: cannot resolve root directory: %s\n", strerror(errno));
    goto done;
  }

  /* Entry must live under root; record its root-relative path. */
  size_t root_length = strlen(root_real);
  if (strncmp(entry_real, root_real, root_length) != 0 || entry_real[root_length] != '/') {
    fprintf(stderr, "ant compile: entry %s is not inside root %s\n", entry_real, root_real);
    goto done;
  }
  entry_relative = strdup(entry_real + root_length + 1);
  if (!entry_relative || !safe_relative_path(entry_relative)) {
    fprintf(stderr, "ant compile: invalid entry path\n");
    goto done;
  }

  base_path = runtime_arg ? realpath(runtime_arg, NULL) : self_exe_path();
  if (!base_path) {
    fprintf(stderr, "ant compile: cannot locate runtime base: %s\n", strerror(errno));
    goto done;
  }

  if (output_arg) {
    output = strdup(output_arg);
  } else {
    char *copy = strdup(entry_real);
    const char *base = copy ? basename(copy) : "app";
    const char *dot = strrchr(base, '.');
    size_t stem = dot ? (size_t)(dot - base) : strlen(base);
    output = malloc(stem + 1);
    if (output) {
      memcpy(output, base, stem);
      output[stem] = '\0';
    }
    free(copy);
  }
  if (!output) goto done;

  if (!collect_entries(&list, root_real, "", output)) goto done;
  if (list.count == 0) {
    fprintf(stderr, "ant compile: no files found under %s\n", root_real);
    goto done;
  }

  /* Assemble: copy base, append payload (entry manifest + archive), trailer. */
  out = fopen(output, "wb");
  if (!out) {
    fprintf(stderr, "ant compile: cannot create %s: %s\n", output, strerror(errno));
    goto done;
  }

  {
    FILE *base = fopen(base_path, "rb");
    if (!base) {
      fprintf(stderr, "ant compile: cannot open runtime base %s: %s\n", base_path, strerror(errno));
      goto done;
    }
    struct stat base_info;
    if (fstat(fileno(base), &base_info) != 0) {
      fclose(base);
      goto done;
    }
    if (!copy_stream(base, out, (uint64_t)base_info.st_size)) {
      fclose(base);
      fprintf(stderr, "ant compile: failed copying runtime base\n");
      goto done;
    }
    fclose(base);

    uint64_t payload_offset = (uint64_t)base_info.st_size;

    /* Manifest: entry relative path. */
    unsigned char entry_length[4];
    size_t entry_relative_length = strlen(entry_relative);
    put_u32(entry_length, (uint32_t)entry_relative_length);
    if (!write_all(out, entry_length, sizeof(entry_length)) || !write_all(out, entry_relative, entry_relative_length))
      goto done;

    /* Archive header + entries. */
    unsigned char archive_header[12];
    memcpy(archive_header, archive_magic, 8);
    put_u32(archive_header + 8, (uint32_t)list.count);
    if (!write_all(out, archive_header, sizeof(archive_header))) goto done;
    for (size_t i = 0; i < list.count; i++) {
      if (!write_archive_entry(out, &list.items[i])) {
        fprintf(stderr, "ant compile: failed writing %s\n", list.items[i].relative);
        goto done;
      }
    }

    long payload_end = ftell(out);
    if (payload_end < 0) goto done;
    uint64_t payload_size = (uint64_t)payload_end - payload_offset;

    unsigned char trailer[TRAILER_SIZE];
    memcpy(trailer, trailer_magic, 8);
    put_u64(trailer + 8, payload_offset);
    put_u64(trailer + 16, payload_size);
    if (!write_all(out, trailer, sizeof(trailer))) goto done;
  }

  if (fclose(out) != 0) {
    out = NULL;
    goto done;
  }
  out = NULL;

  if (chmod(output, 0755) != 0) {
    fprintf(stderr, "ant compile: cannot mark %s executable: %s\n", output, strerror(errno));
    goto done;
  }

#if defined(__APPLE__)
  /* Appending bytes invalidates the base binary's code signature; re-sign
   * ad-hoc so the fused binary runs on Apple silicon. Best-effort. */
  {
    char command[8192];
    snprintf(command, sizeof(command), "codesign --force --sign - \"%s\" >/dev/null 2>&1", output);
    if (system(command) != 0)
      fprintf(stderr, "ant compile: warning: ad-hoc codesign failed; run: codesign --force --sign - %s\n", output);
  }
#endif

  fprintf(stderr, "ant compile: wrote %s (%zu file%s, entry %s)\n", output, list.count, list.count == 1 ? "" : "s",
          entry_relative);
  status = EXIT_SUCCESS;

done:
  if (out) fclose(out);
  list_free(&list);
  free(entry_relative);
  free(output);
  free(base_path);
  free(root_real);
  free(entry_real);
  return status;
}

/* ======================================================================== *
 * Runtime: detect + extract a fused payload                                *
 * ======================================================================== */

static int read_trailer(FILE *file, uint64_t file_size, uint64_t *payload_offset, uint64_t *payload_size) {
  if (file_size < TRAILER_SIZE) return 0;
  if (fseek(file, (long)(file_size - TRAILER_SIZE), SEEK_SET) != 0) return 0;
  unsigned char trailer[TRAILER_SIZE];
  if (!read_exact(file, trailer, sizeof(trailer))) return 0;
  if (memcmp(trailer, trailer_magic, 8) != 0) return 0;
  uint64_t offset = get_u64(trailer + 8);
  uint64_t size = get_u64(trailer + 16);
  if (offset >= file_size || size == 0 || offset + size + TRAILER_SIZE != file_size) return 0;
  *payload_offset = offset;
  *payload_size = size;
  return 1;
}

bool ant_compile_has_payload(void) {
  char *path = self_exe_path();
  if (!path) return false;
  FILE *file = fopen(path, "rb");
  free(path);
  if (!file) return false;

  bool found = false;
  if (fseek(file, 0, SEEK_END) == 0) {
    long size = ftell(file);
    uint64_t offset, payload;
    if (size > TRAILER_SIZE) found = read_trailer(file, (uint64_t)size, &offset, &payload) != 0;
  }
  fclose(file);
  return found;
}

/* Extract the ANTAPP01 archive at the current stream position into dest. */
static int extract_archive_stream(FILE *file, const char *dest) {
  unsigned char header[12];
  if (!read_exact(file, header, sizeof(header)) || memcmp(header, archive_magic, 8) != 0) return 0;
  uint32_t count = get_u32(header + 8);

  for (uint32_t i = 0; i < count; i++) {
    unsigned char entry_header[20];
    if (!read_exact(file, entry_header, sizeof(entry_header))) return 0;
    uint32_t path_length = get_u32(entry_header);
    uint64_t data_length = get_u64(entry_header + 4);
    uint32_t mode = get_u32(entry_header + 12) & 0777;
    unsigned char type = entry_header[16];
    if (!path_length || path_length > ANT_COMPILE_ENTRY_MAX || (type != ENTRY_FILE && type != ENTRY_SYMLINK) ||
        (type == ENTRY_SYMLINK && data_length > ANT_COMPILE_ENTRY_MAX))
      return 0;

    char *relative = malloc((size_t)path_length + 1);
    size_t full_length = strlen(dest) + 1 + path_length + 1;
    char *output = malloc(full_length);
    if (!relative || !output || !read_exact(file, relative, path_length)) {
      free(relative);
      free(output);
      return 0;
    }
    relative[path_length] = '\0';
    if (!safe_relative_path(relative)) {
      free(relative);
      free(output);
      return 0;
    }
    snprintf(output, full_length, "%s/%s", dest, relative);
    free(relative);
    if (!make_parents(output)) {
      free(output);
      return 0;
    }

    int ok = 1;
    if (type == ENTRY_SYMLINK) {
      char *target = malloc((size_t)data_length + 1);
      if (!target || !read_exact(file, target, (size_t)data_length)) ok = 0;
      else {
        target[data_length] = '\0';
        ok = symlink(target, output) == 0;
      }
      free(target);
    } else {
      FILE *target = fopen(output, "wb");
      if (!target) ok = 0;
      else {
        ok = copy_stream(file, target, data_length);
        fclose(target);
        if (ok) chmod(output, mode);
      }
    }
    free(output);
    if (!ok) return 0;
  }
  return 1;
}

char *ant_compile_extract_payload(char **out_root) {
  if (out_root) *out_root = NULL;
  char *self = self_exe_path();
  if (!self) return NULL;
  FILE *file = fopen(self, "rb");
  free(self);
  if (!file) return NULL;

  char *entry_path = NULL;
  char *root = NULL;
  char *entry_relative = NULL;

  if (fseek(file, 0, SEEK_END) != 0) goto done;
  {
    long size = ftell(file);
    if (size <= TRAILER_SIZE) goto done;

    uint64_t payload_offset, payload_size;
    if (!read_trailer(file, (uint64_t)size, &payload_offset, &payload_size)) goto done;
    if (fseek(file, (long)payload_offset, SEEK_SET) != 0) goto done;

    unsigned char entry_length[4];
    if (!read_exact(file, entry_length, sizeof(entry_length))) goto done;
    uint32_t entry_relative_length = get_u32(entry_length);
    if (!entry_relative_length || entry_relative_length > ANT_COMPILE_ENTRY_MAX) goto done;
    entry_relative = malloc((size_t)entry_relative_length + 1);
    if (!entry_relative || !read_exact(file, entry_relative, entry_relative_length)) goto done;
    entry_relative[entry_relative_length] = '\0';
    if (!safe_relative_path(entry_relative)) goto done;

    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp = "/tmp";
    size_t template_length = strlen(tmp) + strlen("/ant-XXXXXX") + 1;
    root = malloc(template_length);
    if (!root) goto done;
    snprintf(root, template_length, "%s/ant-XXXXXX", tmp);
    if (!mkdtemp(root)) goto done;

    if (extract_archive_stream(file, root)) {
      size_t path_length = strlen(root) + 1 + entry_relative_length + 1;
      entry_path = malloc(path_length);
      if (entry_path) snprintf(entry_path, path_length, "%s/%s", root, entry_relative);
    }
  }

done:
  fclose(file);
  free(entry_relative);
  if (entry_path && out_root) {
    *out_root = root;
  } else {
    if (root) ant_compile_cleanup(root);
    free(root);
  }
  return entry_path;
}

void ant_compile_cleanup(const char *root) {
  if (!root || !root[0]) return;
  DIR *dir = opendir(root);
  if (!dir) return;
  struct dirent *item;
  while ((item = readdir(dir))) {
    if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
    char child[4096];
    snprintf(child, sizeof(child), "%s/%s", root, item->d_name);
    struct stat info;
    if (lstat(child, &info) == 0 && S_ISDIR(info.st_mode)) ant_compile_cleanup(child);
    else unlink(child);
  }
  closedir(dir);
  rmdir(root);
}
