#ifndef ANT_WASM_UV_SHIM_H
#define ANT_WASM_UV_SHIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

static constexpr int UV_ENOSYS =-52;

typedef struct uv_loop_s { void *data; } uv_loop_t;
typedef enum { UV_METRICS_IDLE_TIME = 1 } uv_loop_option;
typedef struct { bool done; } uv_once_t;
#define UV_ONCE_INIT { false }

typedef struct uv_handle_s uv_handle_t;
typedef struct uv_timer_s uv_timer_t;
typedef struct uv_async_s uv_async_t;
typedef struct uv_work_s uv_work_t;

typedef void (*uv_close_cb)(uv_handle_t *handle);
typedef void (*uv_timer_cb)(uv_timer_t *handle);
typedef void (*uv_async_cb)(uv_async_t *handle);
typedef void (*uv_work_cb)(uv_work_t *req);
typedef void (*uv_after_work_cb)(uv_work_t *req, int status);

#define UV_HANDLE_FIELDS \
  void *data;            \
  uv_loop_t *loop;       \
  bool closing;

struct uv_handle_s { UV_HANDLE_FIELDS };
struct uv_timer_s { UV_HANDLE_FIELDS uv_timer_cb cb; uint64_t timeout; bool active; };
struct uv_async_s { UV_HANDLE_FIELDS uv_async_cb cb; };
struct uv_work_s { void *data; uv_loop_t *loop; };

static inline const char *uv_strerror(int err) {
  return strerror(-err);
}

static inline int uv_queue_work(
  uv_loop_t *loop, uv_work_t *req, 
  uv_work_cb work_cb, uv_after_work_cb after_work_cb
) {
  (void)loop;
  (void)req;
  (void)work_cb;
  (void)after_work_cb;
  return UV_ENOSYS;
}

static inline uint64_t uv_hrtime(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static inline uint64_t uv_now(const uv_loop_t *loop) {
  (void)loop;
  return uv_hrtime() / 1000000u;
}

static inline void uv_once(uv_once_t *guard, void (*callback)(void)) {
  if (guard->done) return;
  guard->done = true;
  callback();
}

static inline int uv_loop_configure(uv_loop_t *loop, uv_loop_option option, ...) {
  (void)loop;
  (void)option;
  return 0;
}

static inline uint64_t uv_metrics_idle_time(uv_loop_t *loop) {
  (void)loop;
  return 0;
}

static inline void uv_unref(uv_handle_t *handle) {
  (void)handle;
}

static inline int uv_cwd(char *buffer, size_t *size) {
  if (*size < 2) {
    *size = 2;
    return -105;
  }
  buffer[0] = '/';
  buffer[1] = '\0';
  *size = 1;
  return 0;
}

static inline int uv_is_closing(const uv_handle_t *handle) {
  return handle->closing;
}

static inline void uv_close(uv_handle_t *handle, uv_close_cb close_cb) {
  handle->closing = true;
  if (close_cb) close_cb(handle);
}

static inline int uv_timer_init(uv_loop_t *loop, uv_timer_t *handle) {
  *handle = (uv_timer_t){ .loop = loop };
  return 0;
}

static inline int uv_timer_start(uv_timer_t *handle, uv_timer_cb cb, uint64_t timeout, uint64_t repeat) {
  (void)repeat;
  handle->cb = cb;
  handle->timeout = timeout;
  handle->active = true;
  return 0;
}

static inline int uv_timer_stop(uv_timer_t *handle) {
  handle->active = false;
  return 0;
}

static inline int uv_async_init(uv_loop_t *loop, uv_async_t *handle, uv_async_cb cb) {
  *handle = (uv_async_t){ .loop = loop, .cb = cb };
  return 0;
}

static inline int uv_async_send(uv_async_t *handle) {
  if (!handle->closing && handle->cb) handle->cb(handle);
  return 0;
}

#undef UV_HANDLE_FIELDS

#endif
