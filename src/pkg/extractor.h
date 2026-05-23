#ifndef PKG_EXTRACTOR_H
#define PKG_EXTRACTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  EXTRACT_OK = 0,
  EXTRACT_DECOMPRESSION_FAILED = -1,
  EXTRACT_INVALID_TAR_HEADER = -2,
  EXTRACT_IO_ERROR = -3,
  EXTRACT_OUT_OF_MEMORY = -4,
  EXTRACT_PATH_TOO_LONG = -5,
  EXTRACT_UNSUPPORTED_FORMAT = -6,
  EXTRACT_INVALID_PATH = -7
} extract_error_t;

typedef struct extractor extractor_t;

extractor_t *extractor_init(const char *output_path);
void extractor_deinit(extractor_t *self);

extract_error_t extractor_feed_compressed(extractor_t *self,
                                          const uint8_t *data, size_t len);
extract_error_t extractor_feed_tar(extractor_t *self, const uint8_t *data,
                                   size_t len);

typedef struct {
  uint32_t files;
  uint64_t bytes;
} extractor_stats_t;

extractor_stats_t extractor_stats(const extractor_t *self);

/* Expose gzip decompression callback for fetcher and general use */
extract_error_t gzip_decompress(const uint8_t *input, size_t input_len,
                                void (*output_fn)(const uint8_t *data,
                                                  size_t len, void *user_data),
                                void *user_data);

#endif // PKG_EXTRACTOR_H
