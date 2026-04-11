#include <compat.h> // IWYU pragma: keep

#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <uthash.h>
#include <utarray.h>
#include <errno.h>

#include "ant.h"
#include "ptr.h"
#include "utf8.h"
#include "utils.h"
#include "base64.h"
#include "errors.h"
#include "watch.h"
#include "internal.h"
#include "runtime.h"
#include "descriptors.h"

#include "gc/roots.h"
#include "gc/modules.h"
#include "silver/engine.h"

#include "modules/fs.h"
#include "modules/date.h"
#include "modules/buffer.h"
#include "modules/events.h"
#include "modules/stream.h"
#include "modules/symbol.h"

typedef enum {
  FS_ENC_NONE = 0,
  FS_ENC_UTF8,
  FS_ENC_UTF16LE,
  FS_ENC_LATIN1,
  FS_ENC_BASE64,
  FS_ENC_BASE64URL,
  FS_ENC_HEX,
  FS_ENC_ASCII,
} fs_encoding_t;

typedef enum {
  FS_OP_READ,
  FS_OP_WRITE,
  FS_OP_UNLINK,
  FS_OP_MKDIR,
  FS_OP_RMDIR,
  FS_OP_STAT,
  FS_OP_READ_BYTES,
  FS_OP_EXISTS,
  FS_OP_READDIR,
  FS_OP_ACCESS,
  FS_OP_REALPATH,
  FS_OP_WRITE_FD,
  FS_OP_READ_FD,
  FS_OP_OPEN,
  FS_OP_CLOSE,
  FS_OP_MKDTEMP,
  FS_OP_CHMOD,
  FS_OP_RENAME
} fs_op_type_t;

typedef struct fs_request_s {
  uv_fs_t uv_req;
  ant_t *js;
  ant_value_t promise;
  ant_value_t target_buffer;
  ant_value_t callback_fn;

  char *path;
  char *path2;
  char *data;
  char *error_msg;
  size_t data_len;
  size_t buf_offset;

  fs_op_type_t op_type;
  fs_encoding_t encoding;
  uv_file fd;

  int completed;
  int failed;
  int recursive;
  int error_code;
  int with_file_types;
} fs_request_t;

typedef enum {
  FS_WATCH_MODE_EVENT = 0,
  FS_WATCH_MODE_STAT
} fs_watch_mode_t;

typedef union {
  uv_fs_event_t event;
  uv_fs_poll_t poll;
} fs_watcher_handle_t;

typedef struct fs_watcher_s {
  ant_t *js;
  ant_value_t obj;
  ant_value_t callback;
  fs_watcher_handle_t handle;
  uv_stat_t last_stat;
  
  struct fs_watcher_s *next_active;
  char *path;
  
  bool last_stat_valid;
  bool in_active_list;
  bool closing;
  bool handle_closed;
  bool finalized;
  bool persistent;
  
  fs_watch_mode_t mode;
} fs_watcher_t;

typedef struct {
  bool persistent;
  bool recursive;
  ant_value_t listener;
} fs_watch_options_t;

typedef struct {
  bool persistent;
  unsigned int interval_ms;
  ant_value_t listener;
} fs_watchfile_options_t;

typedef struct {
  mode_t mode;
  double size, uid, gid;
  double atime_ms, mtime_ms, ctime_ms, birthtime_ms;
} fs_stat_fields_t;

static ant_value_t g_dirent_proto      = 0;
static ant_value_t g_fswatcher_proto   = 0;
static ant_value_t g_fswatcher_ctor    = 0;
static ant_value_t g_readstream_proto  = 0;
static ant_value_t g_readstream_ctor   = 0;
static ant_value_t g_writestream_proto = 0;
static ant_value_t g_writestream_ctor  = 0;

static fs_watcher_t *active_watchers = NULL;
static UT_array *pending_requests    = NULL;

enum { FS_WATCHER_NATIVE_TAG = 0x46535754u }; // FSWT

static fs_watcher_t *fs_watcher_data(ant_value_t value) {
  if (!js_check_native_tag(value, FS_WATCHER_NATIVE_TAG)) return NULL;
  return (fs_watcher_t *)js_get_native_ptr(value);
}

static void fs_add_active_watcher(fs_watcher_t *watcher) {
  if (!watcher || watcher->in_active_list) return;
  watcher->next_active = active_watchers;
  active_watchers = watcher;
  watcher->in_active_list = true;
}

static void fs_remove_active_watcher(fs_watcher_t *watcher) {
  fs_watcher_t **it = NULL;
  if (!watcher || !watcher->in_active_list) return;

  for (it = &active_watchers; *it; it = &(*it)->next_active) {
    if (*it != watcher) continue;
    *it = watcher->next_active;
    watcher->next_active = NULL;
    watcher->in_active_list = false;
    return;
  }
}

static ant_value_t fs_call_value(
  ant_t *js,
  ant_value_t fn,
  ant_value_t this_val,
  ant_value_t *args,
  int nargs
) {
  ant_value_t saved_this = js->this_val;
  ant_value_t result = js_mkundef();

  js->this_val = this_val;
  if (vtype(fn) == T_CFUNC)
    result = ((ant_value_t (*)(ant_t *, ant_value_t *, int))vdata(fn))(js, args, nargs);
  else result = sv_vm_call(js->vm, js, fn, this_val, args, nargs, NULL, false);
  js->this_val = saved_this;
  
  return result;
}

static int parse_open_flags(ant_t *js, ant_value_t arg);
static ant_value_t fs_coerce_path(ant_t *js, ant_value_t arg);

static ant_value_t fs_mk_errno_error(
  ant_t *js, int err_num,
  const char *syscall, const char *path, const char *dest
);

static ant_value_t fs_mk_uv_error(
  ant_t *js, int uv_code,
  const char *syscall, const char *path, const char *dest
);

static bool fs_parse_mode(ant_t *js, ant_value_t arg, mode_t *out) {
  if (vtype(arg) == T_NUM) {
    double mode = js_getnum(arg);
    if (mode < 0) return false;
    *out = (mode_t)mode;
    return true;
  }

  if (vtype(arg) != T_STR) return false;

  size_t len = 0;
  const char *str = js_getstr(js, arg, &len);
  if (!str) return false;

  size_t start = 0;
  if (len > 2 && str[0] == '0' && (str[1] == 'o' || str[1] == 'O')) start = 2;
  if (start == len) return false;

  mode_t mode = 0;
  for (size_t i = start; i < len; i++) {
    if (str[i] < '0' || str[i] > '7') return false;
    mode = (mode_t)((mode << 3) + (str[i] - '0'));
  }

  *out = mode;
  return true;
}

static ant_value_t fs_stream_error(ant_t *js, ant_value_t stream_obj, const char *op, int uv_code) {
  ant_value_t props = js_mkobj(js);
  ant_value_t path_val = js_get(js, stream_obj, "path");
  const char *code = uv_err_name(uv_code);

  if (code) js_set(js, props, "code", js_mkstr(js, code, strlen(code)));
  js_set(js, props, "errno", js_mknum((double)uv_code));
  if (vtype(path_val) == T_STR) js_set(js, props, "path", path_val);
  return js_mkerr_props(js, JS_ERR_TYPE, props, "%s failed: %s", op, uv_strerror(uv_code));
}

static ant_value_t fs_stream_push_chunk(ant_t *js, ant_value_t stream_obj, ant_value_t chunk) {
  return stream_readable_push(js, stream_obj, chunk, js_mkundef());
}

static ant_value_t fs_stream_callback(ant_t *js, ant_value_t callback, ant_value_t value) {
  if (!is_callable(callback)) return js_mkundef();
  return fs_call_value(js, callback, js_mkundef(), &value, 1);
}

static int fs_stream_close_fd_sync(ant_t *js, ant_value_t stream_obj) {
  ant_value_t fd_val = js_get(js, stream_obj, "fd");
  uv_fs_t req;
  int result = 0;

  if (vtype(fd_val) != T_NUM) {
    js_set(js, stream_obj, "pending", js_false);
    js_set(js, stream_obj, "closed", js_true);
    return 0;
  }

  result = uv_fs_close(uv_default_loop(), &req, (uv_file)js_getnum(fd_val), NULL);
  uv_fs_req_cleanup(&req);

  js_set(js, stream_obj, "fd", js_mknull());
  js_set(js, stream_obj, "pending", js_false);
  js_set(js, stream_obj, "closed", js_true);
  
  return result;
}

static int fs_stream_open_fd_sync(ant_t *js, ant_value_t stream_obj) {
  ant_value_t fd_val = js_get(js, stream_obj, "fd");
  ant_value_t path_val = js_get(js, stream_obj, "path");
  ant_value_t mode_val = js_get(js, stream_obj, "mode");
  ant_value_t flags_val = js_get_slot(stream_obj, SLOT_FS_FLAGS);
  
  size_t path_len = 0;
  const char *path = NULL;
  char *path_copy = NULL;
  uv_fs_t req;
  
  int flags = 0;
  int mode = 0666;
  int result = 0;

  if (vtype(fd_val) == T_NUM) return (int)js_getnum(fd_val);
  if (vtype(path_val) != T_STR) return UV_EINVAL;

  path = js_getstr(js, path_val, &path_len);
  if (!path) return UV_EINVAL;

  path_copy = strndup(path, path_len);
  if (!path_copy) return UV_ENOMEM;

  flags = (vtype(flags_val) == T_NUM) ? (int)js_getnum(flags_val) : O_RDONLY;
  if (vtype(mode_val) == T_NUM) mode = (int)js_getnum(mode_val);

  result = uv_fs_open(uv_default_loop(), &req, path_copy, flags, mode, NULL);
  uv_fs_req_cleanup(&req);
  free(path_copy);

  if (result < 0) return result;

  js_set(js, stream_obj, "fd", js_mknum((double)result));
  js_set(js, stream_obj, "pending", js_false);
  js_set(js, stream_obj, "closed", js_false);

  ant_value_t open_arg = js_mknum((double)result);
  eventemitter_emit_args(js, stream_obj, "open", &open_arg, 1);
  eventemitter_emit_args(js, stream_obj, "ready", NULL, 0);

  return result;
}

static ant_value_t fs_stream_destroy(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = js_getthis(js);
  ant_value_t err = nargs > 0 ? args[0] : js_mknull();
  ant_value_t callback = nargs > 1 ? args[1] : js_mkundef();
  int result = fs_stream_close_fd_sync(js, stream_obj);

  if (result < 0 && (is_null(err) || is_undefined(err)))
    err = fs_stream_error(js, stream_obj, "close", result);
  if (is_undefined(err)) err = js_mknull();

  return fs_stream_callback(js, callback, err);
}

static ant_value_t fs_stream_close(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = js_getthis(js);

  if (nargs > 0 && is_callable(args[0])) {
    eventemitter_add_listener(js, stream_obj, "close", args[0], true);
    if (js_truthy(js, js_get(js, stream_obj, "closed")))
      fs_call_value(js, args[0], js_mkundef(), NULL, 0);
  }

  if (!js_truthy(js, js_get(js, stream_obj, "destroyed"))) {
    ant_value_t destroy_fn = js_getprop_fallback(js, stream_obj, "destroy");
    if (is_callable(destroy_fn)) fs_call_value(js, destroy_fn, stream_obj, NULL, 0);
  }

  return stream_obj;
}

static ant_value_t fs_readstream__read(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = js_getthis(js);
  ant_value_t pos_val = js_get(js, stream_obj, "pos");
  ant_value_t end_val = js_get(js, stream_obj, "end");
  ant_value_t bytes_read_val = js_get(js, stream_obj, "bytesRead");
  
  int fd = fs_stream_open_fd_sync(js, stream_obj);
  int64_t pos = (vtype(pos_val) == T_NUM) ? (int64_t)js_getnum(pos_val) : 0;
  int64_t end = (vtype(end_val) == T_NUM) ? (int64_t)js_getnum(end_val) : -1;
  
  size_t want = 16384;
  bool reached_eof = false;

  if (fd < 0) {
    ant_value_t err = fs_stream_error(js, stream_obj, "open", fd);
    ant_value_t destroy_fn = js_getprop_fallback(js, stream_obj, "destroy");
    if (is_callable(destroy_fn)) fs_call_value(js, destroy_fn, stream_obj, &err, 1);
    return js_mkundef();
  }

  if (nargs > 0 && vtype(args[0]) == T_NUM && js_getnum(args[0]) > 0)
    want = (size_t)js_getnum(args[0]);

  if (end >= 0) {
    if (pos > end) {
      if (js_truthy(js, js_get(js, stream_obj, "autoClose"))) fs_stream_close_fd_sync(js, stream_obj);
      return fs_stream_push_chunk(js, stream_obj, js_mknull());
    }
    if ((int64_t)want > (end - pos + 1)) want = (size_t)(end - pos + 1);
  }

  ArrayBufferData *ab = create_array_buffer_data(want);
  if (!ab) {
    ant_value_t err = js_mkerr(js, "Failed to allocate ReadStream buffer");
    ant_value_t destroy_fn = js_getprop_fallback(js, stream_obj, "destroy");
    if (is_callable(destroy_fn)) fs_call_value(js, destroy_fn, stream_obj, &err, 1);
    return js_mkundef();
  }

  uv_fs_t req;
  uv_buf_t buf = uv_buf_init((char *)ab->data, (unsigned int)want);
  int result = uv_fs_read(uv_default_loop(), &req, fd, &buf, 1, pos, NULL);
  uv_fs_req_cleanup(&req);

  if (result < 0) {
    ant_value_t err = fs_stream_error(js, stream_obj, "read", result);
    ant_value_t destroy_fn = js_getprop_fallback(js, stream_obj, "destroy");
    free_array_buffer_data(ab);
    if (is_callable(destroy_fn)) fs_call_value(js, destroy_fn, stream_obj, &err, 1);
    return js_mkundef();
  }

  if (result == 0) {
    free_array_buffer_data(ab);
    if (js_truthy(js, js_get(js, stream_obj, "autoClose")))
      (void)fs_stream_close_fd_sync(js, stream_obj);
    return fs_stream_push_chunk(js, stream_obj, js_mknull());
  }

  ant_value_t chunk = create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, (size_t)result, "Buffer");
  if (vtype(chunk) == T_ERR) {
    free_array_buffer_data(ab);
    return chunk;
  }

  if (result < (int)want) reached_eof = true;
  if (end >= 0 && (pos + result - 1) >= end) reached_eof = true;

  js_set(js, stream_obj, "pos", js_mknum((double)(pos + result)));
  js_set(js, stream_obj, "bytesRead", js_mknum(
    (vtype(bytes_read_val) == T_NUM 
    ? js_getnum(bytes_read_val) : 0.0) + (double)result
  ));
  
  fs_stream_push_chunk(js, stream_obj, chunk);

  if (reached_eof) {
    if (js_truthy(js, js_get(js, stream_obj, "autoClose"))) fs_stream_close_fd_sync(js, stream_obj);
    return fs_stream_push_chunk(js, stream_obj, js_mknull());
  }

  return js_mkundef();
}

static ant_value_t fs_writestream__write(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = js_getthis(js);
  ant_value_t callback = nargs > 2 ? args[2] : js_mkundef();
  ant_value_t pos_val = js_get(js, stream_obj, "pos");
  ant_value_t bytes_written_val = js_get(js, stream_obj, "bytesWritten");
  
  const uint8_t *bytes = NULL;
  size_t len = 0;
  
  int fd = fs_stream_open_fd_sync(js, stream_obj);
  int64_t pos = (vtype(pos_val) == T_NUM) ? (int64_t)js_getnum(pos_val) : -1;
  size_t offset = 0;

  if (fd < 0) return fs_stream_callback(js, callback, fs_stream_error(js, stream_obj, "open", fd));

  if (vtype(args[0]) == T_STR) {
    bytes = (const uint8_t *)js_getstr(js, args[0], &len);
  } else if (!buffer_source_get_bytes(js, args[0], &bytes, &len)) {
    return fs_stream_callback(js, callback, js_mkerr(js, "WriteStream chunk must be a string or ArrayBufferView"));
  }

  while (offset < len) {
    uv_fs_t req;
    uv_buf_t buf = uv_buf_init((char *)(bytes + offset), (unsigned int)(len - offset));
    int result = uv_fs_write(uv_default_loop(), &req, fd, &buf, 1, pos, NULL);
    uv_fs_req_cleanup(&req);

    if (result < 0) {
      if (js_truthy(js, js_get(js, stream_obj, "autoClose"))) fs_stream_close_fd_sync(js, stream_obj);
      return fs_stream_callback(js, callback, fs_stream_error(js, stream_obj, "write", result));
    }
    if (result == 0) {
      if (js_truthy(js, js_get(js, stream_obj, "autoClose"))) fs_stream_close_fd_sync(js, stream_obj);
      return fs_stream_callback(js, callback, js_mkerr(js, "write failed: short write"));
    }

    offset += (size_t)result;
    if (pos >= 0) pos += result;
  }

  if (pos >= 0) js_set(js, stream_obj, "pos", js_mknum((double)pos));
  js_set(js, stream_obj, "bytesWritten", js_mknum(
    (vtype(bytes_written_val) == T_NUM 
    ? js_getnum(bytes_written_val) : 0.0) + (double)offset
  ));
  
  return fs_stream_callback(js, callback, js_mknull());
}

static ant_value_t fs_writestream__final(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = js_getthis(js);
  ant_value_t callback = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t value = js_mknull();

  if (js_truthy(js, js_get(js, stream_obj, "autoClose"))) {
    int result = fs_stream_close_fd_sync(js, stream_obj);
    if (result < 0) value = fs_stream_error(js, stream_obj, "close", result);
  }

  return fs_stream_callback(js, callback, value);
}

static ant_value_t fs_create_readstream_impl(ant_t *js, ant_value_t path_arg, ant_value_t options_arg, ant_value_t proto) {
  ant_value_t path_val = fs_coerce_path(js, path_arg);
  ant_value_t options = is_object_type(options_arg) ? options_arg : js_mkobj(js);
  ant_value_t stream_options = js_mkobj(js);
  
  ant_value_t hwm = js_get(js, options, "highWaterMark");
  ant_value_t flags_raw = js_get(js, options, "flags");
  ant_value_t fd_val = js_get(js, options, "fd");
  ant_value_t start_val = js_get(js, options, "start");
  ant_value_t end_val = js_get(js, options, "end");
  ant_value_t mode_val = js_get(js, options, "mode");
  
  ant_value_t auto_close_val = js_get(js, options, "autoClose");
  ant_value_t emit_close_val = js_get(js, options, "emitClose");
  ant_value_t stream_obj = 0;
  
  int flags = parse_open_flags(js, is_undefined(flags_raw) ? js_mkstr(js, "r", 1) : flags_raw);
  if (vtype(path_val) != T_STR) return js_mkerr(js, "ReadStream path must be a string");
  if (vtype(hwm) == T_NUM && js_getnum(hwm) > 0) js_set(js, stream_options, "highWaterMark", hwm);

  stream_obj = stream_construct_readable(js, proto, stream_options);
  if (is_err(stream_obj)) return stream_obj;

  js_set(js, stream_obj, "_read", js_mkfun(fs_readstream__read));
  js_set(js, stream_obj, "_destroy", js_mkfun(fs_stream_destroy));
  js_set(js, stream_obj, "path", path_val);
  js_set(js, stream_obj, "flags", is_undefined(flags_raw) ? js_mkstr(js, "r", 1) : flags_raw);
  js_set(js, stream_obj, "mode", vtype(mode_val) == T_NUM ? mode_val : js_mknum(0666));
  js_set(js, stream_obj, "fd", vtype(fd_val) == T_NUM ? fd_val : js_mkundef());
  js_set(js, stream_obj, "pending", js_bool(vtype(fd_val) != T_NUM));
  js_set(js, stream_obj, "closed", js_false);
  js_set(js, stream_obj, "autoClose", is_undefined(auto_close_val) ? js_true : js_bool(js_truthy(js, auto_close_val)));
  js_set(js, stream_obj, "emitClose", is_undefined(emit_close_val) ? js_true : js_bool(js_truthy(js, emit_close_val)));
  js_set(js, stream_obj, "bytesRead", js_mknum(0));
  js_set(js, stream_obj, "start", vtype(start_val) == T_NUM ? start_val : js_mkundef());
  js_set(js, stream_obj, "end", vtype(end_val) == T_NUM ? end_val : js_mkundef());
  js_set(js, stream_obj, "pos", js_mknum(vtype(start_val) == T_NUM ? js_getnum(start_val) : 0.0));
  js_set_slot(stream_obj, SLOT_FS_FLAGS, js_mknum((double)flags));
  
  return stream_obj;
}

