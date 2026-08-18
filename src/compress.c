#include <compat.h> // IWYU pragma: keep

#include "compress.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <compression.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#define DECMPFS_BLOCK 65536u
#define DECMPFS_MAGIC "fpmc"
#define DECMPFS_TYPE_LZFSE_RSRC 12u
#define DECMPFS_XATTR "com.apple.decmpfs"
#define RSRC_XATTR "com.apple.ResourceFork"

static uint8_t *read_all(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;

  if (fseeko(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  off_t size = ftello(f);
  if (size <= 0 || fseeko(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

  uint8_t *buf = malloc((size_t)size);
  if (!buf) { fclose(f); return NULL; }

  size_t got = fread(buf, 1, (size_t)size, f);
  fclose(f);
  if (got != (size_t)size) { free(buf); return NULL; }

  *out_len = got;
  return buf;
}

static void put_u32le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
  p[2] = (uint8_t)((v >> 16) & 0xff);
  p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void put_u64le(uint8_t *p, uint64_t v) {
  put_u32le(p, (uint32_t)(v & 0xffffffffu));
  put_u32le(p + 4, (uint32_t)(v >> 32));
}

static int restore_plain(const char *path, const uint8_t *data, size_t len, uint32_t flags) {
  chflags(path, flags);
  removexattr(path, DECMPFS_XATTR, XATTR_SHOWCOMPRESSION);
  removexattr(path, RSRC_XATTR, XATTR_SHOWCOMPRESSION);

  FILE *f = fopen(path, "wb");
  if (!f) return -1;

  size_t put = fwrite(data, 1, len, f);
  return fclose(f) == 0 && put == len ? 0 : -1;
}

static bool matches_on_disk(const char *path, const uint8_t *data, size_t len) {
  size_t back_len = 0;
  uint8_t *back = read_all(path, &back_len);
  if (!back) return false;

  bool same = back_len == len && memcmp(back, data, len) == 0;
  free(back);
  return same;
}

bool ant_fs_compress_supported(void) {
  return true;
}

int ant_fs_compress(const char *path, uint64_t *out_logical, uint64_t *out_physical) {
  size_t len = 0;
  uint8_t *data = read_all(path, &len);
  if (!data) return -1;

  struct stat st;
  if (stat(path, &st) != 0) { free(data); return -1; }

  size_t nblocks = (len + DECMPFS_BLOCK - 1) / DECMPFS_BLOCK;
  size_t table_bytes = (nblocks + 1) * 4;
  size_t cap = table_bytes + len + nblocks * 1024 + 4096;

  uint8_t *fork = malloc(cap);
  uint8_t *scratch = malloc(compression_encode_scratch_buffer_size(COMPRESSION_LZFSE));
  if (!fork || !scratch) { free(fork); free(scratch); free(data); return -1; }

  size_t cursor = table_bytes;
  int rc = -1;

  for (size_t i = 0; i < nblocks; i++) {
    size_t off = i * DECMPFS_BLOCK;
    size_t chunk = len - off < DECMPFS_BLOCK ? len - off : DECMPFS_BLOCK;
    size_t room = cap - cursor;

    size_t wrote = compression_encode_buffer(
      fork + cursor, room, data + off, chunk, scratch, COMPRESSION_LZFSE
    );
    if (wrote == 0) goto done;

    put_u32le(fork + i * 4, (uint32_t)cursor);
    cursor += wrote;
  }
  put_u32le(fork + nblocks * 4, (uint32_t)cursor);

  if (cursor >= len) goto done;

  if (setxattr(path, RSRC_XATTR, fork, cursor, 0, XATTR_SHOWCOMPRESSION) != 0) goto done;

  uint8_t header[16];
  memcpy(header, DECMPFS_MAGIC, 4);
  put_u32le(header + 4, DECMPFS_TYPE_LZFSE_RSRC);
  put_u64le(header + 8, (uint64_t)len);

  if (setxattr(path, DECMPFS_XATTR, header, sizeof(header), 0, XATTR_SHOWCOMPRESSION) != 0) {
    removexattr(path, RSRC_XATTR, XATTR_SHOWCOMPRESSION);
    goto done;
  }

  if (chflags(path, st.st_flags | UF_COMPRESSED) != 0) {
    removexattr(path, DECMPFS_XATTR, XATTR_SHOWCOMPRESSION);
    removexattr(path, RSRC_XATTR, XATTR_SHOWCOMPRESSION);
    goto done;
  }

  if (!matches_on_disk(path, data, len)) {
    restore_plain(path, data, len, st.st_flags);
    goto done;
  }

  if (out_logical) *out_logical = (uint64_t)len;
  if (out_physical) *out_physical = (uint64_t)cursor;
  rc = 0;

done:
  free(fork);
  free(scratch);
  free(data);
  return rc;
}

#else

bool ant_fs_compress_supported(void) {
  return false;
}

int ant_fs_compress(const char *path, uint64_t *out_logical, uint64_t *out_physical) {
  (void)path;
  (void)out_logical;
  (void)out_physical;
  return -1;
}

#endif
