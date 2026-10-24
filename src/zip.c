#include <compat.h> // IWYU pragma: keep

#include "zip.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

#define ZIP_LOCAL_SIZE 30u
#define ZIP_CENTRAL_SIZE 46u
#define ZIP_EOCD_SIZE 22u

typedef struct {
  FILE *out;
  uLong crc;
  uint64_t written;
  uint32_t size;
} zip_sink_t;

static uint16_t zip_u16(const uint8_t *p) {
  return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t zip_u32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int zip_fail(char *err, size_t err_len, int rc, const char *fmt, ...)
  __attribute__((format(printf, 4, 5)));

static int zip_fail(char *err, size_t err_len, int rc, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, err_len, fmt, ap);
  va_end(ap);
  return rc;
}

static int zip_write(zip_sink_t *sink, const uint8_t *data, size_t len, char *err, size_t err_len) {
  if (sink->written + len > sink->size) return zip_fail(err, err_len, -EINVAL, "zip entry is larger than its recorded size");
  if (len > 0 && fwrite(data, 1, len, sink->out) != len)
    return zip_fail(err, err_len, -EIO, "failed to write extracted file: %s", strerror(errno));
  sink->crc = crc32(sink->crc, data, (uInt)len);
  sink->written += len;
  return 0;
}

static int zip_inflate(const uint8_t *data, uint32_t len, zip_sink_t *sink, char *err, size_t err_len) {
  uint8_t buf[32768];
  z_stream strm;
  memset(&strm, 0, sizeof(strm));
  if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) return zip_fail(err, err_len, -ENOMEM, "failed to initialize inflate");

  strm.next_in = (Bytef *)data;
  strm.avail_in = len;
  int rc = 0;
  for (int ret = Z_OK; rc == 0 && ret != Z_STREAM_END;) {
    strm.next_out = buf;
    strm.avail_out = (uInt)sizeof(buf);
    ret = inflate(&strm, Z_NO_FLUSH);
    if (ret == Z_BUF_ERROR) rc = zip_fail(err, err_len, -EINVAL, "zip entry data is truncated");
    else if (ret != Z_OK && ret != Z_STREAM_END) rc = zip_fail(err, err_len, -EINVAL, "zip entry data is corrupt");
    else rc = zip_write(sink, buf, sizeof(buf) - strm.avail_out, err, err_len);
  }

  inflateEnd(&strm);
  return rc;
}

int ant_zip_extract_single(const uint8_t *zip, size_t zip_len, const char *name, FILE *out, char *err, size_t err_len) {
  const uint8_t *eocd = NULL;
  for (size_t pos = zip_len >= ZIP_EOCD_SIZE ? zip_len - ZIP_EOCD_SIZE + 1 : 0; !eocd && pos-- > 0 && zip_len - pos <= ZIP_EOCD_SIZE + 0xffffu;)
    if (zip_u32(zip + pos) == 0x06054b50u && pos + ZIP_EOCD_SIZE + zip_u16(zip + pos + 20) == zip_len) eocd = zip + pos;
  if (!eocd) return zip_fail(err, err_len, -EINVAL, "archive is not a zip file");

  uint16_t entries = zip_u16(eocd + 10);
  uint32_t cd_size = zip_u32(eocd + 12), cd_offset = zip_u32(eocd + 16);
  if (entries == 0xffffu || cd_size == 0xffffffffu || cd_offset == 0xffffffffu)
    return zip_fail(err, err_len, -ENOTSUP, "zip64 archives are not supported");
  if (entries != 1 || zip_u16(eocd + 8) != 1)
    return zip_fail(err, err_len, -EINVAL, "expected exactly one file in the archive, found %u", (unsigned)entries);

  const uint8_t *cd = zip + cd_offset;
  if (cd_size < ZIP_CENTRAL_SIZE || (uint64_t)cd_offset + cd_size > (uint64_t)(eocd - zip) || zip_u32(cd) != 0x02014b50u)
    return zip_fail(err, err_len, -EINVAL, "zip archive is corrupt");

  uint16_t flags = zip_u16(cd + 8), method = zip_u16(cd + 10), name_len = zip_u16(cd + 28);
  uint32_t crc = zip_u32(cd + 16), compressed = zip_u32(cd + 20), size = zip_u32(cd + 24), local = zip_u32(cd + 42);
  if ((uint64_t)ZIP_CENTRAL_SIZE + name_len + zip_u16(cd + 30) + zip_u16(cd + 32) > cd_size)
    return zip_fail(err, err_len, -EINVAL, "zip archive is corrupt");
  if (name_len != strlen(name) || memcmp(cd + ZIP_CENTRAL_SIZE, name, name_len) != 0)
    return zip_fail(err, err_len, -EINVAL, "unexpected archive entry '%.*s' (expected '%s')", (int)name_len, cd + ZIP_CENTRAL_SIZE, name);
  if (flags & 1u) return zip_fail(err, err_len, -ENOTSUP, "encrypted zip entries are not supported");
  if (method != 0 && method != 8) return zip_fail(err, err_len, -ENOTSUP, "unsupported zip compression method %u", (unsigned)method);
  if (compressed == 0xffffffffu || size == 0xffffffffu || local == 0xffffffffu)
    return zip_fail(err, err_len, -ENOTSUP, "zip64 archives are not supported");
  if (size == 0) return zip_fail(err, err_len, -EINVAL, "zip entry '%s' is empty", name);

  uint64_t data = (uint64_t)local + ZIP_LOCAL_SIZE;
  if (data > cd_offset || zip_u32(zip + local) != 0x04034b50u) return zip_fail(err, err_len, -EINVAL, "zip archive is corrupt");
  data += zip_u16(zip + local + 26) + zip_u16(zip + local + 28);
  if (data + compressed > cd_offset) return zip_fail(err, err_len, -EINVAL, "zip archive is corrupt");

  zip_sink_t sink = {.out = out, .crc = crc32(0L, Z_NULL, 0), .size = size};
  int rc = method == 0
    ? zip_write(&sink, zip + data, compressed, err, err_len)
    : zip_inflate(zip + data, compressed, &sink, err, err_len);
  if (rc != 0) return rc;
  if (sink.written != size)
    return zip_fail(err, err_len, -EINVAL, "zip entry size mismatch: expected %u bytes, got %llu", size, (unsigned long long)sink.written);
  if ((uint32_t)sink.crc != crc)
    return zip_fail(err, err_len, -EINVAL, "zip entry CRC-32 mismatch: expected %08x, got %08x", crc, (uint32_t)sink.crc);
  return 0;
}