static ant_value_t fs_create_writestream_impl(ant_t *js, ant_value_t path_arg, ant_value_t options_arg, ant_value_t proto) {
  ant_value_t path_val = fs_coerce_path(js, path_arg);
  ant_value_t options = is_object_type(options_arg) ? options_arg : js_mkobj(js);
  ant_value_t stream_options = js_mkobj(js);
  
  ant_value_t flags_raw = js_get(js, options, "flags");
  ant_value_t fd_val = js_get(js, options, "fd");
  ant_value_t start_val = js_get(js, options, "start");
  ant_value_t mode_val = js_get(js, options, "mode");
  ant_value_t auto_close_val = js_get(js, options, "autoClose");
  ant_value_t emit_close_val = js_get(js, options, "emitClose");
  ant_value_t hwm = js_get(js, options, "highWaterMark");
  ant_value_t stream_obj = 0;
  
  int flags = parse_open_flags(js, is_undefined(flags_raw) ? js_mkstr(js, "w", 1) : flags_raw);
  double start_pos = (vtype(start_val) == T_NUM) ? js_getnum(start_val) : ((flags & O_APPEND) ? -1.0 : 0.0);

  if (vtype(path_val) != T_STR) return js_mkerr(js, "WriteStream path must be a string");
  if (vtype(hwm) == T_NUM && js_getnum(hwm) > 0) js_set(js, stream_options, "highWaterMark", hwm);

  stream_obj = stream_construct_writable(js, proto, stream_options);
  if (is_err(stream_obj)) return stream_obj;

  js_set(js, stream_obj, "_write", js_mkfun(fs_writestream__write));
  js_set(js, stream_obj, "_final", js_mkfun(fs_writestream__final));
  js_set(js, stream_obj, "_destroy", js_mkfun(fs_stream_destroy));
  js_set(js, stream_obj, "path", path_val);
  js_set(js, stream_obj, "flags", is_undefined(flags_raw) ? js_mkstr(js, "w", 1) : flags_raw);
  js_set(js, stream_obj, "mode", vtype(mode_val) == T_NUM ? mode_val : js_mknum(0666));
  js_set(js, stream_obj, "fd", vtype(fd_val) == T_NUM ? fd_val : js_mkundef());
  js_set(js, stream_obj, "pending", js_bool(vtype(fd_val) != T_NUM));
  js_set(js, stream_obj, "closed", js_false);
  js_set(js, stream_obj, "autoClose", is_undefined(auto_close_val) ? js_true : js_bool(js_truthy(js, auto_close_val)));
  js_set(js, stream_obj, "emitClose", is_undefined(emit_close_val) ? js_true : js_bool(js_truthy(js, emit_close_val)));
  js_set(js, stream_obj, "bytesWritten", js_mknum(0));
  js_set(js, stream_obj, "start", vtype(start_val) == T_NUM ? start_val : js_mkundef());
  js_set(js, stream_obj, "pos", js_mknum(start_pos));
  js_set_slot(stream_obj, SLOT_FS_FLAGS, js_mknum((double)flags));
  
  return stream_obj;
}

static ant_value_t js_readstream_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "ReadStream() requires a path argument");
  return fs_create_readstream_impl(js, args[0], nargs > 1 ? args[1] : js_mkundef(), g_readstream_proto);
}

static ant_value_t js_writestream_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "WriteStream() requires a path argument");
  return fs_create_writestream_impl(js, args[0], nargs > 1 ? args[1] : js_mkundef(), g_writestream_proto);
}

static ant_value_t builtin_fs_createReadStream(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "createReadStream() requires a path argument");
  return fs_create_readstream_impl(js, args[0], nargs > 1 ? args[1] : js_mkundef(), g_readstream_proto);
}

static ant_value_t builtin_fs_createWriteStream(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "createWriteStream() requires a path argument");
  return fs_create_writestream_impl(js, args[0], nargs > 1 ? args[1] : js_mkundef(), g_writestream_proto);
}

static void fs_init_stream_constructors(ant_t *js) {
  if (g_readstream_ctor && g_writestream_ctor) return;

  stream_init_constructors(js);

  g_readstream_proto = js_mkobj(js);
  js_set_proto_init(g_readstream_proto, stream_readable_prototype(js));
  js_set(js, g_readstream_proto, "close", js_mkfun(fs_stream_close));
  js_set_sym(js, g_readstream_proto, get_toStringTag_sym(), js_mkstr(js, "ReadStream", 10));
  g_readstream_ctor = js_make_ctor(js, js_readstream_ctor, g_readstream_proto, "ReadStream", 10);
  js_set_proto_init(g_readstream_ctor, stream_readable_constructor(js));

  g_writestream_proto = js_mkobj(js);
  js_set_proto_init(g_writestream_proto, stream_writable_prototype(js));
  js_set(js, g_writestream_proto, "close", js_mkfun(fs_stream_close));
  js_set_sym(js, g_writestream_proto, get_toStringTag_sym(), js_mkstr(js, "WriteStream", 11));
  g_writestream_ctor = js_make_ctor(js, js_writestream_ctor, g_writestream_proto, "WriteStream", 11);
  js_set_proto_init(g_writestream_ctor, stream_writable_constructor(js));
}

static ant_value_t fs_make_date(ant_t *js, double ms) {
  ant_value_t obj = js_mkobj(js);
  ant_value_t date_proto = js_get_ctor_proto(js, "Date", 4);
  if (is_object_type(date_proto)) js_set_proto_init(obj, date_proto);
  js_set_slot(obj, SLOT_DATA, tov(ms));
  js_set_slot(obj, SLOT_BRAND, js_mknum(BRAND_DATE));
  return obj;
}

static ant_value_t fs_stats_object_new(ant_t *js, const fs_stat_fields_t *f) {
  ant_value_t stat_obj = js_mkobj(js);
  ant_value_t proto = js_get_ctor_proto(js, "Stats", 5);

  if (is_object_type(proto) || is_special_object(proto))
    js_set_proto_init(stat_obj, proto);

  js_set_slot(stat_obj, SLOT_DATA, js_mknum((double)f->mode));
  js_set(js, stat_obj, "size", js_mknum(f->size));
  js_set(js, stat_obj, "mode", js_mknum((double)f->mode));
  js_set(js, stat_obj, "uid", js_mknum(f->uid));
  js_set(js, stat_obj, "gid", js_mknum(f->gid));

  js_set(js, stat_obj, "atimeMs",      js_mknum(f->atime_ms));
  js_set(js, stat_obj, "mtimeMs",      js_mknum(f->mtime_ms));
  js_set(js, stat_obj, "ctimeMs",      js_mknum(f->ctime_ms));
  js_set(js, stat_obj, "birthtimeMs",  js_mknum(f->birthtime_ms));

  js_set(js, stat_obj, "atime",      fs_make_date(js, f->atime_ms));
  js_set(js, stat_obj, "mtime",      fs_make_date(js, f->mtime_ms));
  js_set(js, stat_obj, "ctime",      fs_make_date(js, f->ctime_ms));
  js_set(js, stat_obj, "birthtime",  fs_make_date(js, f->birthtime_ms));
  
  return stat_obj;
}

