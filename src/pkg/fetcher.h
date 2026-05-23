#ifndef PKG_FETCHER_H
#define PKG_FETCHER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  FETCH_OK = 0,
  FETCH_CONNECTION_FAILED = -1,
  FETCH_TLS_ERROR = -2,
  FETCH_HTTP2_ERROR = -3,
  FETCH_TIMEOUT = -4,
  FETCH_INVALID_URL = -5,
  FETCH_RESPONSE_ERROR = -6,
  FETCH_OUT_OF_MEMORY = -7
} fetch_error_t;

typedef struct fetcher fetcher_t;

typedef struct {
  void (*on_data)(const uint8_t *data, size_t len, void *user_data);
  void (*on_complete)(uint16_t status_code, void *user_data);
  void (*on_error)(fetch_error_t err, void *user_data);
  void *user_data;
} fetch_stream_handler_t;

typedef void (*fetch_metadata_cb)(const char *name, const uint8_t *data,
                                  size_t len, bool has_error, void *user_data);

typedef struct {
  const char *name;
  uint8_t *data;
  size_t data_len;
  bool has_error;
  bool compressed;
} fetch_metadata_result_t;

fetcher_t *fetcher_init(const char *registry_host);
void fetcher_deinit(fetcher_t *self);

void fetcher_initiate_tarball_connections_async(fetcher_t *self);
fetch_error_t fetcher_fetch_tarball(fetcher_t *self, const char *url,
                                    fetch_stream_handler_t handler);

fetch_error_t fetcher_run(fetcher_t *self);
size_t fetcher_tick(fetcher_t *self);
size_t fetcher_pending_tarball_count(const fetcher_t *self);
void fetcher_finish_tarballs(fetcher_t *self);

char *fetcher_fetch_metadata(fetcher_t *self, const char *package_name,
                             size_t *out_len);
char *fetcher_fetch_metadata_full(fetcher_t *self, const char *package_name,
                                  bool full, size_t *out_len);

fetch_error_t
fetcher_fetch_metadata_batch(fetcher_t *self, const char *const *names,
                             size_t count,
                             fetch_metadata_result_t *out_results);

fetch_error_t fetcher_fetch_metadata_streaming(fetcher_t *self,
                                               const char *const *names,
                                               size_t count,
                                               fetch_metadata_cb callback,
                                               void *user_data);

bool fetcher_get_last_http_error(const fetcher_t *self, char *out_url,
                                 size_t max_url_len, uint16_t *out_status);

#endif // PKG_FETCHER_H