static double uv_ts_to_ms(uv_timespec_t ts) {
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static double posix_ts_to_ms(struct timespec ts) {
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

#ifdef _WIN32
  #define POSIX_ATIME_MS(st) ((double)(st)->st_atime * 1000.0)
  #define POSIX_MTIME_MS(st) ((double)(st)->st_mtime * 1000.0)
  #define POSIX_CTIME_MS(st) ((double)(st)->st_ctime * 1000.0)
  #define POSIX_BIRTH_MS(st) 0.0
#elif defined(__APPLE__)
  #define POSIX_ATIME_MS(st) posix_ts_to_ms((st)->st_atimespec)
  #define POSIX_MTIME_MS(st) posix_ts_to_ms((st)->st_mtimespec)
  #define POSIX_CTIME_MS(st) posix_ts_to_ms((st)->st_ctimespec)
  #define POSIX_BIRTH_MS(st) posix_ts_to_ms((st)->st_birthtimespec)
#elif defined(__linux__)
  #define POSIX_ATIME_MS(st) posix_ts_to_ms((st)->st_atim)
  #define POSIX_MTIME_MS(st) posix_ts_to_ms((st)->st_mtim)
  #define POSIX_CTIME_MS(st) posix_ts_to_ms((st)->st_ctim)
  #define POSIX_BIRTH_MS(st) 0.0
#else
  #define POSIX_ATIME_MS(st) posix_ts_to_ms((st)->st_atim)
  #define POSIX_MTIME_MS(st) posix_ts_to_ms((st)->st_mtim)
  #define POSIX_CTIME_MS(st) posix_ts_to_ms((st)->st_ctim)
  #define POSIX_BIRTH_MS(st) 0.0
#endif

static const fs_stat_fields_t fs_stat_fields_zero = {0};

static ant_value_t fs_stats_object_from_uv(ant_t *js, const uv_stat_t *st) {
  if (!st) return fs_stats_object_new(js, &fs_stat_fields_zero);
  return fs_stats_object_new(js, &(fs_stat_fields_t){
    .mode         = (mode_t)st->st_mode,
    .size         = (double)st->st_size,
    .uid          = (double)st->st_uid,
    .gid          = (double)st->st_gid,
    .atime_ms     = uv_ts_to_ms(st->st_atim),
    .mtime_ms     = uv_ts_to_ms(st->st_mtim),
    .ctime_ms     = uv_ts_to_ms(st->st_ctim),
    .birthtime_ms = uv_ts_to_ms(st->st_birthtim),
  });
}

static ant_value_t fs_stats_object_from_posix(ant_t *js, const struct stat *st) {
  if (!st) return fs_stats_object_new(js, &fs_stat_fields_zero);
  return fs_stats_object_new(js, &(fs_stat_fields_t){
    .mode         = st->st_mode,
    .size         = (double)st->st_size,
    .uid          = (double)st->st_uid,
    .gid          = (double)st->st_gid,
    .atime_ms     = POSIX_ATIME_MS(st),
    .mtime_ms     = POSIX_MTIME_MS(st),
    .ctime_ms     = POSIX_CTIME_MS(st),
    .birthtime_ms = POSIX_BIRTH_MS(st),
  });
}

static bool fs_stat_path_sync(const char *path, uv_stat_t *out) {
  uv_fs_t req;
  int rc = 0;

  if (!path || !out) return false;
  memset(out, 0, sizeof(*out));

  rc = uv_fs_stat(NULL, &req, path, NULL);
  if (rc < 0) {
    uv_fs_req_cleanup(&req);
    return false;
  }

  *out = req.statbuf;
  uv_fs_req_cleanup(&req);
  return true;
}

static const char *fs_watch_basename(const char *path) {
  const char *name = NULL;

  if (!path || !*path) return NULL;
  name = strrchr(path, '/');
#ifdef _WIN32
  {
    const char *alt = strrchr(path, '\\');
    if (!name || (alt && alt > name)) name = alt;
  }
#endif
  return name ? name + 1 : path;
}

static ant_value_t fs_watch_error(ant_t *js, int status, const char *path) {
  ant_value_t props = js_mkobj(js);
  const char *code = uv_err_name(status);
  js_set(js, props, "code", js_mkstr(js, code, strlen(code)));
  
  return js_mkerr_props(
    js, JS_ERR_TYPE,
    props, "%s: %s",
    path ? path : "watch",
    uv_strerror(status)
  );
}

static void fs_watcher_free(fs_watcher_t *watcher) {
  if (!watcher) return;
  free(watcher->path);
  free(watcher);
}

static uv_handle_t *fs_watcher_uv_handle(fs_watcher_t *watcher) {
  if (!watcher) return NULL;
  if (watcher->mode == FS_WATCH_MODE_STAT) return (uv_handle_t *)&watcher->handle.poll;
  return (uv_handle_t *)&watcher->handle.event;
}

static void fs_watcher_on_handle_closed(uv_handle_t *handle) {
  fs_watcher_t *watcher = (fs_watcher_t *)handle->data;

  if (!watcher) return;
  watcher->handle_closed = true;
  watcher->closing = false;
  if (watcher->finalized) fs_watcher_free(watcher);
}

static void fs_watcher_close_native(fs_watcher_t *watcher) {
  uv_handle_t *handle = NULL;

  if (!watcher) return;
  if (watcher->closing || watcher->handle_closed) return;

  handle = fs_watcher_uv_handle(watcher);
  if (!handle) return;

  fs_remove_active_watcher(watcher);
  if (watcher->mode == FS_WATCH_MODE_STAT) uv_fs_poll_stop(&watcher->handle.poll);
  else ant_watch_stop(&watcher->handle.event);
  watcher->closing = true;
  uv_close(handle, fs_watcher_on_handle_closed);
}

static void fs_watcher_emit_error(fs_watcher_t *watcher, int status) {
  ant_value_t args[1];

  if (!watcher || vtype(watcher->obj) != T_OBJ) return;
  args[0] = fs_watch_error(watcher->js, status, watcher->path);
  eventemitter_emit_args(watcher->js, watcher->obj, "error", args, 1);
}

static void fs_watcher_emit_change(fs_watcher_t *watcher, const char *filename, int events) {
  ant_value_t args[2];
  const char *event_name = "change";
  const char *name = filename;

  if (!watcher || vtype(watcher->obj) != T_OBJ) return;
  if ((events & UV_RENAME) != 0) event_name = "rename";
  if (!name || !*name) name = fs_watch_basename(watcher->path);

  args[0] = js_mkstr(watcher->js, event_name, strlen(event_name));
  args[1] = name ? js_mkstr(watcher->js, name, strlen(name)) : js_mkundef();
  eventemitter_emit_args(watcher->js, watcher->obj, "change", args, 2);
}

static void fs_watcher_invoke_watchfile_stats(
  fs_watcher_t *watcher,
  const uv_stat_t *curr,
  const uv_stat_t *prev
) {
  uv_stat_t curr_stat;
  uv_stat_t prev_stat;
  ant_value_t args[2];

  if (!watcher || !is_callable(watcher->callback)) return;

  memset(&curr_stat, 0, sizeof(curr_stat));
  memset(&prev_stat, 0, sizeof(prev_stat));
  if (curr) curr_stat = *curr;
  if (prev) prev_stat = *prev;

  watcher->last_stat = curr_stat;
  watcher->last_stat_valid = curr != NULL;

  args[0] = fs_stats_object_from_uv(watcher->js, &curr_stat);
  args[1] = fs_stats_object_from_uv(watcher->js, &prev_stat);
  fs_call_value(watcher->js, watcher->callback, js_mkundef(), args, 2);
}

static void fs_watcher_on_event(
  uv_fs_event_t *handle,
  const char *filename,
  int events,
  int status
) {
  fs_watcher_t *watcher = (fs_watcher_t *)handle->data;

  if (!watcher) return;
  if (status < 0) {
    fs_watcher_emit_error(watcher, status);
    return;
  }

  if ((events & (UV_CHANGE | UV_RENAME)) == 0) return;
  fs_watcher_emit_change(watcher, filename, events);
}

static void fs_watcher_on_poll(
  uv_fs_poll_t *handle,
  int status,
  const uv_stat_t *prev,
  const uv_stat_t *curr
) {
  fs_watcher_t *watcher = (fs_watcher_t *)handle->data;
  uv_stat_t missing = {0};

  if (!watcher) return;
  if (status < 0 && status != UV_ENOENT) return;
  if (status == UV_ENOENT) {
    if (watcher->last_stat_valid) fs_watcher_invoke_watchfile_stats(watcher, &missing, &watcher->last_stat);
    else fs_watcher_invoke_watchfile_stats(watcher, &missing, &missing);
    return;
  }

  fs_watcher_invoke_watchfile_stats(watcher, curr, prev);
}

static ant_value_t js_fswatcher_close(ant_t *js, ant_value_t *args, int nargs) {
  fs_watcher_t *watcher = fs_watcher_data(js->this_val);

  if (!watcher) return js->this_val;
  fs_watcher_close_native(watcher);
  return js->this_val;
}

static ant_value_t js_fswatcher_ref(ant_t *js, ant_value_t *args, int nargs) {
  fs_watcher_t *watcher = fs_watcher_data(js->this_val);
  uv_handle_t *handle = NULL;

  if (!watcher || watcher->handle_closed) return js->this_val;
  handle = fs_watcher_uv_handle(watcher);
  if (handle) uv_ref(handle);
  watcher->persistent = true;
  return js->this_val;
}

static ant_value_t js_fswatcher_unref(ant_t *js, ant_value_t *args, int nargs) {
  fs_watcher_t *watcher = fs_watcher_data(js->this_val);
  uv_handle_t *handle = NULL;

  if (!watcher || watcher->handle_closed) return js->this_val;
  handle = fs_watcher_uv_handle(watcher);
  if (handle) uv_unref(handle);
  watcher->persistent = false;
  return js->this_val;
}

static ant_value_t js_fswatcher_ctor(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "FSWatcher cannot be constructed directly");
}

static void fs_watcher_finalize(ant_t *js, ant_object_t *obj) {
  fs_watcher_t *watcher = NULL;
  if (!obj) return;

  watcher = (fs_watcher_t *)obj->native.ptr;
  obj->native.ptr = NULL;
  if (!watcher) return;

  watcher->finalized = true;
  fs_watcher_close_native(watcher);
  if (watcher->handle_closed) fs_watcher_free(watcher);
}

static void fs_init_watch_constructors(ant_t *js) {
  ant_value_t events = 0;
  ant_value_t ee_ctor = 0;
  ant_value_t ee_proto = 0;

  if (g_fswatcher_proto && g_fswatcher_ctor) return;

  events = events_library(js);
  ee_ctor = js_get(js, events, "EventEmitter");
  ee_proto = js_get(js, ee_ctor, "prototype");

  g_fswatcher_proto = js_mkobj(js);
  js_set_proto_init(g_fswatcher_proto, ee_proto);
  
  js_set(js, g_fswatcher_proto, "close", js_mkfun(js_fswatcher_close));
  js_set(js, g_fswatcher_proto, "ref", js_mkfun(js_fswatcher_ref));
  js_set(js, g_fswatcher_proto, "unref", js_mkfun(js_fswatcher_unref));
  
  js_set_sym(js, g_fswatcher_proto, get_toStringTag_sym(), js_mkstr(js, "FSWatcher", 9));
  g_fswatcher_ctor = js_make_ctor(js, js_fswatcher_ctor, g_fswatcher_proto, "FSWatcher", 9);
}

static bool fs_parse_watch_options(ant_t *js, ant_value_t *args, int nargs, fs_watch_options_t *out) {
  ant_value_t options = js_mkundef();
  ant_value_t persistent_val = js_mkundef();

  if (!out) return false;

  memset(out, 0, sizeof(*out));
  out->persistent = true;
  out->listener = js_mkundef();

  if (nargs > 1) {
    if (is_callable(args[1])) out->listener = args[1];
    else options = args[1];
  }

  if (nargs > 2 && is_callable(args[2])) out->listener = args[2];
  if (vtype(options) == T_UNDEF || vtype(options) == T_NULL || vtype(options) == T_STR) return true;
  if (vtype(options) != T_OBJ) return false;

  persistent_val = js_get(js, options, "persistent");
  if (vtype(persistent_val) != T_UNDEF) out->persistent = js_truthy(js, persistent_val);
  out->recursive = js_truthy(js, js_get(js, options, "recursive"));
  return true;
}

static bool fs_parse_watchfile_options(
  ant_t *js,
  ant_value_t *args,
  int nargs,
  fs_watchfile_options_t *out
) {
  ant_value_t options = js_mkundef();
  ant_value_t persistent_val = js_mkundef();

  if (!out) return false;

  memset(out, 0, sizeof(*out));
  out->persistent = true;
  out->interval_ms = 5007;
  out->listener = js_mkundef();

  if (nargs > 1) {
    if (is_callable(args[1])) {
      out->listener = args[1];
      return true;
    }
    options = args[1];
  }

  if (nargs > 2 && is_callable(args[2])) out->listener = args[2];
  if (!is_callable(out->listener)) return false;
  if (vtype(options) == T_UNDEF || vtype(options) == T_NULL) return true;
  if (vtype(options) != T_OBJ) return false;

  persistent_val = js_get(js, options, "persistent");
  if (vtype(persistent_val) != T_UNDEF) out->persistent = js_truthy(js, persistent_val);
  {
    ant_value_t interval_val = js_get(js, options, "interval");
    if (vtype(interval_val) == T_NUM && js_getnum(interval_val) > 0)
      out->interval_ms = (unsigned int)js_getnum(interval_val);
  }
  return true;
}

static fs_watcher_t *fs_watcher_new(ant_t *js, fs_watch_mode_t mode) {
  fs_watcher_t *watcher = calloc(1, sizeof(*watcher));

  if (!watcher) return NULL;
  watcher->js = js;
  watcher->obj = js_mkundef();
  watcher->callback = js_mkundef();
  watcher->persistent = true;
  watcher->mode = mode;
  
  return watcher;
}

static int fs_watcher_start_event(fs_watcher_t *watcher, const char *path, bool persistent, bool recursive) {
  unsigned int flags = 0;

  if (!watcher || !path) return UV_EINVAL;
#ifdef UV_FS_EVENT_RECURSIVE
  if (recursive) flags |= UV_FS_EVENT_RECURSIVE;
#else
  (void)recursive;
#endif

  watcher->persistent = persistent;
  return ant_watch_start(
    uv_default_loop(),
    &watcher->handle.event,
    path, fs_watcher_on_event,
    watcher, flags, &watcher->path
  );
}

static int fs_watcher_start_poll(fs_watcher_t *watcher, const char *path, bool persistent, unsigned int interval_ms) {
  int rc = 0;

  if (!watcher || !path) return UV_EINVAL;

  watcher->path = ant_watch_resolve_path(path);
  if (!watcher->path) return UV_ENOMEM;

  rc = uv_fs_poll_init(uv_default_loop(), &watcher->handle.poll);
  if (rc != 0) goto fail;

  watcher->handle.poll.data = watcher;
  rc = uv_fs_poll_start(&watcher->handle.poll, fs_watcher_on_poll, watcher->path, interval_ms);
  if (rc != 0) goto fail;

  watcher->persistent = persistent;
  return 0;

fail:
  free(watcher->path);
  watcher->path = NULL;
  return rc;
}

static ant_value_t fs_watcher_make_object(ant_t *js, fs_watcher_t *watcher) {
  ant_value_t obj = 0;

  if (!watcher) return js_mkerr(js, "Out of memory");
  fs_init_watch_constructors(js);

  obj = js_mkobj(js);
  js_set_proto_init(obj, g_fswatcher_proto);
  js_set_native_ptr(obj, watcher);
  js_set_native_tag(obj, FS_WATCHER_NATIVE_TAG);
  js_set_finalizer(obj, fs_watcher_finalize);
  watcher->obj = obj;
  
  return obj;
}

static void fs_request_fail(fs_request_t *req, int uv_code) {
  req->failed = 1;
  req->error_code = uv_code;
  if (req->error_msg) free(req->error_msg);
  req->error_msg = strdup(uv_strerror(uv_code));
}

static ant_value_t fs_coerce_path(ant_t *js, ant_value_t arg) {
  if (vtype(arg) == T_STR) return arg;
  if (is_object_type(arg)) {
    ant_value_t pathname = js_get(js, arg, "pathname");
    if (vtype(pathname) == T_STR) return pathname;
  }
  return js_mkundef();
}

static int fs_remove_path_recursive(const char *path, bool recursive, bool force) {
  uv_fs_t req;
  
  int result = uv_fs_lstat(NULL, &req, path, NULL);
  if (result < 0) {
    uv_fs_req_cleanup(&req);
    return (force && result == UV_ENOENT) ? 0 : result;
  }

  uv_stat_t statbuf = req.statbuf;
  uv_fs_req_cleanup(&req);

  if ((statbuf.st_mode & S_IFMT) != S_IFDIR) {
    result = uv_fs_unlink(NULL, &req, path, NULL);
    uv_fs_req_cleanup(&req);
    return (force && result == UV_ENOENT) ? 0 : result;
  }

  if (!recursive) return UV_EISDIR;
  result = uv_fs_scandir(NULL, &req, path, 0, NULL);
  
  if (result < 0) {
    uv_fs_req_cleanup(&req);
    return (force && result == UV_ENOENT) ? 0 : result;
  }

  for (;;) {
    uv_dirent_t entry;
    result = uv_fs_scandir_next(&req, &entry);
    if (result == UV_EOF) break;
    if (result < 0) {
      uv_fs_req_cleanup(&req);
      return result;
    }
    
    size_t path_len = strlen(path);
    size_t name_len = strlen(entry.name);
    
    bool needs_sep = path_len > 0 && path[path_len - 1] != '/';
    char *child = malloc(path_len + (needs_sep ? 1u : 0u) + name_len + 1u);
    if (!child) {
      uv_fs_req_cleanup(&req);
      return UV_ENOMEM;
    }
    
    memcpy(child, path, path_len);
    if (needs_sep) child[path_len++] = '/';
    memcpy(child + path_len, entry.name, name_len + 1u);
    
    result = fs_remove_path_recursive(child, true, force);
    free(child);
    if (result < 0) {
      uv_fs_req_cleanup(&req);
      return result;
    }
  }

  uv_fs_req_cleanup(&req);
  result = uv_fs_rmdir(NULL, &req, path, NULL);
  uv_fs_req_cleanup(&req);
  return (force && result == UV_ENOENT) ? 0 : result;
}

static bool fs_parse_rm_options(ant_t *js, ant_value_t options, bool *recursive_out, bool *force_out) {
  if (recursive_out) *recursive_out = false;
  if (force_out) *force_out = false;

  if (vtype(options) == T_UNDEF || vtype(options) == T_NULL) return true;
  if (vtype(options) != T_OBJ) return false;

  if (recursive_out) *recursive_out = js_truthy(js, js_get(js, options, "recursive"));
  if (force_out) *force_out = js_truthy(js, js_get(js, options, "force"));
  return true;
}

static ant_value_t fs_rm_impl(ant_t *js, ant_value_t *args, int nargs, bool return_promise) {
  ant_value_t promise = 0;
  ant_value_t path_val;
  size_t path_len = 0;
  const char *path = NULL;
  bool recursive = false;
  bool force = false;
  int result;

  if (return_promise) promise = js_mkpromise(js);
  if (nargs < 1) {
    ant_value_t err = js_mkerr(js, "rm() requires a path argument");
    if (!return_promise) return err;
    js_reject_promise(js, promise, err);
    return promise;
  }

  path_val = fs_coerce_path(js, args[0]);
  if (vtype(path_val) != T_STR) {
    ant_value_t err = js_mkerr(js, "rm() path must be a string");
    if (!return_promise) return err;
    js_reject_promise(js, promise, err);
    return promise;
  }

  path = js_getstr(js, path_val, &path_len);
  if (!path) {
    ant_value_t err = js_mkerr(js, "Failed to get path string");
    if (!return_promise) return err;
    js_reject_promise(js, promise, err);
    return promise;
  }

  if (!fs_parse_rm_options(js, nargs >= 2 ? args[1] : js_mkundef(), &recursive, &force)) {
    ant_value_t err = js_mkerr_typed(js, JS_ERR_TYPE, "rm() options must be an object");
    if (!return_promise) return err;
    js_reject_promise(js, promise, err);
    return promise;
  }

  result = fs_remove_path_recursive(path, recursive, force);
  if (result < 0) {
    ant_value_t err = js_mkerr(js, "%s", uv_strerror(result));
    if (!return_promise) return err;
    js_reject_promise(js, promise, err);
    return promise;
  }

  if (!return_promise) return js_mkundef();
  js_resolve_promise(js, promise, js_mkundef());
  return promise;
}

static fs_encoding_t parse_encoding(ant_t *js, ant_value_t arg) {
  size_t len;
  const char *str = NULL;

  if (vtype(arg) == T_STR) str = js_getstr(js, arg, &len); 
  else if (vtype(arg) == T_OBJ) {
    ant_value_t enc = js_get(js, arg, "encoding");
    if (vtype(enc) == T_STR) str = js_getstr(js, enc, &len);
  }

  if (!str) return FS_ENC_NONE;

  if (len == 4 && memcmp(str, "utf8", 4) == 0)      return FS_ENC_UTF8;
  if (len == 5 && memcmp(str, "utf-8", 5) == 0)     return FS_ENC_UTF8;
  if (len == 7 && memcmp(str, "utf16le", 7) == 0)   return FS_ENC_UTF16LE;
  if (len == 4 && memcmp(str, "ucs2", 4) == 0)      return FS_ENC_UTF16LE;
  if (len == 5 && memcmp(str, "ucs-2", 5) == 0)     return FS_ENC_UTF16LE;
  if (len == 6 && memcmp(str, "latin1", 6) == 0)    return FS_ENC_LATIN1;
  if (len == 6 && memcmp(str, "binary", 6) == 0)    return FS_ENC_LATIN1;
  if (len == 6 && memcmp(str, "base64", 6) == 0)    return FS_ENC_BASE64;
  if (len == 9 && memcmp(str, "base64url", 9) == 0) return FS_ENC_BASE64URL;
  if (len == 3 && memcmp(str, "hex", 3) == 0)       return FS_ENC_HEX;
  if (len == 5 && memcmp(str, "ascii", 5) == 0)     return FS_ENC_ASCII;

  return FS_ENC_NONE;
}

static ant_value_t encode_data(ant_t *js, const char *data, size_t len, fs_encoding_t enc) {
  switch (enc) {
    case FS_ENC_UTF8:
    case FS_ENC_LATIN1:
    case FS_ENC_ASCII: return js_mkstr(js, data, len);

    case FS_ENC_HEX: {
      char *out = malloc(len * 2);
      if (!out) return js_mkerr(js, "Out of memory");
      for (size_t i = 0; i < len; i++) {
        out[i * 2] = hex_char((unsigned char)data[i] >> 4);
        out[i * 2 + 1] = hex_char(data[i]);
      }
      ant_value_t result = js_mkstr(js, out, len * 2);
      free(out); return result;
    }

    case FS_ENC_BASE64: {
      size_t out_len;
      char *out = ant_base64_encode((const uint8_t *)data, len, &out_len);
      if (!out) return js_mkerr(js, "Out of memory");
      ant_value_t result = js_mkstr(js, out, out_len);
      free(out); return result;
    }

    case FS_ENC_BASE64URL: {
      size_t out_len;
      char *out = ant_base64_encode((const uint8_t *)data, len, &out_len);
      if (!out) return js_mkerr(js, "Out of memory");
      for (size_t i = 0; i < out_len; i++) {
        if (out[i] == '+') out[i] = '-';
        else if (out[i] == '/') out[i] = '_';
      }
      while (out_len > 0 && out[out_len - 1] == '=') out_len--;
      ant_value_t result = js_mkstr(js, out, out_len);
      free(out); return result;
    }

    case FS_ENC_UTF16LE: {
      const unsigned char *s = (const unsigned char *)data;
      char *out = malloc((len / 2) * 4);
      if (!out) return js_mkerr(js, "Out of memory");
      size_t j = 0;
      for (size_t i = 0; i + 1 < len; i += 2) {
        uint32_t cp = s[i] | (s[i + 1] << 8);
        bool is_hi = cp >= 0xD800 && cp <= 0xDBFF && i + 3 < len;
        uint32_t lo = is_hi ? s[i + 2] | (s[i + 3] << 8) : 0;
        bool is_pair = is_hi && lo >= 0xDC00 && lo <= 0xDFFF;
        if (is_pair) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
        j += utf8_encode(cp, out + j);
      }
      ant_value_t result = js_mkstr(js, out, j);
      free(out); return result;
    }

    default: return js_mkstr(js, data, len);
  }
}

static void free_fs_request(fs_request_t *req) {
  if (!req) return;

  if (req->path) free(req->path);
  if (req->path2) free(req->path2);
  if (req->data) free(req->data);
  if (req->error_msg) free(req->error_msg);
  
  uv_fs_req_cleanup(&req->uv_req);
  free(req);
}

static void remove_pending_request(fs_request_t *req) {
  if (!req || !pending_requests) return;
  unsigned int len = utarray_len(pending_requests);
  
  for (unsigned int i = 0; i < len; i++) {
    if (*(fs_request_t**)utarray_eltptr(pending_requests, i) != req) continue;
    utarray_erase(pending_requests, i, 1); break;
  }
}

static ant_value_t fs_read_to_uint8array(ant_t *js, const char *data, size_t len) {
  ArrayBufferData *ab = create_array_buffer_data(len);
  if (!ab) return js_mkundef();
  memcpy(ab->data, data, len);
  ant_value_t result = create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, len, "Buffer");
  if (vtype(result) == T_ERR) free_array_buffer_data(ab);
  return result;
}

static void complete_request(fs_request_t *req) {
  if (req->failed) {
    const char *err_msg = req->error_msg 
      ? req->error_msg 
      : "Unknown error";
      
    ant_value_t props = js_mkobj(req->js);
    ant_value_t reject_value;

    if (req->error_code) {
      const char *code = uv_err_name(req->error_code);
      js_set(req->js, props, "code", js_mkstr(req->js, code, strlen(code)));
      js_set(req->js, props, "errno", js_mknum((double)req->error_code));
      if (req->path) js_set(req->js, props, "path", js_mkstr(req->js, req->path, strlen(req->path)));
      if (req->path2) js_set(req->js, props, "dest", js_mkstr(req->js, req->path2, strlen(req->path2)));
      reject_value = js_mkerr_props(req->js, JS_ERR_TYPE, props, "%s", err_msg);
    } else reject_value = js_mkerr(req->js, "%s", err_msg);

    if (is_err(reject_value)) {
      reject_value = req->js->thrown_value;
      if (vtype(reject_value) == T_UNDEF)
        reject_value = js_mkstr(req->js, err_msg, strlen(err_msg));
      req->js->thrown_exists = false;
      req->js->thrown_value = js_mkundef();
    }
    js_reject_promise(req->js, req->promise, reject_value);
  } else {
    ant_value_t result = js_mkundef();
    if (req->op_type == FS_OP_READ && req->data) {
      if (req->encoding != FS_ENC_NONE)
        result = encode_data(req->js, req->data, req->data_len, req->encoding);
      else result = fs_read_to_uint8array(req->js, req->data, req->data_len);
    } else if (req->op_type == FS_OP_READ_BYTES && req->data)
      result = js_mkstr(req->js, req->data, req->data_len);
    else if (req->op_type == FS_OP_REALPATH && req->data)
      result = js_mkstr(req->js, req->data, req->data_len);
    else if (req->op_type == FS_OP_MKDTEMP && req->data)
      result = js_mkstr(req->js, req->data, req->data_len);
    js_resolve_promise(req->js, req->promise, result);
  }
  
  remove_pending_request(req);
  free_fs_request(req);
}

static void on_read_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;
  
  if (uv_req->result < 0) {
    fs_request_fail(req, (int)uv_req->result);
    req->completed = 1;
    complete_request(req);
    return;
  }
  
  req->data_len = uv_req->result;
  uv_fs_t close_req;
  
  uv_fs_close(uv_default_loop(), &close_req, req->fd, NULL);
  uv_fs_req_cleanup(&close_req);
  
  req->completed = 1;
  complete_request(req);
}

static void on_open_for_read(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;
  
  if (uv_req->result < 0) {
    fs_request_fail(req, (int)uv_req->result);
    req->completed = 1;
    complete_request(req);
    return;
  }
  
  req->fd = (int)uv_req->result;
  uv_fs_req_cleanup(uv_req);
  
  uv_fs_t stat_req;
  int stat_result = uv_fs_fstat(uv_default_loop(), &stat_req, req->fd, NULL);
  
  if (stat_result < 0) {
    fs_request_fail(req, stat_result);
    req->completed = 1;
    uv_fs_t close_req;
    uv_fs_close(uv_default_loop(), &close_req, req->fd, NULL);
    uv_fs_req_cleanup(&close_req);
    complete_request(req);
    return;
  }
  
  size_t file_size = stat_req.statbuf.st_size;
  uv_fs_req_cleanup(&stat_req);
  
  size_t alloc_size = (req->op_type == FS_OP_READ) ? file_size + 1 : file_size;
  req->data = malloc(alloc_size);
  if (!req->data) {
    req->failed = 1;
    req->error_msg = strdup("Out of memory");
    req->completed = 1;
    uv_fs_t close_req;
    uv_fs_close(uv_default_loop(), &close_req, req->fd, NULL);
    uv_fs_req_cleanup(&close_req);
    complete_request(req);
    return;
  }
  
  uv_buf_t buf = uv_buf_init(req->data, (unsigned int)file_size);
  int read_result = uv_fs_read(uv_default_loop(), uv_req, req->fd, &buf, 1, 0, on_read_complete);
  
  if (read_result < 0) {
    fs_request_fail(req, read_result);
    req->completed = 1;
    uv_fs_t close_req;
    uv_fs_close(uv_default_loop(), &close_req, req->fd, NULL);
    uv_fs_req_cleanup(&close_req);
    complete_request(req);
    return;
  }
}

static void on_write_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;
  
  if (uv_req->result < 0) {
    fs_request_fail(req, (int)uv_req->result);
  }
  
  uv_fs_t close_req;
  uv_fs_close(uv_default_loop(), &close_req, req->fd, NULL);
  uv_fs_req_cleanup(&close_req);
  
  req->completed = 1;
  complete_request(req);
}

static void on_open_for_write(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;
  
  if (uv_req->result < 0) {
    fs_request_fail(req, (int)uv_req->result);
    req->completed = 1;
    complete_request(req);
    return;
  }
  
  req->fd = (int)uv_req->result;
  uv_fs_req_cleanup(uv_req);
  
  uv_buf_t buf = uv_buf_init(req->data, (unsigned int)req->data_len);
  int write_result = uv_fs_write(uv_default_loop(), uv_req, req->fd, &buf, 1, 0, on_write_complete);
  
  if (write_result < 0) {
    fs_request_fail(req, write_result);
    req->completed = 1;
    uv_fs_t close_req;
    uv_fs_close(uv_default_loop(), &close_req, req->fd, NULL);
    uv_fs_req_cleanup(&close_req);
    complete_request(req);
    return;
  }
}

static void on_unlink_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;
  
  if (uv_req->result < 0) {
    fs_request_fail(req, (int)uv_req->result);
  }
  
  uv_fs_req_cleanup(uv_req);
  req->completed = 1;
  complete_request(req);
}

static void on_rename_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;

  if (uv_req->result < 0) {
    fs_request_fail(req, (int)uv_req->result);
  }

  uv_fs_req_cleanup(uv_req);
  req->completed = 1;
  complete_request(req);
}

static int mkdirp(const char *path, mode_t mode) {
  int result = -1;
  char *tmp = strdup(path);
  if (!tmp) goto done;

  size_t len = strlen(tmp);
  if (len > 0 && tmp[len - 1] == '/') tmp[len - 1] = '\0';

  for (char *p = tmp + 1; *p; p++) {
    if (*p != '/') continue;
    *p = '\0';
#ifdef _WIN32
    _mkdir(tmp);
#else
    mkdir(tmp, mode);
#endif
    *p = '/';
  }

#ifdef _WIN32
  result = _mkdir(tmp);
#else
  result = mkdir(tmp, mode);
#endif
  if (result != 0 && errno == EEXIST) result = 0;

  free(tmp);
done:
  return result;
}

static void on_mkdir_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;
  
  if (uv_req->result < 0) {
  if (!req->recursive || uv_req->result != UV_EEXIST) {
    fs_request_fail(req, (int)uv_req->result);
  }}
  
  uv_fs_req_cleanup(uv_req);
  req->completed = 1;
  complete_request(req);
}

static void on_rmdir_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;
  
  if (uv_req->result < 0) {
    fs_request_fail(req, (int)uv_req->result);
  }
  
  uv_fs_req_cleanup(uv_req);
  req->completed = 1;
  complete_request(req);
}

static void on_stat_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;
  
  if (uv_req->result < 0) {
    fs_request_fail(req, (int)uv_req->result);
    req->completed = 1;
    complete_request(req);
    return;
  }
  
  ant_value_t stat_obj = fs_stats_object_from_uv(req->js, &uv_req->statbuf);
  
  req->completed = 1;
  js_resolve_promise(req->js, req->promise, stat_obj);
  remove_pending_request(req);
  free_fs_request(req);
}

static void on_realpath_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;

  if (uv_req->result < 0 || !uv_req->ptr) {
    fs_request_fail(req, (int)uv_req->result);
    req->completed = 1;
    complete_request(req);
    return;
  }

  req->data = strdup((const char *)uv_req->ptr);
  if (!req->data) {
    req->failed = 1;
    req->error_msg = strdup("Out of memory");
    req->completed = 1;
    complete_request(req);
    return;
  }

  req->data_len = strlen(req->data);
  req->completed = 1;
  complete_request(req);
}

static ant_value_t create_dirent_object(ant_t *js, const char *name, size_t name_len, uv_dirent_type_t type) {
  ant_value_t obj = js_newobj(js);
  js_set_proto(obj, g_dirent_proto);
  js_set(js, obj, "name", js_mkstr(js, name, name_len));
  js_set_slot(obj, SLOT_DATA, tov((double)type));
  return obj;
}

static void on_mkdtemp_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;

  if (uv_req->result < 0) {
    fs_request_fail(req, (int)uv_req->result);
    req->completed = 1;
    complete_request(req);
    uv_fs_req_cleanup(uv_req);
    return;
  }

  req->data = strdup(uv_req->path);
  if (!req->data) {
    req->failed = 1;
    req->error_msg = strdup("Out of memory");
    req->completed = 1;
    complete_request(req);
    uv_fs_req_cleanup(uv_req);
    return;
  }

  req->data_len = strlen(req->data);
  req->completed = 1;
  uv_fs_req_cleanup(uv_req);
  complete_request(req);
}

static void on_exists_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;
  ant_value_t result = js_bool(uv_req->result >= 0);
  
  req->completed = 1;
  js_resolve_promise(req->js, req->promise, result);
  remove_pending_request(req);
  free_fs_request(req);
}

static void on_access_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;
  
  if (uv_req->result < 0) {
    fs_request_fail(req, (int)uv_req->result);
    req->completed = 1;
    complete_request(req);
    return;
  }
  
  req->completed = 1;
  js_resolve_promise(req->js, req->promise, js_mkundef());
  remove_pending_request(req);
  free_fs_request(req);
}

static void on_chmod_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;

  if (uv_req->result < 0) {
    fs_request_fail(req, (int)uv_req->result);
    req->completed = 1;
    complete_request(req);
    return;
  }

  req->completed = 1;
  js_resolve_promise(req->js, req->promise, js_mkundef());
  remove_pending_request(req);
  free_fs_request(req);
}

static void on_readdir_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;
  
  if (uv_req->result < 0) {
    fs_request_fail(req, (int)uv_req->result);
    req->completed = 1;
    complete_request(req);
    return;
  }
  
  ant_value_t arr = js_mkarr(req->js);
  uv_dirent_t dirent;
  
  while (uv_fs_scandir_next(uv_req, &dirent) != UV_EOF) {
  if (req->with_file_types) {
    ant_value_t entry = create_dirent_object(req->js, dirent.name, strlen(dirent.name), dirent.type);
    js_arr_push(req->js, arr, entry);
  } else {
    ant_value_t name = js_mkstr(req->js, dirent.name, strlen(dirent.name));
    js_arr_push(req->js, arr, name);
  }}
  
  req->completed = 1;
  js_resolve_promise(req->js, req->promise, arr);
  remove_pending_request(req);
  free_fs_request(req);
}

static ant_value_t builtin_fs_readFileSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "readFileSync() requires a path argument");
  
  ant_value_t path_val = fs_coerce_path(js, args[0]);
  if (vtype(path_val) != T_STR) return js_mkerr(js, "readFileSync() path must be a string or URL");
  
  size_t path_len;
  char *path = js_getstr(js, path_val, &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  char *path_cstr = strndup(path, path_len);
  if (!path_cstr) return js_mkerr(js, "Out of memory");
  
  FILE *file = fopen(path_cstr, "rb");
  if (!file) {
    ant_value_t err = fs_mk_errno_error(js, errno, "open", path_cstr, NULL);
    free(path_cstr);
    return err;
  }
  
  fseek(file, 0, SEEK_END);
  long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);
  
  if (file_size < 0) {
    fclose(file);
    free(path_cstr);
    return js_mkerr(js, "Failed to get file size");
  }
  
  char *data = malloc(file_size + 1);
  if (!data) {
    fclose(file);
    free(path_cstr);
    return js_mkerr(js, "Out of memory");
  }
  
  size_t bytes_read = fread(data, 1, file_size, file);
  fclose(file);
  free(path_cstr);
  
  if (bytes_read != (size_t)file_size) {
    free(data);
    return js_mkerr(js, "Failed to read entire file");
  }
  
  fs_encoding_t enc = (nargs > 1) ? parse_encoding(js, args[1]) : FS_ENC_NONE;
  ant_value_t result = (enc != FS_ENC_NONE)
    ? encode_data(js, data, file_size, enc)
    : fs_read_to_uint8array(js, data, file_size);
    
  free(data);
  return result;
}

static ant_value_t builtin_fs_readBytesSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "readBytesSync() requires a path argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "readBytesSync() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  char *path_cstr = strndup(path, path_len);
  if (!path_cstr) return js_mkerr(js, "Out of memory");
  
  FILE *file = fopen(path_cstr, "rb");
  if (!file) {
    ant_value_t err = fs_mk_errno_error(js, errno, "open", path_cstr, NULL);
    free(path_cstr);
    return err;
  }
  
  fseek(file, 0, SEEK_END);
  long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);
  
  if (file_size < 0) {
    fclose(file);
    free(path_cstr);
    return js_mkerr(js, "Failed to get file size");
  }
  
  char *data = malloc(file_size);
  if (!data) {
    fclose(file);
    free(path_cstr);
    return js_mkerr(js, "Out of memory");
  }
  
  size_t bytes_read = fread(data, 1, file_size, file);
  fclose(file);
  free(path_cstr);
  
  if (bytes_read != (size_t)file_size) {
    free(data);
    return js_mkerr(js, "Failed to read entire file");
  }
  
  ant_value_t result = js_mkstr(js, data, file_size);
  free(data);
  
  return result;
}

static ant_value_t builtin_fs_readFile(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "readFile() requires a path argument");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "readFile() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  
  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");
  
  req->js = js;
  req->op_type = FS_OP_READ;
  req->encoding = (nargs > 1) ? parse_encoding(js, args[1]) : FS_ENC_NONE;
  req->promise = js_mkpromise(js);
  req->path = strndup(path, path_len);
  req->uv_req.data = req;
  
  utarray_push_back(pending_requests, &req);
  int result = uv_fs_open(uv_default_loop(), &req->uv_req, req->path, O_RDONLY, 0, on_open_for_read);
  
  if (result < 0) {
    fs_request_fail(req, result);
    req->completed = 1;
    complete_request(req);
  }
  
  return req->promise;
}

static ant_value_t builtin_fs_readBytes(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "readBytes() requires a path argument");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "readBytes() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  
  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");
  
  req->js = js;
  req->op_type = FS_OP_READ_BYTES;
  req->promise = js_mkpromise(js);
  req->path = strndup(path, path_len);
  req->uv_req.data = req;
  
  utarray_push_back(pending_requests, &req);
  int result = uv_fs_open(uv_default_loop(), &req->uv_req, req->path, O_RDONLY, 0, on_open_for_read);
  
  if (result < 0) {
    fs_request_fail(req, result);
    req->completed = 1;
    complete_request(req);
  }
  
  return req->promise;
}

static ant_value_t fs_write_file_sync_impl(
  ant_t *js,
  ant_value_t *args,
  int nargs,
  const char *fn_name,
  const char *mode
) {
  if (nargs < 2) return js_mkerr(js, "%s() requires path and data arguments", fn_name);
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "%s() path must be a string", fn_name);
  if (vtype(args[1]) != T_STR) return js_mkerr(js, "%s() data must be a string", fn_name);

  size_t path_len, data_len;
  char *path = js_getstr(js, args[0], &path_len);
  char *data = js_getstr(js, args[1], &data_len);
  if (!path || !data) return js_mkerr(js, "Failed to get arguments");

  char *path_cstr = strndup(path, path_len);
  if (!path_cstr) return js_mkerr(js, "Out of memory");

  FILE *file = fopen(path_cstr, mode);
  if (!file) {
    ant_value_t err = fs_mk_errno_error(js, errno, "open", path_cstr, NULL);
    free(path_cstr);
    return err;
  }

  size_t bytes_written = fwrite(data, 1, data_len, file);
  fclose(file);
  free(path_cstr);

  if (bytes_written != data_len) {
    return js_mkerr(js, "Failed to write entire file");
  }

  return js_mkundef();
}

static ant_value_t builtin_fs_writeFileSync(ant_t *js, ant_value_t *args, int nargs) {
  return fs_write_file_sync_impl(js, args, nargs, "writeFileSync", "wb");
}

static ant_value_t builtin_fs_copyFileSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "copyFileSync() requires src and dest arguments");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "copyFileSync() src must be a string");
  if (vtype(args[1]) != T_STR) return js_mkerr(js, "copyFileSync() dest must be a string");
  
  size_t src_len, dest_len;
  char *src = js_getstr(js, args[0], &src_len);
  char *dest = js_getstr(js, args[1], &dest_len);
  
  if (!src || !dest) return js_mkerr(js, "Failed to get arguments");
  
  char *src_cstr = strndup(src, src_len);
  char *dest_cstr = strndup(dest, dest_len);
  if (!src_cstr || !dest_cstr) {
    free(src_cstr);
    free(dest_cstr);
    return js_mkerr(js, "Out of memory");
  }
  
  FILE *in = fopen(src_cstr, "rb");
  if (!in) {
    ant_value_t err = fs_mk_errno_error(js, errno, "copyfile", src_cstr, dest_cstr);
    free(src_cstr); free(dest_cstr);
    return err;  
  }
  
  FILE *out = fopen(dest_cstr, "wb");
  if (!out) {
    ant_value_t err = fs_mk_errno_error(js, errno, "copyfile", src_cstr, dest_cstr);
    fclose(in);
    free(src_cstr); free(dest_cstr);
    return err;
  }
  
  char buf[8192];
  size_t n;
  
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
  if (fwrite(buf, 1, n, out) != n) {
    ant_value_t err = fs_mk_errno_error(js, errno ? errno : EIO, "copyfile", src_cstr, dest_cstr);
    fclose(in); fclose(out);
    free(src_cstr); free(dest_cstr);
    return err;
  }}
  
  fclose(in); fclose(out);
  free(src_cstr); free(dest_cstr);

  return js_mkundef();
}

typedef struct {
  bool recursive;
  bool force;
  bool error_on_exist;
} fs_cp_options_t;

static void fs_parse_cp_options(ant_t *js, ant_value_t value, fs_cp_options_t *opts) {
  opts->recursive = false;
  opts->force = true;
  opts->error_on_exist = false;

  if (vtype(value) != T_OBJ) return;

  ant_value_t recursive = js_get(js, value, "recursive");
  ant_value_t force = js_get(js, value, "force");
  ant_value_t error_on_exist = js_get(js, value, "errorOnExist");

  if (!is_undefined(recursive)) opts->recursive = js_truthy(js, recursive);
  if (!is_undefined(force)) opts->force = js_truthy(js, force);
  if (!is_undefined(error_on_exist)) opts->error_on_exist = js_truthy(js, error_on_exist);
}

static char *fs_join_path(const char *base, const char *name) {
  size_t base_len = strlen(base);
  size_t name_len = strlen(name);
  bool need_sep = base_len > 0 && base[base_len - 1] != '/';
  size_t total_len = base_len + (need_sep ? 1 : 0) + name_len;

  char *joined = calloc(total_len + 1, 1);
  if (!joined) return NULL;

  memcpy(joined, base, base_len);
  if (need_sep) joined[base_len++] = '/';
  memcpy(joined + base_len, name, name_len);
  joined[total_len] = '\0';
  return joined;
}

static ant_value_t fs_copy_file_sync_impl(
  ant_t *js,
  const char *src_cstr,
  const char *dest_cstr,
  const fs_cp_options_t *opts,
  const char *op_name
) {
  struct stat dest_st;
  if (!opts->force && stat(dest_cstr, &dest_st) == 0) {
    if (opts->error_on_exist) return fs_mk_errno_error(js, EEXIST, op_name, src_cstr, dest_cstr);
    return js_mkundef();
  }

  FILE *in = fopen(src_cstr, "rb");
  if (!in) return fs_mk_errno_error(js, errno, op_name, src_cstr, dest_cstr);

  FILE *out = fopen(dest_cstr, "wb");
  if (!out) {
    ant_value_t err = fs_mk_errno_error(js, errno, op_name, src_cstr, dest_cstr);
    fclose(in);
    return err;
  }

  char buf[8192];
  size_t n = 0;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
  if (fwrite(buf, 1, n, out) != n) {
    ant_value_t err = fs_mk_errno_error(js, errno ? errno : EIO, op_name, src_cstr, dest_cstr);
    fclose(in);
    fclose(out);
    return err;
  }}

  fclose(in);
  fclose(out);
  
  return js_mkundef();
}

static ant_value_t builtin_fs_copyFile(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "copyFile() requires src and dest arguments");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "copyFile() src must be a string");
  if (vtype(args[1]) != T_STR) return js_mkerr(js, "copyFile() dest must be a string");

  size_t src_len = 0, dest_len = 0;
  const char *src = js_getstr(js, args[0], &src_len);
  const char *dest = js_getstr(js, args[1], &dest_len);
  if (!src || !dest) return js_mkerr(js, "Failed to get arguments");

  char *src_cstr = strndup(src, src_len);
  char *dest_cstr = strndup(dest, dest_len);
  if (!src_cstr || !dest_cstr) {
    free(src_cstr);
    free(dest_cstr);
    return js_mkerr(js, "Out of memory");
  }

  fs_cp_options_t opts = {
    .recursive = false,
    .force = true,
    .error_on_exist = false,
  };

  ant_value_t promise = js_mkpromise(js);
  ant_value_t result = fs_copy_file_sync_impl(js, src_cstr, dest_cstr, &opts, "copyFile");
  free(src_cstr);
  free(dest_cstr);

  if (is_err(result)) js_reject_promise(js, promise, result);
  else js_resolve_promise(js, promise, js_mkundef());
  
  return promise;
}

static ant_value_t fs_copy_path_sync_impl(
  ant_t *js,
  const char *src_cstr,
  const char *dest_cstr,
  const fs_cp_options_t *opts,
  const char *op_name
) {
  struct stat src_st;
  if (stat(src_cstr, &src_st) != 0) return fs_mk_errno_error(js, errno, op_name, src_cstr, dest_cstr);

  if ((src_st.st_mode & S_IFMT) == S_IFDIR) {
    if (!opts->recursive) {
      return js_mkerr(js, "%s() recursive option is required to copy directories", op_name);
    }

    struct stat dest_st;
    if (stat(dest_cstr, &dest_st) != 0) {
      if (errno != ENOENT) return fs_mk_errno_error(js, errno, op_name, src_cstr, dest_cstr);
    #ifdef _WIN32
      if (_mkdir(dest_cstr) != 0) return fs_mk_errno_error(js, errno, op_name, src_cstr, dest_cstr);
    #else
      if (mkdir(dest_cstr, (mode_t)(src_st.st_mode & 0777)) != 0) {
        return fs_mk_errno_error(js, errno, op_name, src_cstr, dest_cstr);
      }
    #endif
    } else if ((dest_st.st_mode & S_IFMT) != S_IFDIR) {
      return fs_mk_errno_error(js, EEXIST, op_name, src_cstr, dest_cstr);
    }

    uv_fs_t req;
    int rc = uv_fs_scandir(NULL, &req, src_cstr, 0, NULL);
    if (rc < 0) {
      ant_value_t err = fs_mk_uv_error(js, rc, "scandir", src_cstr, dest_cstr);
      uv_fs_req_cleanup(&req);
      return err;
    }

    uv_dirent_t dirent;
    ant_value_t result = js_mkundef();
    while (uv_fs_scandir_next(&req, &dirent) != UV_EOF) {
      if (
        strcmp(dirent.name, ".") == 0 ||
        strcmp(dirent.name, "..") == 0
      ) continue;
      
      char *child_src = fs_join_path(src_cstr, dirent.name);
      char *child_dest = fs_join_path(dest_cstr, dirent.name);
      if (!child_src || !child_dest) {
        free(child_src);
        free(child_dest);
        result = js_mkerr(js, "Out of memory");
        break;
      }
      
      result = fs_copy_path_sync_impl(js, child_src, child_dest, opts, op_name);
      free(child_src);
      free(child_dest);
      if (is_err(result)) break;
    }
    
    uv_fs_req_cleanup(&req);
    return result;
  }

  return fs_copy_file_sync_impl(js, src_cstr, dest_cstr, opts, op_name);
}

static ant_value_t fs_cp_sync_common(
  ant_t *js,
  ant_value_t *args,
  int nargs,
  const char *fn_name
) {
  if (nargs < 2) return js_mkerr(js, "%s() requires src and dest arguments", fn_name);
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "%s() src must be a string", fn_name);
  if (vtype(args[1]) != T_STR) return js_mkerr(js, "%s() dest must be a string", fn_name);

  size_t src_len = 0, dest_len = 0;
  const char *src = js_getstr(js, args[0], &src_len);
  const char *dest = js_getstr(js, args[1], &dest_len);
  if (!src || !dest) return js_mkerr(js, "Failed to get arguments");

  fs_cp_options_t opts;
  fs_parse_cp_options(js, nargs >= 3 ? args[2] : js_mkundef(), &opts);

  char *src_cstr = strndup(src, src_len);
  char *dest_cstr = strndup(dest, dest_len);
  if (!src_cstr || !dest_cstr) {
    free(src_cstr);
    free(dest_cstr);
    return js_mkerr(js, "Out of memory");
  }

  ant_value_t result = fs_copy_path_sync_impl(js, src_cstr, dest_cstr, &opts, fn_name);
  free(src_cstr);
  free(dest_cstr);
  return result;
}

static ant_value_t builtin_fs_cpSync(ant_t *js, ant_value_t *args, int nargs) {
  return fs_cp_sync_common(js, args, nargs, "cpSync");
}

static ant_value_t builtin_fs_cp(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t promise = js_mkpromise(js);
  ant_value_t result = fs_cp_sync_common(js, args, nargs, "cp");
  if (is_err(result)) js_reject_promise(js, promise, result);
  else js_resolve_promise(js, promise, js_mkundef());
  return promise;
}

static ant_value_t builtin_fs_renameSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "renameSync() requires oldPath and newPath arguments");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "renameSync() oldPath must be a string");
  if (vtype(args[1]) != T_STR) return js_mkerr(js, "renameSync() newPath must be a string");
  
  size_t old_len, new_len;
  char *old_path = js_getstr(js, args[0], &old_len);
  char *new_path = js_getstr(js, args[1], &new_len);
  
  if (!old_path || !new_path) return js_mkerr(js, "Failed to get arguments");
  
  char *old_cstr = strndup(old_path, old_len);
  char *new_cstr = strndup(new_path, new_len);
  if (!old_cstr || !new_cstr) {
    free(old_cstr);
    free(new_cstr);
    return js_mkerr(js, "Out of memory");
  }
  
  int result = rename(old_cstr, new_cstr);
  
  if (result != 0) {
    ant_value_t err = fs_mk_errno_error(js, errno, "rename", old_cstr, new_cstr);
    free(old_cstr);
    free(new_cstr);
    return err;
  }
  
  free(old_cstr);
  free(new_cstr);
  
  return js_mkundef();
}

static ant_value_t builtin_fs_rename(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "rename() requires oldPath and newPath arguments");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "rename() oldPath must be a string");
  if (vtype(args[1]) != T_STR) return js_mkerr(js, "rename() newPath must be a string");

  size_t old_len = 0, new_len = 0;
  const char *old_path = js_getstr(js, args[0], &old_len);
  const char *new_path = js_getstr(js, args[1], &new_len);
  if (!old_path || !new_path) return js_mkerr(js, "Failed to get arguments");

  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");

  req->js = js;
  req->op_type = FS_OP_RENAME;
  req->promise = js_mkpromise(js);
  req->path = strndup(old_path, old_len);
  req->path2 = strndup(new_path, new_len);
  req->uv_req.data = req;

  if (!req->path || !req->path2) {
    free_fs_request(req);
    return js_mkerr(js, "Out of memory");
  }

  utarray_push_back(pending_requests, &req);
  int result = uv_fs_rename(uv_default_loop(), &req->uv_req, req->path, req->path2, on_rename_complete);

  if (result < 0) {
    fs_request_fail(req, result);
    req->completed = 1;
    complete_request(req);
  }

  return req->promise;
}

static double fs_time_arg_to_seconds(ant_t *js, ant_value_t v) {
  if (is_date_instance(v)) {
    ant_value_t t = js_get_slot(js_as_obj(v), SLOT_DATA);
    return (vtype(t) == T_NUM) ? js_getnum(t) / 1000.0 : 0.0;
  }
  return js_to_number(js, v);
}

static ant_value_t builtin_fs_utimesSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 3) return js_mkerr(js, "utimesSync() requires path, atime, and mtime");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "utimesSync() path must be a string");

  const char *path = js_str(js, args[0]);
  double atime = fs_time_arg_to_seconds(js, args[1]);
  double mtime = fs_time_arg_to_seconds(js, args[2]);

  uv_fs_t req;
  int rc = uv_fs_utime(NULL, &req, path, atime, mtime, NULL);
  uv_fs_req_cleanup(&req);

  if (rc < 0) return js_mkerr(js, "utimesSync: %s", uv_strerror(rc));
  return js_mkundef();
}

static ant_value_t builtin_fs_utimes(ant_t *js, ant_value_t *args, int nargs) {
  return builtin_fs_utimesSync(js, args, nargs);
}

static ant_value_t builtin_fs_futimesSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 3) return js_mkerr(js, "futimesSync() requires fd, atime, and mtime");
  int fd = (int)js_to_number(js, args[0]);
  double atime = fs_time_arg_to_seconds(js, args[1]);
  double mtime = fs_time_arg_to_seconds(js, args[2]);

  uv_fs_t req;
  int rc = uv_fs_futime(NULL, &req, fd, atime, mtime, NULL);
  uv_fs_req_cleanup(&req);

  if (rc < 0) return js_mkerr(js, "futimesSync: %s", uv_strerror(rc));
  return js_mkundef();
}

static ant_value_t builtin_fs_futimes(ant_t *js, ant_value_t *args, int nargs) {
  return builtin_fs_futimesSync(js, args, nargs);
}

static ant_value_t builtin_fs_appendFileSync(ant_t *js, ant_value_t *args, int nargs) {
  return fs_write_file_sync_impl(js, args, nargs, "appendFileSync", "ab");
}

static ant_value_t builtin_fs_appendFile(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t promise = js_mkpromise(js);
  ant_value_t result = fs_write_file_sync_impl(js, args, nargs, "appendFile", "ab");
  if (is_err(result)) js_reject_promise(js, promise, result);
  else js_resolve_promise(js, promise, js_mkundef());
  return promise;
}

static ant_value_t builtin_fs_writeFile(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "writeFile() requires path and data arguments");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "writeFile() path must be a string");
  if (vtype(args[1]) != T_STR) return js_mkerr(js, "writeFile() data must be a string");
  
  size_t path_len, data_len;
  char *path = js_getstr(js, args[0], &path_len);
  char *data = js_getstr(js, args[1], &data_len);
  
  if (!path || !data) return js_mkerr(js, "Failed to get arguments");
  
  
  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");
  
  req->js = js;
  req->op_type = FS_OP_WRITE;
  req->promise = js_mkpromise(js);
  req->path = strndup(path, path_len);
  req->data = malloc(data_len);
  if (!req->data) {
    free(req->path);
    free(req);
    return js_mkerr(js, "Out of memory");
  }
  
  memcpy(req->data, data, data_len);
  req->data_len = data_len;
  req->uv_req.data = req;
  
  utarray_push_back(pending_requests, &req);
  int result = uv_fs_open(uv_default_loop(), &req->uv_req, req->path, O_WRONLY | O_CREAT | O_TRUNC, 0644, on_open_for_write);
  
  if (result < 0) {
    fs_request_fail(req, result);
    req->completed = 1;
    complete_request(req);
  }
  
  return req->promise;
}

static ant_value_t builtin_fs_unlinkSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "unlinkSync() requires a path argument");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "unlinkSync() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  char *path_cstr = strndup(path, path_len);
  if (!path_cstr) return js_mkerr(js, "Out of memory");
  int result = unlink(path_cstr);
  
  if (result != 0) {
    ant_value_t err = fs_mk_errno_error(js, errno, "unlink", path_cstr, NULL);
    free(path_cstr);
    return err;
  }
  
  free(path_cstr);
  return js_mkundef();
}

static ant_value_t builtin_fs_unlink(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "unlink() requires a path argument");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "unlink() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  
  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");
  
  req->js = js;
  req->op_type = FS_OP_UNLINK;
  req->promise = js_mkpromise(js);
  req->path = strndup(path, path_len);
  req->uv_req.data = req;
  
  utarray_push_back(pending_requests, &req);
  int result = uv_fs_unlink(uv_default_loop(), &req->uv_req, req->path, on_unlink_complete);
  
  if (result < 0) {
    fs_request_fail(req, result);
    req->completed = 1;
    complete_request(req);
  }
  
  return req->promise;
}

static ant_value_t builtin_fs_mkdirSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "mkdirSync() requires a path argument");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "mkdirSync() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  int mode = 0755;
  int recursive = 0;
  
  if (nargs < 2) goto do_mkdir;
  
  switch (vtype(args[1])) {
    case T_NUM:
      mode = (int)js_getnum(args[1]);
      break;
    case T_OBJ: {
      ant_value_t opt = args[1];
      recursive = js_get(js, opt, "recursive") == js_true;
      ant_value_t mode_val = js_get(js, opt, "mode");
      if (vtype(mode_val) == T_NUM) mode = (int)js_getnum(mode_val);
      break;
    }
  }
  
do_mkdir:
  
  char *path_cstr = strndup(path, path_len);
  if (!path_cstr) return js_mkerr(js, "Out of memory");
  
  int result;
  if (recursive) {
    result = mkdirp(path_cstr, (mode_t)mode);
  } else {
#ifdef _WIN32
    (void)mode;
    result = _mkdir(path_cstr);
#else
    result = mkdir(path_cstr, (mode_t)mode);
#endif
  }
  if (result != 0) {
    ant_value_t err = fs_mk_errno_error(js, errno, "mkdir", path_cstr, NULL);
    free(path_cstr);
    return err;
  }
  
  free(path_cstr);
  
  return js_mkundef();
}

static ant_value_t builtin_fs_mkdir(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "mkdir() requires a path argument");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "mkdir() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  int mode = 0755;
  int recursive = 0;
  if (nargs >= 2) {
  switch (vtype(args[1])) {
    case T_NUM:
      mode = (int)js_getnum(args[1]);
      break;
    case T_OBJ: {
      ant_value_t opt = args[1];
      recursive = js_get(js, opt, "recursive") == js_true;
      ant_value_t mode_val = js_get(js, opt, "mode");
      if (vtype(mode_val) == T_NUM) mode = (int)js_getnum(mode_val);
      break;
    }}
  }

  if (recursive) {
    ant_value_t promise = js_mkpromise(js);
    char *path_cstr = strndup(path, path_len);
    
    if (!path_cstr) return js_mkerr(js, "Out of memory");
    int result = mkdirp(path_cstr, (mode_t)mode);
    
    free(path_cstr);
    if (result != 0) {
      js_reject_promise(js, promise, js_mkerr(js, "Failed to create directory: %s", strerror(errno)));
    } else js_resolve_promise(js, promise, js_mkundef());
    
    return promise;
  }

  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");

  req->js = js;
  req->op_type = FS_OP_MKDIR;
  req->promise = js_mkpromise(js);
  req->path = strndup(path, path_len);
  req->recursive = recursive;
  req->uv_req.data = req;
  
  utarray_push_back(pending_requests, &req);
  int result = uv_fs_mkdir(uv_default_loop(), &req->uv_req, req->path, mode, on_mkdir_complete);
  
  if (result < 0) {
    fs_request_fail(req, result);
    req->completed = 1;
    complete_request(req);
  }
  
  return req->promise;
}

static ant_value_t builtin_fs_mkdtempSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_STR)
    return js_mkerr(js, "mkdtempSync() requires a prefix string");

  size_t prefix_len;
  const char *prefix = js_getstr(js, args[0], &prefix_len);
  if (!prefix) return js_mkerr(js, "Failed to get prefix string");

  size_t tpl_len = prefix_len + 6;
  char *tpl = malloc(tpl_len + 1);
  if (!tpl) return js_mkerr(js, "Out of memory");

  memcpy(tpl, prefix, prefix_len);
  memcpy(tpl + prefix_len, "XXXXXX", 6);
  tpl[tpl_len] = '\0';

  char *result = mkdtemp(tpl);
  if (!result) {
    free(tpl);
    return js_mkerr(js, "mkdtempSync failed: %s", strerror(errno));
  }

  ant_value_t ret = js_mkstr(js, result, strlen(result));
  free(tpl);
  return ret;
}

static ant_value_t builtin_fs_mkdtemp(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_STR)
    return js_mkerr(js, "mkdtemp() requires a prefix string");

  size_t prefix_len;
  const char *prefix = js_getstr(js, args[0], &prefix_len);
  if (!prefix) return js_mkerr(js, "Failed to get prefix string");

  size_t tpl_len = prefix_len + 6;
  char *tpl = malloc(tpl_len + 1);
  if (!tpl) return js_mkerr(js, "Out of memory");

  memcpy(tpl, prefix, prefix_len);
  memcpy(tpl + prefix_len, "XXXXXX", 6);
  tpl[tpl_len] = '\0';

  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) { free(tpl); return js_mkerr(js, "Out of memory"); }

  req->js = js;
  req->op_type = FS_OP_MKDTEMP;
  req->promise = js_mkpromise(js);
  req->path = tpl;
  req->uv_req.data = req;

  utarray_push_back(pending_requests, &req);
  int result = uv_fs_mkdtemp(uv_default_loop(), &req->uv_req, req->path, on_mkdtemp_complete);

  if (result < 0) {
    fs_request_fail(req, result);
    req->completed = 1;
    complete_request(req);
  }

  return req->promise;
}

static ant_value_t builtin_fs_rmdirSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "rmdirSync() requires a path argument");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "rmdirSync() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  char *path_cstr = strndup(path, path_len);
  if (!path_cstr) return js_mkerr(js, "Out of memory");
  
#ifdef _WIN32
  int result = _rmdir(path_cstr);
#else
  int result = rmdir(path_cstr);
#endif
  if (result != 0) {
    ant_value_t err = fs_mk_errno_error(js, errno, "rmdir", path_cstr, NULL);
    free(path_cstr);
    return err;
  }
  
  free(path_cstr);
  return js_mkundef();
}

static ant_value_t builtin_fs_rmdir(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "rmdir() requires a path argument");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "rmdir() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  
  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");
  
  req->js = js;
  req->op_type = FS_OP_RMDIR;
  req->promise = js_mkpromise(js);
  req->path = strndup(path, path_len);
  req->uv_req.data = req;
  
  utarray_push_back(pending_requests, &req);
  int result = uv_fs_rmdir(uv_default_loop(), &req->uv_req, req->path, on_rmdir_complete);
  
  if (result < 0) {
    fs_request_fail(req, result);
    req->completed = 1;
    complete_request(req);
  }
  
  return req->promise;
}

static ant_value_t builtin_fs_rmSync(ant_t *js, ant_value_t *args, int nargs) {
  return fs_rm_impl(js, args, nargs, false);
}

static ant_value_t builtin_fs_rm(ant_t *js, ant_value_t *args, int nargs) {
  return fs_rm_impl(js, args, nargs, true);
}

static ant_value_t dirent_isFile(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this = js_getthis(js);
  ant_value_t type_val = js_get_slot(this, SLOT_DATA);
  if (vtype(type_val) != T_NUM) return js_false;
  return js_bool((int)js_getnum(type_val) == UV_DIRENT_FILE);
}

static ant_value_t dirent_isDirectory(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this = js_getthis(js);
  ant_value_t type_val = js_get_slot(this, SLOT_DATA);
  if (vtype(type_val) != T_NUM) return js_false;
  return js_bool((int)js_getnum(type_val) == UV_DIRENT_DIR);
}

static ant_value_t dirent_isSymbolicLink(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this = js_getthis(js);
  ant_value_t type_val = js_get_slot(this, SLOT_DATA);
  if (vtype(type_val) != T_NUM) return js_false;
  return js_bool((int)js_getnum(type_val) == UV_DIRENT_LINK);
}

static ant_value_t dirent_isBlockDevice(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this = js_getthis(js);
  ant_value_t type_val = js_get_slot(this, SLOT_DATA);
  if (vtype(type_val) != T_NUM) return js_false;
  return js_bool((int)js_getnum(type_val) == UV_DIRENT_BLOCK);
}

static ant_value_t dirent_isCharacterDevice(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this = js_getthis(js);
  ant_value_t type_val = js_get_slot(this, SLOT_DATA);
  if (vtype(type_val) != T_NUM) return js_false;
  return js_bool((int)js_getnum(type_val) == UV_DIRENT_CHAR);
}

static ant_value_t dirent_isFIFO(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this = js_getthis(js);
  ant_value_t type_val = js_get_slot(this, SLOT_DATA);
  if (vtype(type_val) != T_NUM) return js_false;
  return js_bool((int)js_getnum(type_val) == UV_DIRENT_FIFO);
}

static ant_value_t dirent_isSocket(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this = js_getthis(js);
  ant_value_t type_val = js_get_slot(this, SLOT_DATA);
  if (vtype(type_val) != T_NUM) return js_false;
  return js_bool((int)js_getnum(type_val) == UV_DIRENT_SOCKET);
}

static ant_value_t stat_isFile(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this = js_getthis(js);
  ant_value_t mode_val = js_get_slot(this, SLOT_DATA);
  
  if (vtype(mode_val) != T_NUM) return js_false;
  mode_t mode = (mode_t)js_getnum(mode_val);
  
  return js_bool(S_ISREG(mode));
}

static ant_value_t stat_isDirectory(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this = js_getthis(js);
  ant_value_t mode_val = js_get_slot(this, SLOT_DATA);
  
  if (vtype(mode_val) != T_NUM) return js_false;
  mode_t mode = (mode_t)js_getnum(mode_val);
  
  return js_bool(S_ISDIR(mode));
}

static ant_value_t stat_isSymbolicLink(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this = js_getthis(js);
  ant_value_t mode_val = js_get_slot(this, SLOT_DATA);
  
  if (vtype(mode_val) != T_NUM) return js_false;
  mode_t mode = (mode_t)js_getnum(mode_val);
  
  return js_bool(S_ISLNK(mode));
}

static ant_value_t create_stats_object(ant_t *js, struct stat *st) {
  return fs_stats_object_from_posix(js, st);
}

static const char *errno_to_code(int err_num) {
switch (err_num) {
  case ENOENT: return "ENOENT";
  case EACCES: return "EACCES";
  case ENOTDIR: return "ENOTDIR";
  case ELOOP: return "ELOOP";
  case ENAMETOOLONG: return "ENAMETOOLONG";
  case EOVERFLOW: return "EOVERFLOW";
  case EROFS: return "EROFS";
  case ETXTBSY: return "ETXTBSY";
  case EEXIST: return "EEXIST";
  case ENOTEMPTY: return "ENOTEMPTY";
  case EISDIR: return "EISDIR";
  case EBUSY: return "EBUSY";
  case EINVAL: return "EINVAL";
  case EPERM: return "EPERM";
  case EIO: return "EIO";
  default: return "UNKNOWN";
}}

static ant_value_t fs_mk_syscall_error(
  ant_t *js, const char *code, double err_num,
  const char *message, const char *syscall,
  const char *path, const char *dest
) {
  ant_value_t props = js_mkobj(js);

  if (!code) code = "UNKNOWN";
  if (!message) message = "Unknown error";

  js_set(js, props, "code", js_mkstr(js, code, strlen(code)));
  js_set(js, props, "errno", js_mknum(err_num));
  
  if (syscall) js_set(js, props, "syscall", js_mkstr(js, syscall, strlen(syscall)));
  if (path) js_set(js, props, "path", js_mkstr(js, path, strlen(path)));
  if (dest) js_set(js, props, "dest", js_mkstr(js, dest, strlen(dest)));

  if (path && dest) return js_mkerr_props(
    js, JS_ERR_GENERIC,
    props, "%s: %s, %s '%s' -> '%s'",
    code, message, syscall, path, dest
  );
    
  if (path) return js_mkerr_props(
    js, JS_ERR_GENERIC,
    props, "%s: %s, %s '%s'",
    code, message, syscall, path
  );
  
  return js_mkerr_props(
    js, JS_ERR_GENERIC, 
    props, "%s: %s, %s",
    code, message, syscall
  );
}

static ant_value_t fs_mk_errno_error(
  ant_t *js, int err_num,
  const char *syscall, const char *path, const char *dest
) {
  int uv_code = uv_translate_sys_error(err_num);
  if (uv_code == 0) uv_code = -err_num;
  
  return fs_mk_syscall_error(
    js, errno_to_code(err_num),
    (double)uv_code,
    uv_strerror(uv_code),
    syscall, path, dest
  );
}

static ant_value_t fs_mk_uv_error(
  ant_t *js, int uv_code,
  const char *syscall, const char *path, const char *dest
) {
  return fs_mk_syscall_error(
    js, uv_err_name(uv_code),
    (double)uv_code,
    uv_strerror(uv_code),
    syscall, path, dest
  );
}

static ant_value_t builtin_fs_statSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "statSync() requires a path argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "statSync() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  char *path_cstr = strndup(path, path_len);
  if (!path_cstr) return js_mkerr(js, "Out of memory");
  
  struct stat st;
  int result = stat(path_cstr, &st);
  
  if (result != 0) {
    ant_value_t err = fs_mk_errno_error(js, errno, "stat", path_cstr, NULL);
    free(path_cstr); return err;
  }
  
  free(path_cstr);
  return create_stats_object(js, &st);
}

static ant_value_t builtin_fs_stat(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "stat() requires a path argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "stat() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  
  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");
  
  req->js = js;
  req->op_type = FS_OP_STAT;
  req->promise = js_mkpromise(js);
  req->path = strndup(path, path_len);
  req->uv_req.data = req;
  
  utarray_push_back(pending_requests, &req);
  int result = uv_fs_stat(uv_default_loop(), &req->uv_req, req->path, on_stat_complete);
  
  if (result < 0) {
    fs_request_fail(req, result);
    req->completed = 1;
    complete_request(req);
  }
  
  return req->promise;
}

static ant_value_t builtin_fs_lstatSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "lstatSync() requires a path argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "lstatSync() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  char *path_cstr = strndup(path, path_len);
  if (!path_cstr) return js_mkerr(js, "Out of memory");
  
  uv_fs_t req;
  int result = uv_fs_lstat(NULL, &req, path_cstr, NULL);

  if (result < 0) {
    ant_value_t err = fs_mk_uv_error(js, result, "lstat", path_cstr, NULL);
    uv_fs_req_cleanup(&req);
    free(path_cstr);
    return err;
  }

  ant_value_t stat_obj = fs_stats_object_from_uv(js, &req.statbuf);
  uv_fs_req_cleanup(&req);
  free(path_cstr);
  
  return stat_obj;
}

static ant_value_t builtin_fs_lstat(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "lstat() requires a path argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "lstat() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  
  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");
  
  req->js = js;
  req->op_type = FS_OP_STAT;
  req->promise = js_mkpromise(js);
  req->path = strndup(path, path_len);
  req->uv_req.data = req;
  
  utarray_push_back(pending_requests, &req);
  int result = uv_fs_lstat(uv_default_loop(), &req->uv_req, req->path, on_stat_complete);
  
  if (result < 0) {
    fs_request_fail(req, result);
    req->completed = 1;
    complete_request(req);
  }
  
  return req->promise;
}

static ant_value_t builtin_fs_fstatSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "fstatSync() requires an fd argument");
  if (vtype(args[0]) != T_NUM) return js_mkerr(js, "fstatSync() fd must be a number");

  uv_file fd = (uv_file)(int)js_getnum(args[0]);
  uv_fs_t req;
  int result = uv_fs_fstat(NULL, &req, fd, NULL);

  if (result < 0) {
    ant_value_t err = fs_mk_uv_error(js, result, "fstat", NULL, NULL);
    uv_fs_req_cleanup(&req);
    return err;
  }

  ant_value_t stat_obj = fs_stats_object_from_uv(js, &req.statbuf);
  uv_fs_req_cleanup(&req);
  return stat_obj;
}

static ant_value_t builtin_fs_fstat(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "fstat() requires an fd argument");
  if (vtype(args[0]) != T_NUM) return js_mkerr(js, "fstat() fd must be a number");

  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");

  req->js = js;
  req->op_type = FS_OP_STAT;
  req->promise = js_mkpromise(js);
  req->fd = (uv_file)(int)js_getnum(args[0]);
  req->uv_req.data = req;

  utarray_push_back(pending_requests, &req);
  int result = uv_fs_fstat(uv_default_loop(), &req->uv_req, req->fd, on_stat_complete);

  if (result < 0) {
    fs_request_fail(req, result);
    req->completed = 1;
    complete_request(req);
  }

  return req->promise;
}

static ant_value_t builtin_fs_existsSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "existsSync() requires a path argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "existsSync() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  char *path_cstr = strndup(path, path_len);
  if (!path_cstr) return js_mkerr(js, "Out of memory");
  
  struct stat st;
  int result = stat(path_cstr, &st);
  free(path_cstr);
  
  return js_bool(result == 0);
}

static ant_value_t builtin_fs_exists(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "exists() requires a path argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "exists() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  
  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");
  
  req->js = js;
  req->op_type = FS_OP_EXISTS;
  req->promise = js_mkpromise(js);
  req->path = strndup(path, path_len);
  req->uv_req.data = req;
  
  utarray_push_back(pending_requests, &req);
  int result = uv_fs_stat(uv_default_loop(), &req->uv_req, req->path, on_exists_complete);
  
  if (result < 0) {
    req->completed = 1;
    js_resolve_promise(req->js, req->promise, js_false);
    remove_pending_request(req);
    free_fs_request(req);
  }
  
  return req->promise;
}

static ant_value_t builtin_fs_accessSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "accessSync() requires a path argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "accessSync() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  int mode = F_OK;
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    mode = (int)js_getnum(args[1]);
  }
  
  char *path_cstr = strndup(path, path_len);
  if (!path_cstr) return js_mkerr(js, "Out of memory");
  int result = access(path_cstr, mode);
  
  if (result != 0) {
    ant_value_t err = fs_mk_errno_error(js, errno, "access", path_cstr, NULL);
    free(path_cstr); return err;
  }
  
  free(path_cstr);
  return js_mkundef();
}

static ant_value_t builtin_fs_chmodSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "chmodSync() requires a path argument");
  if (nargs < 2) return js_mkerr(js, "chmodSync() requires a mode argument");

  ant_value_t path_val = fs_coerce_path(js, args[0]);
  if (vtype(path_val) != T_STR) return js_mkerr(js, "chmodSync() path must be a string or URL");

  mode_t mode = 0;
  if (!fs_parse_mode(js, args[1], &mode)) {
    return js_mkerr(js, "chmodSync() mode must be a number or octal string");
  }

  size_t path_len = 0;
  char *path = js_getstr(js, path_val, &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");

  char *path_cstr = strndup(path, path_len);
  if (!path_cstr) return js_mkerr(js, "Out of memory");

  uv_fs_t req;
  int result = uv_fs_chmod(uv_default_loop(), &req, path_cstr, mode, NULL);

  if (result < 0) {
    ant_value_t err = fs_mk_uv_error(js, result, "chmod", path_cstr, NULL);
    uv_fs_req_cleanup(&req);
    free(path_cstr);
    return err;
  }

  uv_fs_req_cleanup(&req);
  free(path_cstr);
  return js_mkundef();
}

static ant_value_t builtin_fs_chmod(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "chmod() requires a path argument");
  if (nargs < 2) return js_mkerr(js, "chmod() requires a mode argument");

  ant_value_t path_val = fs_coerce_path(js, args[0]);
  if (vtype(path_val) != T_STR) return js_mkerr(js, "chmod() path must be a string or URL");

  mode_t mode = 0;
  if (!fs_parse_mode(js, args[1], &mode)) {
    return js_mkerr(js, "chmod() mode must be a number or octal string");
  }

  size_t path_len = 0;
  char *path = js_getstr(js, path_val, &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");

  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");

  req->js = js;
  req->op_type = FS_OP_CHMOD;
  req->promise = js_mkpromise(js);
  req->path = strndup(path, path_len);
  req->uv_req.data = req;

  if (!req->path) {
    free_fs_request(req);
    return js_mkerr(js, "Out of memory");
  }

  utarray_push_back(pending_requests, &req);
  int result = uv_fs_chmod(uv_default_loop(), &req->uv_req, req->path, mode, on_chmod_complete);

  if (result < 0) {
    fs_request_fail(req, result);
    req->completed = 1;
    complete_request(req);
  }

  return req->promise;
}

static ant_value_t builtin_fs_access(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "access() requires a path argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "access() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  int mode = F_OK;
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    mode = (int)js_getnum(args[1]);
  }
  
  
  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");
  
  req->js = js;
  req->op_type = FS_OP_ACCESS;
  req->promise = js_mkpromise(js);
  req->path = strndup(path, path_len);
  req->uv_req.data = req;
  
  utarray_push_back(pending_requests, &req);
  int result = uv_fs_access(uv_default_loop(), &req->uv_req, req->path, mode, on_access_complete);
  
  if (result < 0) {
    fs_request_fail(req, result);
    req->completed = 1;
    complete_request(req);
  }
  
  return req->promise;
}

static ant_value_t builtin_fs_realpathSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "realpathSync() requires a path argument");

  ant_value_t path_val = fs_coerce_path(js, args[0]);
  if (vtype(path_val) != T_STR) return js_mkerr(js, "realpathSync() path must be a string");

  size_t path_len = 0;
  const char *path = js_getstr(js, path_val, &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");

  char *resolved = realpath(path, NULL);
  if (!resolved) return js_mkerr(js, "realpathSync failed for '%s'", path);

  ant_value_t result = js_mkstr(js, resolved, strlen(resolved));
  free(resolved);
  return result;
}

static ant_value_t builtin_fs_realpath(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "realpath() requires a path argument");

  ant_value_t path_val = fs_coerce_path(js, args[0]);
  if (vtype(path_val) != T_STR) return js_mkerr(js, "realpath() path must be a string");

  size_t path_len = 0;
  const char *path = js_getstr(js, path_val, &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");

  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");

  req->js = js;
  req->op_type = FS_OP_REALPATH;
  req->promise = js_mkpromise(js);
  req->path = strndup(path, path_len);
  req->uv_req.data = req;

  if (!req->path) {
    free_fs_request(req);
    return js_mkerr(js, "Out of memory");
  }

  utarray_push_back(pending_requests, &req);
  int result = uv_fs_realpath(uv_default_loop(), &req->uv_req, req->path, on_realpath_complete);

  if (result < 0) {
    fs_request_fail(req, result);
    req->completed = 1;
    complete_request(req);
  }

  return req->promise;
}

static ant_value_t builtin_fs_readlinkSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "readlinkSync() requires a path argument");

  ant_value_t path_val = fs_coerce_path(js, args[0]);
  if (vtype(path_val) != T_STR) return js_mkerr(js, "readlinkSync() path must be a string");

  size_t path_len = 0;
  const char *path = js_getstr(js, path_val, &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");

  char *path_cstr = strndup(path, path_len);
  if (!path_cstr) return js_mkerr(js, "Out of memory");

  uv_fs_t req;
  int result = uv_fs_readlink(NULL, &req, path_cstr, NULL);

  if (result < 0 || !req.ptr) {
    ant_value_t err = fs_mk_uv_error(js, result, "readlink", path_cstr, NULL);
    uv_fs_req_cleanup(&req);
    free(path_cstr);
    return err;
  }

  ant_value_t link = js_mkstr(js, (const char *)req.ptr, strlen((const char *)req.ptr));
  uv_fs_req_cleanup(&req);
  free(path_cstr);
  return link;
}

static ant_value_t builtin_fs_readlink(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "readlink() requires a path argument");

  ant_value_t path_val = fs_coerce_path(js, args[0]);
  if (vtype(path_val) != T_STR) return js_mkerr(js, "readlink() path must be a string");

  size_t path_len = 0;
  const char *path = js_getstr(js, path_val, &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");

  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");

  req->js = js;
  req->op_type = FS_OP_REALPATH;
  req->promise = js_mkpromise(js);
  req->path = strndup(path, path_len);
  req->uv_req.data = req;

  if (!req->path) {
    free_fs_request(req);
    return js_mkerr(js, "Out of memory");
  }

  utarray_push_back(pending_requests, &req);
  int result = uv_fs_readlink(uv_default_loop(), &req->uv_req, req->path, on_realpath_complete);

  if (result < 0) {
    fs_request_fail(req, result);
    req->completed = 1;
    complete_request(req);
  }

  return req->promise;
}

static ant_value_t builtin_fs_readdirSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "readdirSync() requires a path argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "readdirSync() path must be a string");
  
  bool with_file_types = false;
  if (nargs >= 2 && vtype(args[1]) == T_OBJ) {
    ant_value_t wft = js_get(js, args[1], "withFileTypes");
    with_file_types = js_truthy(js, wft);
  }

  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  char *path_cstr = strndup(path, path_len);
  if (!path_cstr) return js_mkerr(js, "Out of memory");
  
  uv_fs_t req;
  int result = uv_fs_scandir(NULL, &req, path_cstr, 0, NULL);
  
  if (result < 0) {
    ant_value_t err = fs_mk_uv_error(js, result, "scandir", path_cstr, NULL);
    uv_fs_req_cleanup(&req);
    free(path_cstr);
    return err;
  }
  
  free(path_cstr);
  
  ant_value_t arr = js_mkarr(js);
  uv_dirent_t dirent;
  
  while (uv_fs_scandir_next(&req, &dirent) != UV_EOF) {
  if (with_file_types) {
    ant_value_t entry = create_dirent_object(js, dirent.name, strlen(dirent.name), dirent.type);
    js_arr_push(js, arr, entry);
  } else {
    ant_value_t name = js_mkstr(js, dirent.name, strlen(dirent.name));
    js_arr_push(js, arr, name);
  }}
  
  uv_fs_req_cleanup(&req);
  return arr;
}

static ant_value_t builtin_fs_readdir(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "readdir() requires a path argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "readdir() path must be a string");
  
  bool with_file_types = false;
  if (nargs >= 2 && vtype(args[1]) == T_OBJ) {
    ant_value_t wft = js_get(js, args[1], "withFileTypes");
    with_file_types = js_truthy(js, wft);
  }

  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  
  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");
  
  req->js = js;
  req->op_type = FS_OP_READDIR;
  req->with_file_types = with_file_types;
  req->promise = js_mkpromise(js);
  req->path = strndup(path, path_len);
  req->uv_req.data = req;
  
  utarray_push_back(pending_requests, &req);
  int result = uv_fs_scandir(uv_default_loop(), &req->uv_req, req->path, 0, on_readdir_complete);
  
  if (result < 0) {
    fs_request_fail(req, result);
    req->completed = 1;
    complete_request(req);
  }
  
  return req->promise;
}

static void on_write_fd_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;
  
  if (uv_req->result < 0) {
    fs_request_fail(req, (int)uv_req->result);
    req->completed = 1;
    complete_request(req);
    return;
  }
  
  req->completed = 1;
  js_resolve_promise(req->js, req->promise, js_mknum((double)uv_req->result));
  remove_pending_request(req);
  free_fs_request(req);
}

static void on_read_fd_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;

  if (uv_req->result < 0) {
    if (is_callable(req->callback_fn)) {
      ant_value_t err = js_mkerr(req->js, "read failed: %s", uv_strerror((int)uv_req->result));
      ant_value_t cb_args[1] = { err };
      fs_call_value(req->js, req->callback_fn, js_mkundef(), cb_args, 1);
      remove_pending_request(req);
      free_fs_request(req);
      return;
    }
    fs_request_fail(req, (int)uv_req->result);
    req->completed = 1;
    complete_request(req);
    return;
  }

  ssize_t bytes_read = uv_req->result;

  if (req->data && bytes_read > 0) {
    TypedArrayData *ta = buffer_get_typedarray_data(req->target_buffer);
    if (ta && ta->buffer && ta->buffer->data) {
      uint8_t *dest = ta->buffer->data + ta->byte_offset + req->buf_offset;
      memcpy(dest, req->data, (size_t)bytes_read);
    }
  }

  if (is_callable(req->callback_fn)) {
    ant_value_t cb_args[3] = { js_mknull(), js_mknum((double)bytes_read), req->target_buffer };
    fs_call_value(req->js, req->callback_fn, js_mkundef(), cb_args, 3);
    remove_pending_request(req);
    free_fs_request(req);
    return;
  }

  req->completed = 1;
  js_resolve_promise(req->js, req->promise, js_mknum((double)bytes_read));
  remove_pending_request(req);
  free_fs_request(req);
}

static ant_value_t builtin_fs_read_fd(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "read() requires fd and buffer arguments");
  if (vtype(args[0]) != T_NUM) return js_mkerr(js, "read() fd must be a number");

  int fd = (int)js_getnum(args[0]);

  ant_value_t buf_arg = args[1];
  TypedArrayData *ta_data = buffer_get_typedarray_data(buf_arg);
  if (!ta_data || !ta_data->buffer || !ta_data->buffer->data)
    return js_mkerr(js, "read() buffer argument must be a Buffer or TypedArray");

  size_t buf_len = ta_data->byte_length;
  size_t offset = 0;
  size_t length = buf_len;
  int64_t position = -1;

  ant_value_t callback = js_mkundef();
  int data_nargs = nargs;
  if (nargs >= 3 && is_callable(args[nargs - 1])) {
    callback = args[nargs - 1];
    data_nargs = nargs - 1;
  }

  if (data_nargs >= 3 && vtype(args[2]) == T_NUM) offset = (size_t)js_getnum(args[2]);
  if (data_nargs >= 4 && vtype(args[3]) == T_NUM) length = (size_t)js_getnum(args[3]);
  if (data_nargs >= 5 && vtype(args[4]) == T_NUM) position = (int64_t)js_getnum(args[4]);

  if (offset > buf_len) return js_mkerr(js, "read() offset out of bounds");
  if (offset + length > buf_len) return js_mkerr(js, "read() length extends beyond buffer");

  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");

  req->js = js;
  req->op_type = FS_OP_READ_FD;
  req->promise = js_mkpromise(js);
  req->fd = fd;
  req->target_buffer = buf_arg;
  req->buf_offset = offset;
  req->data_len = length;
  req->callback_fn = callback;
  req->data = malloc(length > 0 ? length : 1);
  if (!req->data) {
    free(req);
    return js_mkerr(js, "Out of memory");
  }
  req->uv_req.data = req;

  utarray_push_back(pending_requests, &req);

  uv_buf_t buf = uv_buf_init(req->data, (unsigned int)length);
  int result = uv_fs_read(uv_default_loop(), &req->uv_req, req->fd, &buf, 1, position, on_read_fd_complete);

  if (result < 0) {
    fs_request_fail(req, result);
    req->completed = 1;
    complete_request(req);
  }

  return req->promise;
}

static ant_value_t builtin_fs_readSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "readSync() requires fd and buffer arguments");
  if (vtype(args[0]) != T_NUM) return js_mkerr(js, "readSync() fd must be a number");
  
  int fd = (int)js_getnum(args[0]);
  
  TypedArrayData *ta_data = buffer_get_typedarray_data(args[1]);
  if (!ta_data || !ta_data->buffer || !ta_data->buffer->data)
    return js_mkerr(js, "readSync() second argument must be a Buffer, TypedArray, or DataView");
  
  uint8_t *buf_data = ta_data->buffer->data + ta_data->byte_offset;
  size_t buf_len = ta_data->byte_length;
  size_t offset = 0;
  size_t length = buf_len;
  int64_t position = -1;
  
  if (nargs >= 3) {
  if (vtype(args[2]) == T_OBJ) {
    ant_value_t off_val = js_get(js, args[2], "offset");
    ant_value_t len_val = js_get(js, args[2], "length");
    ant_value_t pos_val = js_get(js, args[2], "position");
    if (vtype(off_val) == T_NUM) offset = (size_t)js_getnum(off_val);
    if (vtype(len_val) == T_NUM) length = (size_t)js_getnum(len_val);
    else length = buf_len - offset;
    if (vtype(pos_val) == T_NUM) position = (int64_t)js_getnum(pos_val);
  } else if (vtype(args[2]) == T_NUM) {
    offset = (size_t)js_getnum(args[2]);
    length = buf_len - offset;
    if (nargs >= 4 && vtype(args[3]) == T_NUM) length = (size_t)js_getnum(args[3]);
    if (nargs >= 5 && vtype(args[4]) == T_NUM) position = (int64_t)js_getnum(args[4]);
  }}
  
  if (offset > buf_len) return js_mkerr(js, "offset is out of bounds");
  if (offset + length > buf_len) return js_mkerr(js, "length extends beyond buffer");
  
  uv_fs_t req;
  uv_buf_t buf = uv_buf_init((char *)(buf_data + offset), (unsigned int)length);
  int result = uv_fs_read(uv_default_loop(), &req, fd, &buf, 1, position, NULL);
  uv_fs_req_cleanup(&req);
  
  if (result < 0) return js_mkerr(js, "readSync failed: %s", uv_strerror(result));
  return js_mknum((double)result);
}

static ant_value_t builtin_fs_writeSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "writeSync() requires fd and data arguments");
  if (vtype(args[0]) != T_NUM) return js_mkerr(js, "writeSync() fd must be a number");
  
  int fd = (int)js_getnum(args[0]);
  
  if (vtype(args[1]) == T_STR) {
    size_t str_len;
    const char *str = js_getstr(js, args[1], &str_len);
    if (!str) return js_mkerr(js, "Failed to get string");
    
    int64_t position = -1;
    if (nargs >= 3 && vtype(args[2]) == T_NUM)
      position = (int64_t)js_getnum(args[2]);
    
    uv_fs_t req;
    uv_buf_t buf = uv_buf_init((char *)str, (unsigned int)str_len);
    int result = uv_fs_write(uv_default_loop(), &req, fd, &buf, 1, position, NULL);
    uv_fs_req_cleanup(&req);
    
    if (result < 0) return js_mkerr(js, "writeSync failed: %s", uv_strerror(result));
    return js_mknum((double)result);
  }
  
  TypedArrayData *ta_data = buffer_get_typedarray_data(args[1]);
  if (!ta_data || !ta_data->buffer || !ta_data->buffer->data)
    return js_mkerr(js, "writeSync() second argument must be a Buffer, TypedArray, DataView, or string");
  
  uint8_t *buf_data = ta_data->buffer->data + ta_data->byte_offset;
  size_t buf_len = ta_data->byte_length;
  size_t offset = 0;
  size_t length = buf_len;
  int64_t position = -1;
  
  if (nargs >= 3) {
    if (vtype(args[2]) == T_OBJ) {
      ant_value_t off_val = js_get(js, args[2], "offset");
      ant_value_t len_val = js_get(js, args[2], "length");
      ant_value_t pos_val = js_get(js, args[2], "position");
      if (vtype(off_val) == T_NUM) offset = (size_t)js_getnum(off_val);
      if (vtype(len_val) == T_NUM) length = (size_t)js_getnum(len_val);
      else length = buf_len - offset;
      if (vtype(pos_val) == T_NUM) position = (int64_t)js_getnum(pos_val);
    } else if (vtype(args[2]) == T_NUM) {
      offset = (size_t)js_getnum(args[2]);
      length = buf_len - offset;
      if (nargs >= 4 && vtype(args[3]) == T_NUM) length = (size_t)js_getnum(args[3]);
      if (nargs >= 5 && vtype(args[4]) == T_NUM) position = (int64_t)js_getnum(args[4]);
    }
  }
  
  if (offset > buf_len) return js_mkerr(js, "offset is out of bounds");
  if (offset + length > buf_len) return js_mkerr(js, "length extends beyond buffer");
  
  uv_fs_t req;
  uv_buf_t buf = uv_buf_init((char *)(buf_data + offset), (unsigned int)length);
  int result = uv_fs_write(uv_default_loop(), &req, fd, &buf, 1, position, NULL);
  uv_fs_req_cleanup(&req);
  
  if (result < 0) return js_mkerr(js, "writeSync failed: %s", uv_strerror(result));
  return js_mknum((double)result);
}

static ant_value_t builtin_fs_write_fd(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "write() requires fd and data arguments");
  if (vtype(args[0]) != T_NUM) return js_mkerr(js, "write() fd must be a number");
  
  int fd = (int)js_getnum(args[0]);
  const char *write_data;
  size_t write_len;
  int64_t position = -1;
  
  if (vtype(args[1]) == T_STR) {
    size_t str_len;
    const char *str = js_getstr(js, args[1], &str_len);
    if (!str) return js_mkerr(js, "Failed to get string");
    
    if (nargs >= 3 && vtype(args[2]) == T_NUM)
      position = (int64_t)js_getnum(args[2]);
    
    write_data = str;
    write_len = str_len;
  } else {
    TypedArrayData *ta_data = buffer_get_typedarray_data(args[1]);
    if (!ta_data || !ta_data->buffer || !ta_data->buffer->data)
      return js_mkerr(js, "write() second argument must be a Buffer, TypedArray, DataView, or string");
    
    uint8_t *buf_data = ta_data->buffer->data + ta_data->byte_offset;
    size_t buf_len = ta_data->byte_length;
    size_t offset = 0;
    size_t length = buf_len;
    
    if (nargs >= 3) {
    if (vtype(args[2]) == T_OBJ) {
      ant_value_t off_val = js_get(js, args[2], "offset");
      ant_value_t len_val = js_get(js, args[2], "length");
      ant_value_t pos_val = js_get(js, args[2], "position");
      if (vtype(off_val) == T_NUM) offset = (size_t)js_getnum(off_val);
      if (vtype(len_val) == T_NUM) length = (size_t)js_getnum(len_val);
      else length = buf_len - offset;
      if (vtype(pos_val) == T_NUM) position = (int64_t)js_getnum(pos_val);
    } else if (vtype(args[2]) == T_NUM) {
      offset = (size_t)js_getnum(args[2]);
      length = buf_len - offset;
      if (nargs >= 4 && vtype(args[3]) == T_NUM) length = (size_t)js_getnum(args[3]);
      if (nargs >= 5 && vtype(args[4]) == T_NUM) position = (int64_t)js_getnum(args[4]);
    }}
    
    if (offset > buf_len) return js_mkerr(js, "offset is out of bounds");
    if (offset + length > buf_len) return js_mkerr(js, "length extends beyond buffer");
    
    write_data = (const char *)(buf_data + offset);
    write_len = length;
  }
  
  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");
  
  req->js = js;
  req->op_type = FS_OP_WRITE_FD;
  req->promise = js_mkpromise(js);
  req->fd = fd;
  req->data = malloc(write_len);
  if (!req->data) {
    free(req);
    return js_mkerr(js, "Out of memory");
  }
  memcpy(req->data, write_data, write_len);
  req->data_len = write_len;
  req->uv_req.data = req;
  
  utarray_push_back(pending_requests, &req);
  
  uv_buf_t buf = uv_buf_init(req->data, (unsigned int)req->data_len);
  int result = uv_fs_write(uv_default_loop(), &req->uv_req, req->fd, &buf, 1, position, on_write_fd_complete);
  
  if (result < 0) {
    fs_request_fail(req, result);
    req->completed = 1;
    complete_request(req);
  }
  
  return req->promise;
}

static ant_value_t builtin_fs_writevSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "writevSync() requires fd and buffers arguments");
  if (vtype(args[0]) != T_NUM) return js_mkerr(js, "writevSync() fd must be a number");
  
  int fd = (int)js_getnum(args[0]);
  ant_offset_t arr_len = js_arr_len(js, args[1]);
  if (arr_len == 0) return js_mknum(0);
  
  int64_t position = -1;
  if (nargs >= 3 && vtype(args[2]) == T_NUM)
    position = (int64_t)js_getnum(args[2]);
  
  uv_buf_t *bufs = calloc((size_t)arr_len, sizeof(uv_buf_t));
  if (!bufs) return js_mkerr(js, "Out of memory");
  
  for (ant_offset_t i = 0; i < arr_len; i++) {
    ant_value_t item = js_arr_get(js, args[1], i);
    TypedArrayData *ta = buffer_get_typedarray_data(item);
    if (!ta || !ta->buffer || !ta->buffer->data) {
      free(bufs);
      return js_mkerr(js, "writevSync() buffers must contain ArrayBufferViews");
    }
    bufs[i] = uv_buf_init((char *)(ta->buffer->data + ta->byte_offset), (unsigned int)ta->byte_length);
  }
  
  uv_fs_t req;
  int result = uv_fs_write(uv_default_loop(), &req, fd, bufs, (unsigned int)arr_len, position, NULL);
  uv_fs_req_cleanup(&req);
  free(bufs);
  
  if (result < 0) return js_mkerr(js, "writevSync failed: %s", uv_strerror(result));
  return js_mknum((double)result);
}

static ant_value_t builtin_fs_writev_fd(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "writev() requires fd and buffers arguments");
  if (vtype(args[0]) != T_NUM) return js_mkerr(js, "writev() fd must be a number");
  
  int fd = (int)js_getnum(args[0]);
  ant_offset_t arr_len = js_arr_len(js, args[1]);
  
  if (arr_len == 0) {
    ant_value_t promise = js_mkpromise(js);
    js_resolve_promise(js, promise, js_mknum(0));
    return promise;
  }
  
  int64_t position = -1;
  if (nargs >= 3 && vtype(args[2]) == T_NUM)
    position = (int64_t)js_getnum(args[2]);
  
  size_t total_len = 0;
  for (ant_offset_t i = 0; i < arr_len; i++) {
    ant_value_t item = js_arr_get(js, args[1], i);
    TypedArrayData *ta = buffer_get_typedarray_data(item);
    if (!ta || !ta->buffer || !ta->buffer->data)
      return js_mkerr(js, "writev() buffers must contain ArrayBufferViews");
    total_len += ta->byte_length;
  }
  
  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");
  
  req->data = malloc(total_len);
  if (!req->data) {
    free(req);
    return js_mkerr(js, "Out of memory");
  }
  
  size_t off = 0;
  for (ant_offset_t i = 0; i < arr_len; i++) {
    ant_value_t item = js_arr_get(js, args[1], i);
    TypedArrayData *ta = buffer_get_typedarray_data(item);
    memcpy(req->data + off, ta->buffer->data + ta->byte_offset, ta->byte_length);
    off += ta->byte_length;
  }
  
  req->js = js;
  req->op_type = FS_OP_WRITE_FD;
  req->promise = js_mkpromise(js);
  req->fd = fd;
  req->data_len = total_len;
  req->uv_req.data = req;
  
  utarray_push_back(pending_requests, &req);
  
  uv_buf_t buf = uv_buf_init(req->data, (unsigned int)total_len);
  int result = uv_fs_write(uv_default_loop(), &req->uv_req, req->fd, &buf, 1, position, on_write_fd_complete);
  
  if (result < 0) {
    fs_request_fail(req, result);
    req->completed = 1;
    complete_request(req);
  }
  
  return req->promise;
}

static int parse_open_flags(ant_t *js, ant_value_t arg) {
  if (vtype(arg) == T_NUM) return (int)js_getnum(arg);
  if (vtype(arg) != T_STR) return O_RDONLY;
  
  size_t len;
  const char *str = js_getstr(js, arg, &len);
  if (!str) return O_RDONLY;
  
  if (len == 1 && str[0] == 'r')                      return O_RDONLY;
  if (len == 2 && memcmp(str, "r+", 2) == 0)          return O_RDWR;
  if (len == 1 && str[0] == 'w')                      return O_WRONLY | O_CREAT | O_TRUNC;
  if (len == 2 && memcmp(str, "wx", 2) == 0)          return O_WRONLY | O_CREAT | O_TRUNC | O_EXCL;
  if (len == 2 && memcmp(str, "w+", 2) == 0)          return O_RDWR | O_CREAT | O_TRUNC;
  if (len == 3 && memcmp(str, "wx+", 3) == 0)         return O_RDWR | O_CREAT | O_TRUNC | O_EXCL;
  if (len == 1 && str[0] == 'a')                      return O_WRONLY | O_CREAT | O_APPEND;
  if (len == 2 && memcmp(str, "ax", 2) == 0)          return O_WRONLY | O_CREAT | O_APPEND | O_EXCL;
  if (len == 2 && memcmp(str, "a+", 2) == 0)          return O_RDWR | O_CREAT | O_APPEND;
  if (len == 3 && memcmp(str, "ax+", 3) == 0)         return O_RDWR | O_CREAT | O_APPEND | O_EXCL;
  
  return O_RDONLY;
}

static ant_value_t builtin_fs_openSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "openSync() requires a path argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "openSync() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  int flags = (nargs >= 2) ? parse_open_flags(js, args[1]) : O_RDONLY;
  int mode = (nargs >= 3 && vtype(args[2]) == T_NUM) ? (int)js_getnum(args[2]) : 0666;
  
  char *path_cstr = strndup(path, path_len);
  if (!path_cstr) return js_mkerr(js, "Out of memory");
  
  uv_fs_t req;
  int result = uv_fs_open(uv_default_loop(), &req, path_cstr, flags, mode, NULL);
  
  if (result < 0) {
    ant_value_t err = fs_mk_uv_error(js, result, "open", path_cstr, NULL);
    uv_fs_req_cleanup(&req);
    free(path_cstr);
    return err;
  }
  
  uv_fs_req_cleanup(&req);
  free(path_cstr);
  
  return js_mknum((double)result);
}

static ant_value_t builtin_fs_closeSync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "closeSync() requires a fd argument");
  if (vtype(args[0]) != T_NUM) return js_mkerr(js, "closeSync() fd must be a number");
  
  int fd = (int)js_getnum(args[0]);
  
  uv_fs_t req;
  int result = uv_fs_close(uv_default_loop(), &req, fd, NULL);
  uv_fs_req_cleanup(&req);
  
  if (result < 0) return js_mkerr(js, "closeSync failed: %s", uv_strerror(result));
  return js_mkundef();
}

static void on_open_fd_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;
  
  if (uv_req->result < 0) {
    fs_request_fail(req, (int)uv_req->result);
    req->completed = 1;
    complete_request(req);
    return;
  }
  
  req->completed = 1;
  js_resolve_promise(req->js, req->promise, js_mknum((double)uv_req->result));
  remove_pending_request(req);
  free_fs_request(req);
}

static ant_value_t builtin_fs_open_fd(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "open() requires a path argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "open() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  int flags = (nargs >= 2) ? parse_open_flags(js, args[1]) : O_RDONLY;
  int mode = (nargs >= 3 && vtype(args[2]) == T_NUM) ? (int)js_getnum(args[2]) : 0666;
  
  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");
  
  req->js = js;
  req->op_type = FS_OP_OPEN;
  req->promise = js_mkpromise(js);
  req->path = strndup(path, path_len);
  req->uv_req.data = req;
  
  utarray_push_back(pending_requests, &req);
  int result = uv_fs_open(uv_default_loop(), &req->uv_req, req->path, flags, mode, on_open_fd_complete);
  
  if (result < 0) {
    fs_request_fail(req, result);
    req->completed = 1;
    complete_request(req);
  }
  
  return req->promise;
}

static void on_close_fd_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;
  
  if (uv_req->result < 0) {
    fs_request_fail(req, (int)uv_req->result);
    req->completed = 1;
    complete_request(req);
    return;
  }
  
  req->completed = 1;
  js_resolve_promise(req->js, req->promise, js_mkundef());
  remove_pending_request(req);
  free_fs_request(req);
}

static ant_value_t builtin_fs_close_fd(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "close() requires a fd argument");
  if (vtype(args[0]) != T_NUM) return js_mkerr(js, "close() fd must be a number");
  
  int fd = (int)js_getnum(args[0]);
  
  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");
  
  req->js = js;
  req->op_type = FS_OP_CLOSE;
  req->promise = js_mkpromise(js);
  req->fd = fd;
  req->uv_req.data = req;
  
  utarray_push_back(pending_requests, &req);
  int result = uv_fs_close(uv_default_loop(), &req->uv_req, req->fd, on_close_fd_complete);
  
  if (result < 0) {
    fs_request_fail(req, result);
    req->completed = 1;
    complete_request(req);
  }
  
  return req->promise;
}

static ant_value_t builtin_fs_watch(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t path_val = 0;
  ant_value_t watcher_obj = 0;
  fs_watch_options_t opts;
  fs_watcher_t *watcher = NULL;
  uv_handle_t *handle = NULL;
  const char *path = NULL;
  size_t path_len = 0;
  int rc = 0;

  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "watch() requires a path argument");
  if (!fs_parse_watch_options(js, args, nargs, &opts))
    return js_mkerr_typed(js, JS_ERR_TYPE, "watch() options must be a string, object, or callback");

  path_val = fs_coerce_path(js, args[0]);
  if (vtype(path_val) != T_STR) return js_mkerr_typed(js, JS_ERR_TYPE, "watch() path must be a string or URL");

  path = js_getstr(js, path_val, &path_len);
  if (!path || path_len == 0) return js_mkerr(js, "watch() path must not be empty");

  watcher = fs_watcher_new(js, FS_WATCH_MODE_EVENT);
  if (!watcher) return js_mkerr(js, "Out of memory");

  watcher_obj = fs_watcher_make_object(js, watcher);
  if (is_err(watcher_obj)) {
    fs_watcher_free(watcher);
    return watcher_obj;
  }

  rc = fs_watcher_start_event(watcher, path, opts.persistent, opts.recursive);
  if (rc != 0) {
    js_set_native_ptr(watcher_obj, NULL);
    fs_watcher_free(watcher);
    return fs_watch_error(js, rc, path);
  }

  fs_add_active_watcher(watcher);
  if (!opts.persistent) {
    handle = fs_watcher_uv_handle(watcher);
    if (handle) uv_unref(handle);
  }
  if (is_callable(opts.listener))
    eventemitter_add_listener(js, watcher_obj, "change", opts.listener, false);
  return watcher_obj;
}

static ant_value_t builtin_fs_watchFile(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t path_val = 0;
  fs_watchfile_options_t opts;
  
  fs_watcher_t *watcher = NULL;
  uv_handle_t *handle = NULL;
  
  const char *path = NULL;
  size_t path_len = 0;
  int rc = 0;

  if (nargs < 2)
    return js_mkerr_typed(js, JS_ERR_TYPE, "watchFile() requires a path and listener");
  if (!fs_parse_watchfile_options(js, args, nargs, &opts))
    return js_mkerr_typed(js, JS_ERR_TYPE, "watchFile() requires a listener callback");

  path_val = fs_coerce_path(js, args[0]);
  if (vtype(path_val) != T_STR) return js_mkerr_typed(js, JS_ERR_TYPE, "watchFile() path must be a string or URL");

  path = js_getstr(js, path_val, &path_len);
  if (!path || path_len == 0) return js_mkerr(js, "watchFile() path must not be empty");

  watcher = fs_watcher_new(js, FS_WATCH_MODE_STAT);
  if (!watcher) return js_mkerr(js, "Out of memory");

  watcher->callback = opts.listener;
  watcher->last_stat_valid = fs_stat_path_sync(path, &watcher->last_stat);

  rc = fs_watcher_start_poll(watcher, path, opts.persistent, opts.interval_ms);
  if (rc != 0) {
    fs_watcher_free(watcher);
    return fs_watch_error(js, rc, path);
  }

  fs_add_active_watcher(watcher);
  if (!opts.persistent) {
    handle = fs_watcher_uv_handle(watcher);
    if (handle) uv_unref(handle);
  }
  return js_mkundef();
}

static ant_value_t builtin_fs_unwatchFile(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t path_val = 0;
  ant_value_t listener = js_mkundef();
  
  char *resolved = NULL;
  fs_watcher_t *watcher = NULL;
  fs_watcher_t *next = NULL;
  
  const char *path = NULL;
  size_t path_len = 0;

  if (nargs < 1) return js_mkundef();

  path_val = fs_coerce_path(js, args[0]);
  if (vtype(path_val) != T_STR) return js_mkundef();

  path = js_getstr(js, path_val, &path_len);
  if (!path || path_len == 0) return js_mkundef();
  if (nargs > 1 && is_callable(args[1])) listener = args[1];

  resolved = ant_watch_resolve_path(path);
  if (!resolved) return js_mkundef();

  for (watcher = active_watchers; watcher; watcher = next) {
    next = watcher->next_active;
    if (watcher->mode != FS_WATCH_MODE_STAT) continue;
    if (!watcher->path || strcmp(watcher->path, resolved) != 0) continue;
    if (is_callable(listener) && watcher->callback != listener) continue;
    fs_watcher_close_native(watcher);
  }

  free(resolved);
  return js_mkundef();
}

void init_fs_module(void) {
  utarray_new(pending_requests, &ut_ptr_icd);
  
  ant_t *js = rt->js;
  ant_value_t glob = js->global;
  
  ant_value_t stats_ctor = js_mkobj(js);
  ant_value_t stats_proto = js_mkobj(js);
  
  js_set(js, stats_proto, "isFile", js_mkfun(stat_isFile));
  js_set(js, stats_proto, "isDirectory", js_mkfun(stat_isDirectory));
  js_set(js, stats_proto, "isSymbolicLink", js_mkfun(stat_isSymbolicLink));
  js_set_sym(js, stats_proto, get_toStringTag_sym(), js_mkstr(js, "Stats", 5));
  
  js_mkprop_fast(js, stats_ctor, "prototype", 9, stats_proto);
  js_mkprop_fast(js, stats_ctor, "name", 4, js_mkstr(js, "Stats", 5));
  js_set_descriptor(js, stats_ctor, "name", 4, 0);
  
  js_set(js, glob, "Stats", js_obj_to_func(stats_ctor));

  g_dirent_proto = js_mkobj(js);
  js_set(js, g_dirent_proto, "isFile", js_mkfun(dirent_isFile));
  js_set(js, g_dirent_proto, "isDirectory", js_mkfun(dirent_isDirectory));
  js_set(js, g_dirent_proto, "isSymbolicLink", js_mkfun(dirent_isSymbolicLink));
  js_set(js, g_dirent_proto, "isBlockDevice", js_mkfun(dirent_isBlockDevice));
  js_set(js, g_dirent_proto, "isCharacterDevice", js_mkfun(dirent_isCharacterDevice));
  js_set(js, g_dirent_proto, "isFIFO", js_mkfun(dirent_isFIFO));
  js_set(js, g_dirent_proto, "isSocket", js_mkfun(dirent_isSocket));
  js_set_sym(js, g_dirent_proto, get_toStringTag_sym(), js_mkstr(js, "Dirent", 6));
  gc_register_root(&g_dirent_proto);
}

static ant_value_t fs_callback_success_handler(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ctx = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t callback = js_get_slot(ctx, SLOT_DATA);

  if (!is_callable(callback)) return js_mkundef();

  if (js_truthy(js, js_get(js, ctx, "existsStyle"))) {
    ant_value_t cb_args[1] = { nargs > 0 ? args[0] : js_false };
    fs_call_value(js, callback, js_mkundef(), cb_args, 1);
    return js_mkundef();
  }

  ant_value_t cb_args[2] = { js_mknull(), nargs > 0 ? args[0] : js_mkundef() };
  fs_call_value(js, callback, js_mkundef(), cb_args, 2);
  return js_mkundef();
}

static ant_value_t fs_callback_error_handler(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ctx = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t callback = js_get_slot(ctx, SLOT_DATA);
  ant_value_t cb_args[1];

  if (!is_callable(callback)) return js_mkundef();

  if (js_truthy(js, js_get(js, ctx, "existsStyle"))) {
    cb_args[0] = js_false;
    fs_call_value(js, callback, js_mkundef(), cb_args, 1);
    return js_mkundef();
  }

  cb_args[0] = nargs > 0 ? args[0] : js_mkundef();
  fs_call_value(js, callback, js_mkundef(), cb_args, 1);
  return js_mkundef();
}

static void fs_callback_emit_success(
  ant_t *js,
  ant_value_t callback,
  bool exists_style,
  ant_value_t value
) {
  if (exists_style) {
    ant_value_t cb_args[1] = { value };
    fs_call_value(js, callback, js_mkundef(), cb_args, 1);
    return;
  }

  ant_value_t cb_args[2] = { js_mknull(), value };
  fs_call_value(js, callback, js_mkundef(), cb_args, 2);
}

static void fs_callback_emit_error(
  ant_t *js,
  ant_value_t callback,
  bool exists_style,
  ant_value_t error
) {
  ant_value_t cb_args[1];

  cb_args[0] = exists_style ? js_false : error;
  fs_call_value(js, callback, js_mkundef(), cb_args, 1);
}

static ant_value_t fs_callback_attach_promise(
  ant_t *js,
  ant_value_t promise,
  ant_value_t callback,
  bool exists_style
) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t success_fn = js_mkundef();
  ant_value_t error_fn = js_mkundef();

  GC_ROOT_PIN(js, promise);
  GC_ROOT_PIN(js, callback);

  ant_value_t success_ctx = js_mkobj(js);
  GC_ROOT_PIN(js, success_ctx);
  ant_value_t error_ctx = js_mkobj(js);
  GC_ROOT_PIN(js, error_ctx);

  js_set_slot(success_ctx, SLOT_DATA, callback);
  js_set_slot(error_ctx, SLOT_DATA, callback);
  if (exists_style) {
    js_set(js, success_ctx, "existsStyle", js_true);
    js_set(js, error_ctx, "existsStyle", js_true);
  }

  success_fn = js_heavy_mkfun(js, fs_callback_success_handler, success_ctx);
  GC_ROOT_PIN(js, success_fn);
  error_fn = js_heavy_mkfun(js, fs_callback_error_handler, error_ctx);
  GC_ROOT_PIN(js, error_fn);

  js_promise_then(js, promise, success_fn, error_fn);
  GC_ROOT_RESTORE(js, root_mark);
  return js_mkundef();
}

static ant_value_t fs_callback_wrapper_call(ant_t *js, ant_value_t *args, int nargs) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t wrapper = js_getcurrentfunc(js);
  ant_value_t config = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t original = js_get_slot(config, SLOT_DATA);
  
  ant_value_t callback = js_mkundef();
  ant_value_t result = js_mkundef();
  ant_value_t this_arg = js_getthis(js);
  ant_value_t ex = js_mkundef();
  
  bool exists_style = false;
  int call_nargs = nargs;

  GC_ROOT_PIN(js, original);
  GC_ROOT_PIN(js, this_arg);

  exists_style = js_truthy(js, js_get(js, config, "existsStyle"));
  if (nargs > 0 && is_callable(args[nargs - 1])) {
    callback = args[nargs - 1];
    GC_ROOT_PIN(js, callback);
    call_nargs--;
  }

  result = fs_call_value(js, original, this_arg, args, call_nargs);
  GC_ROOT_PIN(js, result);

  if (!is_callable(callback)) {
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }

  if (is_err(result) || js->thrown_exists) {
    ex = js->thrown_exists ? js->thrown_value : result;
    GC_ROOT_PIN(js, ex);
    js->thrown_exists = false;
    js->thrown_value = js_mkundef();
    js->thrown_stack = js_mkundef();
    fs_callback_emit_error(js, callback, exists_style, ex);
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkundef();
  }

  if (vtype(result) != T_PROMISE) {
    fs_callback_emit_success(js, callback, exists_style, result);
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkundef();
  }

  ant_value_t attach_result = fs_callback_attach_promise(js, result, callback, exists_style);
  GC_ROOT_RESTORE(js, root_mark);
  return attach_result;
}

static ant_value_t fs_make_callback_wrapper(ant_t *js, ant_value_t original, bool exists_style) {
  ant_value_t config = js_mkobj(js);

  js_set_slot(config, SLOT_DATA, original);
  if (exists_style) js_set(js, config, "existsStyle", js_true);
  return js_heavy_mkfun(js, fs_callback_wrapper_call, config);
}

static void fs_set_promise_methods(ant_t *js, ant_value_t lib) {
  js_set(js, lib, "appendFile", js_mkfun(builtin_fs_appendFile));
  js_set(js, lib, "cp", js_mkfun(builtin_fs_cp));
  js_set(js, lib, "copyFile", js_mkfun(builtin_fs_copyFile));
  js_set(js, lib, "readFile", js_mkfun(builtin_fs_readFile));
  js_set(js, lib, "open", js_mkfun(builtin_fs_open_fd));
  js_set(js, lib, "close", js_mkfun(builtin_fs_close_fd));
  js_set(js, lib, "writeFile", js_mkfun(builtin_fs_writeFile));
  js_set(js, lib, "write", js_mkfun(builtin_fs_write_fd));
  js_set(js, lib, "writev", js_mkfun(builtin_fs_writev_fd));
  js_set(js, lib, "rename", js_mkfun(builtin_fs_rename));
  js_set(js, lib, "rm", js_mkfun(builtin_fs_rm));
  js_set(js, lib, "unlink", js_mkfun(builtin_fs_unlink));
  js_set(js, lib, "mkdir", js_mkfun(builtin_fs_mkdir));
  js_set(js, lib, "mkdtemp", js_mkfun(builtin_fs_mkdtemp));
  js_set(js, lib, "rmdir", js_mkfun(builtin_fs_rmdir));
  js_set(js, lib, "stat", js_mkfun(builtin_fs_stat));
  js_set(js, lib, "lstat", js_mkfun(builtin_fs_lstat));
  js_set(js, lib, "fstat", js_mkfun(builtin_fs_fstat));
  js_set(js, lib, "utimes", js_mkfun(builtin_fs_utimes));
  js_set(js, lib, "futimes", js_mkfun(builtin_fs_futimes));
  js_set(js, lib, "exists", js_mkfun(builtin_fs_exists));
  js_set(js, lib, "access", js_mkfun(builtin_fs_access));
  js_set(js, lib, "chmod", js_mkfun(builtin_fs_chmod));
  js_set(js, lib, "readdir", js_mkfun(builtin_fs_readdir));
  js_set(js, lib, "realpath", js_mkfun(builtin_fs_realpath));
  js_set(js, lib, "readlink", js_mkfun(builtin_fs_readlink));
}

static void fs_set_callback_compatible_methods(ant_t *js, ant_value_t lib) {
  ant_value_t realpath = fs_make_callback_wrapper(js, js_mkfun(builtin_fs_realpath), false);

  js_set(js, lib, "appendFile", fs_make_callback_wrapper(js, js_mkfun(builtin_fs_appendFile), false));
  js_set(js, lib, "cp", fs_make_callback_wrapper(js, js_mkfun(builtin_fs_cp), false));
  js_set(js, lib, "copyFile", fs_make_callback_wrapper(js, js_mkfun(builtin_fs_copyFile), false));
  js_set(js, lib, "readFile", fs_make_callback_wrapper(js, js_mkfun(builtin_fs_readFile), false));
  js_set(js, lib, "open", fs_make_callback_wrapper(js, js_mkfun(builtin_fs_open_fd), false));
  js_set(js, lib, "close", fs_make_callback_wrapper(js, js_mkfun(builtin_fs_close_fd), false));
  js_set(js, lib, "writeFile", fs_make_callback_wrapper(js, js_mkfun(builtin_fs_writeFile), false));
  js_set(js, lib, "write", fs_make_callback_wrapper(js, js_mkfun(builtin_fs_write_fd), false));
  js_set(js, lib, "writev", fs_make_callback_wrapper(js, js_mkfun(builtin_fs_writev_fd), false));
  js_set(js, lib, "rename", fs_make_callback_wrapper(js, js_mkfun(builtin_fs_rename), false));
  js_set(js, lib, "rm", fs_make_callback_wrapper(js, js_mkfun(builtin_fs_rm), false));
  js_set(js, lib, "unlink", fs_make_callback_wrapper(js, js_mkfun(builtin_fs_unlink), false));
  js_set(js, lib, "mkdir", fs_make_callback_wrapper(js, js_mkfun(builtin_fs_mkdir), false));
  js_set(js, lib, "mkdtemp", fs_make_callback_wrapper(js, js_mkfun(builtin_fs_mkdtemp), false));
  js_set(js, lib, "rmdir", fs_make_callback_wrapper(js, js_mkfun(builtin_fs_rmdir), false));
  js_set(js, lib, "stat", fs_make_callback_wrapper(js, js_mkfun(builtin_fs_stat), false));
  js_set(js, lib, "lstat", fs_make_callback_wrapper(js, js_mkfun(builtin_fs_lstat), false));
  js_set(js, lib, "fstat", fs_make_callback_wrapper(js, js_mkfun(builtin_fs_fstat), false));
  js_set(js, lib, "utimes", fs_make_callback_wrapper(js, js_mkfun(builtin_fs_utimes), false));
  js_set(js, lib, "futimes", fs_make_callback_wrapper(js, js_mkfun(builtin_fs_futimes), false));
  js_set(js, lib, "exists", fs_make_callback_wrapper(js, js_mkfun(builtin_fs_exists), true));
  js_set(js, lib, "access", fs_make_callback_wrapper(js, js_mkfun(builtin_fs_access), false));
  js_set(js, lib, "chmod", fs_make_callback_wrapper(js, js_mkfun(builtin_fs_chmod), false));
  js_set(js, lib, "readdir", fs_make_callback_wrapper(js, js_mkfun(builtin_fs_readdir), false));
  js_set(js, lib, "readlink", fs_make_callback_wrapper(js, js_mkfun(builtin_fs_readlink), false));

  js_set(js, realpath, "native", realpath);
  js_set(js, lib, "realpath", realpath);
}

static ant_value_t fs_make_constants(ant_t *js) {
  ant_value_t constants = js_newobj(js);
  js_set(js, constants, "F_OK", js_mknum(F_OK));
  js_set(js, constants, "R_OK", js_mknum(R_OK));
  js_set(js, constants, "W_OK", js_mknum(W_OK));
  js_set(js, constants, "X_OK", js_mknum(X_OK));
  js_set(js, constants, "O_RDONLY", js_mknum(O_RDONLY));
  js_set(js, constants, "O_WRONLY", js_mknum(O_WRONLY));
  js_set(js, constants, "O_RDWR", js_mknum(O_RDWR));
  js_set(js, constants, "O_CREAT", js_mknum(O_CREAT));
  js_set(js, constants, "O_EXCL", js_mknum(O_EXCL));
  js_set(js, constants, "O_TRUNC", js_mknum(O_TRUNC));
  js_set(js, constants, "O_APPEND", js_mknum(O_APPEND));
#ifdef O_SYMLINK
  js_set(js, constants, "O_SYMLINK", js_mknum(O_SYMLINK));
#endif
#ifdef O_NOFOLLOW
  js_set(js, constants, "O_NOFOLLOW", js_mknum(O_NOFOLLOW));
#endif
#ifdef S_IFMT
  js_set(js, constants, "S_IFMT", js_mknum(S_IFMT));
#endif
#ifdef S_IFREG
  js_set(js, constants, "S_IFREG", js_mknum(S_IFREG));
#endif
#ifdef S_IFDIR
  js_set(js, constants, "S_IFDIR", js_mknum(S_IFDIR));
#endif
#ifdef S_IFLNK
  js_set(js, constants, "S_IFLNK", js_mknum(S_IFLNK));
#endif
  return constants;
}

static ant_value_t builtin_fs_promises_getter(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t getter_fn = js_getcurrentfunc(js);
  ant_value_t cached = js_get_slot(getter_fn, SLOT_DATA);
  if (is_object_type(cached)) return cached;

  ant_value_t promises = fs_promises_library(js);
  js_set_slot(getter_fn, SLOT_DATA, promises);
  return promises;
}

ant_value_t fs_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);
  ant_value_t realpath_sync = js_heavy_mkfun(js, builtin_fs_realpathSync, js_mkundef());
  
  fs_set_callback_compatible_methods(js, lib);
  fs_init_watch_constructors(js);
  fs_init_stream_constructors(js);

  js_set(js, lib, "read", js_mkfun(builtin_fs_read_fd));
  js_set(js, lib, "readFileSync", js_mkfun(builtin_fs_readFileSync));
  js_set(js, lib, "readSync", js_mkfun(builtin_fs_readSync));
  js_set(js, lib, "stream", js_mkfun(builtin_fs_readBytes));
  js_set(js, lib, "createReadStream", js_mkfun(builtin_fs_createReadStream));
  js_set(js, lib, "createWriteStream", js_mkfun(builtin_fs_createWriteStream));
  js_set(js, lib, "openSync", js_mkfun(builtin_fs_openSync));
  js_set(js, lib, "closeSync", js_mkfun(builtin_fs_closeSync));
  js_set(js, lib, "writeFileSync", js_mkfun(builtin_fs_writeFileSync));
  js_set(js, lib, "writeSync", js_mkfun(builtin_fs_writeSync));
  js_set(js, lib, "writevSync", js_mkfun(builtin_fs_writevSync));
  js_set(js, lib, "appendFileSync", js_mkfun(builtin_fs_appendFileSync));
  js_set(js, lib, "cpSync", js_mkfun(builtin_fs_cpSync));
  js_set(js, lib, "copyFileSync", js_mkfun(builtin_fs_copyFileSync));
  js_set(js, lib, "renameSync", js_mkfun(builtin_fs_renameSync));
  js_set(js, lib, "rmSync", js_mkfun(builtin_fs_rmSync));
  js_set(js, lib, "unlinkSync", js_mkfun(builtin_fs_unlinkSync));
  js_set(js, lib, "mkdirSync", js_mkfun(builtin_fs_mkdirSync));
  js_set(js, lib, "mkdtempSync", js_mkfun(builtin_fs_mkdtempSync));
  js_set(js, lib, "rmdirSync", js_mkfun(builtin_fs_rmdirSync));
  js_set(js, lib, "statSync", js_mkfun(builtin_fs_statSync));
  js_set(js, lib, "lstatSync", js_mkfun(builtin_fs_lstatSync));
  js_set(js, lib, "fstatSync", js_mkfun(builtin_fs_fstatSync));
  js_set(js, lib, "utimesSync", js_mkfun(builtin_fs_utimesSync));
  js_set(js, lib, "futimesSync", js_mkfun(builtin_fs_futimesSync));
  js_set(js, lib, "existsSync", js_mkfun(builtin_fs_existsSync));
  js_set(js, lib, "accessSync", js_mkfun(builtin_fs_accessSync));
  js_set(js, lib, "chmodSync", js_mkfun(builtin_fs_chmodSync));
  js_set(js, lib, "readdirSync", js_mkfun(builtin_fs_readdirSync));
  js_set(js, lib, "realpathSync", realpath_sync);
  js_set(js, lib, "readlinkSync", js_mkfun(builtin_fs_readlinkSync));
  js_set(js, lib, "watch", js_mkfun(builtin_fs_watch));
  js_set(js, lib, "watchFile", js_mkfun(builtin_fs_watchFile));
  js_set(js, lib, "unwatchFile", js_mkfun(builtin_fs_unwatchFile));
  js_set(js, lib, "FSWatcher", g_fswatcher_ctor);
  js_set(js, lib, "ReadStream", g_readstream_ctor);
  js_set(js, lib, "WriteStream", g_writestream_ctor);
  js_set(js, realpath_sync, "native", realpath_sync);
  
  js_set_getter_desc(
    js, lib,
    "promises", 8,
    js_heavy_mkfun(js, builtin_fs_promises_getter, js_mkundef()),
    JS_DESC_E | JS_DESC_C
  );
  
  js_set(js, lib, "constants", fs_make_constants(js));
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "fs", 2));
  
  return lib;
}

ant_value_t fs_promises_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);

  fs_set_promise_methods(js, lib);
  js_set(js, lib, "constants", fs_make_constants(js));

  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "fs/promises", 11));
  return lib;
}

ant_value_t fs_constants_library(ant_t *js) {
  ant_value_t constants = fs_make_constants(js);
  js_set(js, constants, "default", constants);
  js_set_slot_wb(js, constants, SLOT_DEFAULT, constants);
  js_set_sym(js, constants, get_toStringTag_sym(), js_mkstr(js, "constants", 9));
  return constants;
}

int has_pending_fs_ops(void) {
  return pending_requests && utarray_len(pending_requests) > 0;
}

void gc_mark_fs(ant_t *js, gc_mark_fn mark) {
  fs_watcher_t *watcher = NULL;

  if (g_fswatcher_proto) mark(js, g_fswatcher_proto);
  if (g_fswatcher_ctor) mark(js, g_fswatcher_ctor);
  if (g_readstream_proto) mark(js, g_readstream_proto);
  if (g_readstream_ctor) mark(js, g_readstream_ctor);
  if (g_writestream_proto) mark(js, g_writestream_proto);
  if (g_writestream_ctor) mark(js, g_writestream_ctor);
  if (!pending_requests) return;
  
  unsigned int len = utarray_len(pending_requests);
  for (unsigned int i = 0; i < len; i++) {
    fs_request_t **reqp = (fs_request_t **)utarray_eltptr(pending_requests, i);
    if (reqp && *reqp) {
    mark(js, (*reqp)->promise);
    if (is_object_type((*reqp)->target_buffer)) mark(js, (*reqp)->target_buffer);
    if (is_callable((*reqp)->callback_fn))      mark(js, (*reqp)->callback_fn);
  }}

  for (watcher = active_watchers; watcher; watcher = watcher->next_active) {
    if (vtype(watcher->obj) == T_OBJ) mark(js, watcher->obj);
    if (vtype(watcher->callback) != T_UNDEF) mark(js, watcher->callback);
  }
}
