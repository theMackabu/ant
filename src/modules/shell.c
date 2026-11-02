#include <uv.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <limits.h>
#include <poll.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

#include "ant.h"
#include "cfunc.h"
#include "debug.h"
#include "gc/roots.h"
#include "internal.h"
#include "modules/buffer.h"
#include "modules/shell.h"
#include "process_plan.h"
#include "ptr.h"

#define SH_READ_LIMIT (1 << 20)

static constexpr uint32_t SH_PLAN_TAG = Ant_NativeTag("SHPL");

typedef struct {
  ant_process_plan_t plan;
} sh_plan_t;

typedef struct {
  uv_fs_t request;
  ant_t *js;
  ant_value_t promise;
  char *path;
  char *data;
  size_t len;
  size_t offset;
  int fd;
} sh_fs_request_t;

#ifndef _WIN32
#ifndef RLIMIT_NPROC
#define RLIMIT_NPROC -1
#endif
#ifndef RLIMIT_MEMLOCK
#define RLIMIT_MEMLOCK -1
#endif
#ifndef RLIMIT_RSS
#define RLIMIT_RSS -1
#endif
#ifndef RLIMIT_RTTIME
#define RLIMIT_RTTIME -1
#endif
#ifndef RLIMIT_NICE
#define RLIMIT_NICE -1
#endif
#ifndef RLIMIT_SIGPENDING
#define RLIMIT_SIGPENDING -1
#endif
#ifndef RLIMIT_MSGQUEUE
#define RLIMIT_MSGQUEUE -1
#endif
#ifndef RLIMIT_RTPRIO
#define RLIMIT_RTPRIO -1
#endif
#ifndef RLIMIT_LOCKS
#define RLIMIT_LOCKS -1
#endif

static const int sh_limit_resources[] = {
  RLIMIT_CORE, RLIMIT_DATA, RLIMIT_FSIZE, RLIMIT_NOFILE,
  RLIMIT_STACK, RLIMIT_CPU, RLIMIT_NPROC, RLIMIT_AS,
  RLIMIT_MEMLOCK, RLIMIT_RSS, RLIMIT_RTTIME, RLIMIT_NICE,
  RLIMIT_SIGPENDING, RLIMIT_MSGQUEUE, RLIMIT_RTPRIO, RLIMIT_LOCKS,
};

#define SH_LIMIT_COUNT ((int)(sizeof(sh_limit_resources) / sizeof(sh_limit_resources[0])))
#endif

static void sh_plan_dispose(sh_plan_t *plan) {
  if (!plan) return;
  ant_process_plan_dispose(&plan->plan);
  free(plan);
}

static void sh_plan_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t holder = js_obj_from_ptr(obj);
  sh_plan_t *plan = js_get_native(holder, SH_PLAN_TAG);

  if (plan && plan->plan.has_spawned) {
    plan->plan.has_spawned = false;
    gc_unroot_pending_promise(js, js_obj_ptr(plan->plan.spawned));
  }

  sh_plan_dispose(plan);
  js_clear_native(holder, SH_PLAN_TAG);
}

static sh_plan_t *sh_plan_get(ant_value_t value) {
  return is_special_object(value) ? js_get_native(value, SH_PLAN_TAG) : NULL;
}

static sh_plan_t *sh_plan_arg(ant_value_t *args, int nargs) {
  return nargs > 0 ? sh_plan_get(args[0]) : NULL;
}

static bool sh_int_arg(ant_value_t value, int min, int max, int *out) {
  if (vtype(value) != kTypeNumber) return false;
  double number = Ant_Value_AsNumberUnchecked(value);
  if (number != number || number < min || number > max || (double)(int)number != number) return false;
  *out = (int)number;
  return true;
}

static int sh_mask_arg(ant_value_t *args, int nargs, int index) {
  int mask = -1;
  if (nargs > index && vtype(args[index]) == kTypeNumber) sh_int_arg(args[index], 0, 0777, &mask);
  return mask;
}

static bool sh_bytes_arg(ant_t *js, ant_value_t value, const char **data, size_t *len) {
  if (vtype(value) == kTypeString) {
    *data = js_getstr(js, value, len);
    return *data != NULL;
  }

  const uint8_t *bytes = NULL;
  if (!buffer_source_get_bytes(js, value, &bytes, len)) return false;
  *data = (const char *)bytes;

  return true;
}

static char *sh_copy_cstring(ant_t *js, ant_value_t value, ant_value_t *error) {
  size_t len = 0;
  const char *text = NULL;

  if (!sh_bytes_arg(js, value, &text, &len) || !text) {
    *error = js_mkerr_typed(js, JS_ERR_TYPE, "ant:shell: expected a string");
    return NULL;
  }

  if (memchr(text, '\0', len)) {
    *error = js_mkerr_typed(js, JS_ERR_TYPE, "ant:shell: command arguments cannot contain NUL bytes");
    return NULL;
  }

  char *copy = malloc(len + 1);
  if (!copy) {
    *error = Ant_Error_CreateOutOfMemory(js);
    return NULL;
  }

  memcpy(copy, text, len);
  copy[len] = '\0';

  return copy;
}

static void sh_free_strings(char **strings, size_t count) {
  if (!strings) return;
  for (size_t i = 0; i < count; i++) free(strings[i]);
  free(strings);
}

static char **sh_copy_string_array(ant_t *js, ant_value_t array, size_t *count, ant_value_t *error) {
  ant_offset_t len = js_arr_len(js, array);
  char **strings = calloc((size_t)len + 1, sizeof(*strings));
  if (!strings) {
    *error = Ant_Error_CreateOutOfMemory(js);
    return NULL;
  }

  for (ant_offset_t i = 0; i < len; i++) {
    if ((strings[i] = sh_copy_cstring(js, js_arr_get(js, array, i), error))) continue;
    sh_free_strings(strings, (size_t)i);
    return NULL;
  }

  *count = (size_t)len;
  return strings;
}

static void sh_close_sync(int fd) {
  uv_fs_t request;
  uv_fs_close(NULL, &request, fd, NULL);
  uv_fs_req_cleanup(&request);
}

static ant_value_t sh_error_text(ant_t *js, int error) {
  const char *text = uv_strerror(error);
  return js_mkstr(js, text, strlen(text));
}

static ant_value_t sh_fd_result(ant_t *js, ssize_t result) {
  return result < 0 ? sh_error_text(js, (int)result) : Ant_Value_FromNumber((double)result);
}

static ant_value_t sh_bytes_value(ant_t *js, const char *data, size_t len) {
  ArrayBufferData *buffer = create_array_buffer_data(js, len);
  if (!buffer) return sh_error_text(js, UV_ENOMEM);
  if (len) memcpy(buffer->data, data, len);
  return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, len, js->builtins.buffer_proto);
}

#ifndef _WIN32
static bool sh_limit_arg(ant_value_t value, int *resource) {
  int index = 0;
  if (!sh_int_arg(value, 0, SH_LIMIT_COUNT - 1, &index) || sh_limit_resources[index] < 0) return false;
  *resource = sh_limit_resources[index];
  return true;
}

static void sh_plan_add_limits(ant_t *js, ant_process_plan_t *plan, ant_value_t limits) {
  ant_offset_t len = js_arr_len(js, limits);

  for (ant_offset_t i = 0; i + 1 < len && plan->limit_count < ANT_PROCESS_FD_LIMIT; i += 2) {
    int resource = 0;
    ant_value_t value = js_arr_get(js, limits, i + 1);
    if (!sh_limit_arg(js_arr_get(js, limits, i), &resource) || vtype(value) != kTypeNumber) continue;

    plan->limit_resources[plan->limit_count] = resource;
    plan->limit_values[plan->limit_count] = Ant_Value_AsNumberUnchecked(value);
    plan->limit_count++;
  }
}
#endif

static ant_value_t sh_plan_wrap(ant_t *js, sh_plan_t *plan) {
  ant_value_t holder = Ant_Object_CreateBare(js);
  if (is_err(holder)) {
    sh_plan_dispose(plan);
    return holder;
  }

  js_set_native(holder, plan, SH_PLAN_TAG);
  if (js_get_native(holder, SH_PLAN_TAG) != plan) {
    sh_plan_dispose(plan);
    return Ant_Error_CreateOutOfMemory(js);
  }

  js_set_finalizer(holder, sh_plan_finalize);
  return holder;
}

static ant_value_t sh_ops_plan(ant_params_t) {
  if (nargs < 1 || vtype(args[0]) != kTypeString) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid shell cwd");

  ant_value_t error = Ant_Value_Undefined;
  char *cwd = sh_copy_cstring(js, args[0], &error);
  if (!cwd) return error;

  sh_plan_t *plan = calloc(1, sizeof(*plan));
  if (!plan) {
    free(cwd);
    return Ant_Error_CreateOutOfMemory(js);
  }

  ant_process_plan_init(&plan->plan);
  plan->plan.cwd = cwd;
  plan->plan.umask = sh_mask_arg(args, nargs, 1);

#ifndef _WIN32
  if (nargs > 2 && vtype(args[2]) == kTypeArray) sh_plan_add_limits(js, &plan->plan, args[2]);
#endif

  return sh_plan_wrap(js, plan);
}

static ant_value_t sh_ops_capture(ant_params_t) {
  sh_plan_t *plan = sh_plan_arg(args, nargs);
  if (!plan) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid shell plan");

  size_t index = 0;
  if (!ant_process_plan_add_capture(&plan->plan, &index)) return js_mkerr(js, "ant:shell: too many output captures");

  return Ant_Value_FromNumber((double)index);
}

static ant_value_t sh_ops_external(ant_params_t) {
  sh_plan_t *plan = sh_plan_arg(args, nargs);
  if (!plan || nargs < 3 || vtype(args[1]) != kTypeString || vtype(args[2]) != kTypeArray)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid shell command");

  ant_value_t error = Ant_Value_Undefined;
  char **argv = NULL, **env = NULL;
  size_t argc = 0, envc = 0;

  char *file = sh_copy_cstring(js, args[1], &error);
  if (!file) return error;
  if (!(argv = sh_copy_string_array(js, args[2], &argc, &error))) goto failed;

  if (argc == 0) {
    error = js_mkerr_typed(js, JS_ERR_TYPE, "Invalid shell command");
    goto failed;
  }

  if (nargs > 3 && vtype(args[3]) == kTypeArray && !(env = sh_copy_string_array(js, args[3], &envc, &error))) goto failed;
  if (ant_process_plan_take_external(&plan->plan, file, argv, argc, env)) return Ant_Value_Undefined;
  error = Ant_Error_CreateOutOfMemory(js);

failed:
  free(file);
  sh_free_strings(argv, argc);
  sh_free_strings(env, envc);
  return error;
}

static ant_value_t sh_ops_native(ant_params_t) {
  sh_plan_t *plan = sh_plan_arg(args, nargs);
  const char *out = NULL, *err = NULL;
  size_t out_len = 0, err_len = 0;
  int status = 0;

  bool valid = plan && nargs >= 4
    && sh_bytes_arg(js, args[1], &out, &out_len)
    && sh_bytes_arg(js, args[2], &err, &err_len)
    && sh_int_arg(args[3], 0, 255, &status);
  if (!valid) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid shell native stage");

  if (!ant_process_plan_add_native_stage(&plan->plan, out, out_len, err, err_len, status)) return Ant_Error_CreateOutOfMemory(js);
  return Ant_Value_Undefined;
}

static ant_value_t sh_ops_fd(ant_params_t) {
  sh_plan_t *plan = sh_plan_arg(args, nargs);
  int stage = 0, kind = 0, fd = 0, source = 0;

  bool valid = plan && nargs >= 5
    && sh_int_arg(args[1], -1, INT32_MAX, &stage)
    && sh_int_arg(args[2], 0, ANT_PROCESS_FD_ADOPT, &kind)
    && sh_int_arg(args[3], 0, ANT_PROCESS_FD_LIMIT - 1, &fd)
    && sh_int_arg(args[4], -1, INT32_MAX, &source)
    && (stage < 0 || (size_t)stage < plan->plan.command_count);
  if (!valid) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid shell redirection");

  ant_process_plan_command_t *command = stage >= 0 ? &plan->plan.commands[stage] : NULL;
  ant_value_t error = Ant_Value_Undefined;
  char *path = NULL;
  const char *data = NULL;
  size_t len = 0;

  if (kind <= ANT_PROCESS_FD_OPEN_EXCLUSIVE && (nargs < 6 || !(path = sh_copy_cstring(js, args[5], &error))))
    return is_err(error) ? error : js_mkerr_typed(js, JS_ERR_TYPE, "Invalid shell redirection");

  if (kind == ANT_PROCESS_FD_BYTES && (nargs < 6 || !sh_bytes_arg(js, args[5], &data, &len)))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid shell here-document");

  bool added = ant_process_plan_add_fd(&plan->plan, command, (ant_process_fd_kind_t)kind, fd, source, path, data, len);
  free(path);

  return added ? Ant_Value_Undefined : js_mkerr_typed(js, JS_ERR_TYPE, "Invalid shell redirection");
}

static ant_value_t sh_ops_spawned(ant_params_t) {
  sh_plan_t *plan = sh_plan_arg(args, nargs);
  if (!plan) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid shell plan");
  return ant_process_plan_spawned(js, &plan->plan);
}

static ant_value_t sh_ops_submit(ant_params_t) {
  sh_plan_t *plan = sh_plan_arg(args, nargs);
  if (!plan) return ant_process_plan_rejected_result(js, js_mkerr(js, "Invalid compiled process plan"));
  return ant_process_plan_submit(js, &plan->plan);
}

static sh_fs_request_t *sh_fs_request_new(ant_t *js, int fd) {
  sh_fs_request_t *request = calloc(1, sizeof(*request));
  if (!request) return NULL;

  request->js = js;
  request->fd = fd;
  request->promise = js_mkpromise(js);

  if (is_err(request->promise)) {
    free(request);
    return NULL;
  }

  request->request.data = request;
  gc_root_pending_promise(js, js_obj_ptr(request->promise));

  return request;
}

static bool sh_fs_request_alloc(sh_fs_request_t *request, const char *data, size_t len) {
  request->len = len;
  if (len == 0) return true;
  if (!(request->data = malloc(len))) return false;
  if (data) memcpy(request->data, data, len);
  return true;
}

static void sh_fs_request_settle(sh_fs_request_t *request, ant_value_t value) {
  ant_t *js = request->js;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, request->promise);
  GC_ROOT_PIN(js, value);
  js_resolve_promise(js, request->promise, value);
  GC_ROOT_RESTORE(js, root_mark);

  gc_unroot_pending_promise(js, js_obj_ptr(request->promise));
  free(request->path);
  free(request->data);
  free(request);
}

static void sh_open_done(uv_fs_t *uv_request) {
  sh_fs_request_t *request = uv_request->data;
  ssize_t result = uv_request->result;
  uv_fs_req_cleanup(uv_request);
  sh_fs_request_settle(request, sh_fd_result(request->js, result));
}

static ant_value_t sh_ops_open(ant_params_t) {
  int kind = 0;
  if (nargs < 2 || vtype(args[0]) != kTypeString || !sh_int_arg(args[1], 0, ANT_PROCESS_FD_OPEN_EXCLUSIVE, &kind))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid shell open");

  ant_value_t error = Ant_Value_Undefined;
  char *path = sh_copy_cstring(js, args[0], &error);
  if (!path) return error;

  sh_fs_request_t *request = sh_fs_request_new(js, -1);
  if (!request) {
    free(path);
    return Ant_Error_CreateOutOfMemory(js);
  }

  request->path = path;
  ant_value_t promise = request->promise;
  int flags = ant_process_open_flags((ant_process_fd_kind_t)kind);
  int mask = sh_mask_arg(args, nargs, 2);

  if (mask >= 0) {
    sh_fs_request_settle(request, sh_fd_result(js, ant_process_open_sync(path, flags, mask)));
    return promise;
  }

  int rc = uv_fs_open(js->loop, &request->request, path, flags, 0666, sh_open_done);
  if (rc < 0) {
    uv_fs_req_cleanup(&request->request);
    sh_fs_request_settle(request, sh_error_text(js, rc));
  }

  return promise;
}

static void sh_write_next(sh_fs_request_t *request);

static void sh_write_done(uv_fs_t *uv_request) {
  sh_fs_request_t *request = uv_request->data;
  ssize_t result = uv_request->result;
  uv_fs_req_cleanup(uv_request);

  if (result < 0) {
    sh_fs_request_settle(request, sh_error_text(request->js, (int)result));
    return;
  }

  request->offset += (size_t)result;
  sh_write_next(request);
}

static void sh_write_next(sh_fs_request_t *request) {
  if (request->offset >= request->len) {
    sh_fs_request_settle(request, Ant_Value_FromNumber(0));
    return;
  }

  size_t remaining = request->len - request->offset;
  uv_buf_t buffer = uv_buf_init(request->data + request->offset, (unsigned int)(remaining > UINT32_MAX ? UINT32_MAX : remaining));

  int rc = uv_fs_write(request->js->loop, &request->request, request->fd, &buffer, 1, -1, sh_write_done);
  if (rc >= 0) return;

  uv_fs_req_cleanup(&request->request);
  sh_fs_request_settle(request, sh_error_text(request->js, rc));
}

static bool sh_write_args(ant_t *js, ant_value_t *args, int nargs, int *fd, const char **data, size_t *len) {
  return nargs >= 2 && sh_int_arg(args[0], 0, INT32_MAX, fd) && sh_bytes_arg(js, args[1], data, len);
}

static bool sh_read_args(ant_value_t *args, int nargs, int *fd, int *max) {
  return nargs >= 2 && sh_int_arg(args[0], 0, INT32_MAX, fd) && sh_int_arg(args[1], 1, SH_READ_LIMIT, max);
}

static ant_value_t sh_ops_write(ant_params_t) {
  int fd = 0;
  const char *data = NULL;
  size_t len = 0;
  if (!sh_write_args(js, args, nargs, &fd, &data, &len)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid shell write");

  sh_fs_request_t *request = sh_fs_request_new(js, fd);
  if (!request) return Ant_Error_CreateOutOfMemory(js);

  if (!sh_fs_request_alloc(request, data, len)) {
    sh_fs_request_settle(request, Ant_Value_Undefined);
    return Ant_Error_CreateOutOfMemory(js);
  }

  ant_value_t promise = request->promise;
  sh_write_next(request);

  return promise;
}

static ant_value_t sh_ops_pipe(ant_params_t) {
  uv_file fds[2];
  int rc = uv_pipe(fds, 0, 0);
  if (rc < 0) return js_mkerr(js, "ant:shell: pipe: %s", uv_strerror(rc));

  ant_value_t pair = Ant_Array_Create(js);
  if (is_err(pair)) {
    sh_close_sync(fds[0]);
    sh_close_sync(fds[1]);
    return pair;
  }

  js_arr_push(js, pair, Ant_Value_FromNumber((double)fds[0]));
  js_arr_push(js, pair, Ant_Value_FromNumber((double)fds[1]));

  return pair;
}

static void sh_read_done(uv_fs_t *uv_request) {
  sh_fs_request_t *request = uv_request->data;
  ssize_t result = uv_request->result;
  uv_fs_req_cleanup(uv_request);

  ant_t *js = request->js;
  ant_value_t value = result < 0 ? sh_error_text(js, (int)result) : sh_bytes_value(js, request->data, (size_t)result);
  sh_fs_request_settle(request, value);
}

static ant_value_t sh_ops_read(ant_params_t) {
  int fd = 0, max = 0;
  if (!sh_read_args(args, nargs, &fd, &max)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid shell read");

  sh_fs_request_t *request = sh_fs_request_new(js, fd);
  if (!request) return Ant_Error_CreateOutOfMemory(js);

  if (!sh_fs_request_alloc(request, NULL, (size_t)max)) {
    sh_fs_request_settle(request, Ant_Value_Undefined);
    return Ant_Error_CreateOutOfMemory(js);
  }

  ant_value_t promise = request->promise;
  uv_buf_t buffer = uv_buf_init(request->data, (unsigned int)max);

  int rc = uv_fs_read(js->loop, &request->request, fd, &buffer, 1, -1, sh_read_done);
  if (rc < 0) {
    uv_fs_req_cleanup(&request->request);
    sh_fs_request_settle(request, sh_error_text(js, rc));
  }

  return promise;
}

#ifndef _WIN32
static rlim_t sh_rlimit_value(double value, rlim_t current) {
  if (value >= 0) return (rlim_t)value;
  if (value == -1) return RLIM_INFINITY;
  return current;
}

static double sh_rlimit_number(rlim_t value) {
  return value == RLIM_INFINITY ? -1 : (double)value;
}

static int sh_rlimit_set(int resource, struct rlimit *limit, ant_value_t soft, ant_value_t hard) {
  limit->rlim_cur = sh_rlimit_value(Ant_Value_AsNumberUnchecked(soft), limit->rlim_cur);
  limit->rlim_max = sh_rlimit_value(Ant_Value_AsNumberUnchecked(hard), limit->rlim_max);
  return setrlimit(resource, limit) == 0 ? 0 : errno;
}
#endif

static ant_value_t sh_ops_rlimit(ant_params_t) {
#ifdef _WIN32
  return Ant_Value_Undefined;
#else
  int resource = 0;
  struct rlimit limit;
  if (nargs < 1 || !sh_limit_arg(args[0], &resource) || getrlimit(resource, &limit) != 0) return Ant_Value_Undefined;

  if (nargs >= 3 && vtype(args[1]) == kTypeNumber && vtype(args[2]) == kTypeNumber) {
    int error = sh_rlimit_set(resource, &limit, args[1], args[2]);
    if (error) return js_mkstr(js, strerror(error), strlen(strerror(error)));
  }

  ant_value_t pair = Ant_Array_Create(js);
  if (is_err(pair)) return pair;

  js_arr_push(js, pair, Ant_Value_FromNumber(sh_rlimit_number(limit.rlim_cur)));
  js_arr_push(js, pair, Ant_Value_FromNumber(sh_rlimit_number(limit.rlim_max)));

  return pair;
#endif
}

#ifndef _WIN32
typedef struct sh_io_op {
  struct sh_io_op *next;
  sh_fs_request_t *request;
  ssize_t result;
  bool write;
} sh_io_op_t;

typedef struct sh_io_worker {
  struct sh_io_worker *next;
  uv_loop_t *loop;
  uv_async_t async;
  uv_mutex_t lock;
  uv_thread_t thread;
  int wake[2];
  sh_io_op_t *active;
  sh_io_op_t *completed;
  size_t pending;
} sh_io_worker_t;

typedef struct {
  struct pollfd *fds;
  sh_io_op_t **ops;
  size_t capacity;
  size_t count;
} sh_io_pollset_t;

static uv_once_t sh_io_once = UV_ONCE_INIT;
static uv_mutex_t sh_io_registry;
static sh_io_worker_t *sh_io_workers;

static void sh_io_registry_init(void) {
  uv_mutex_init(&sh_io_registry);
}

static void sh_io_wake(sh_io_worker_t *worker) {
  char byte = 1;
  (void)!write(worker->wake[1], &byte, 1);
}

static void sh_io_drain(int fd) {
  char drain[64];
  while (read(fd, drain, sizeof(drain)) > 0);
}

static void sh_io_reserve(sh_io_pollset_t *set, size_t count) {
  if (count <= set->capacity) return;

  size_t next = set->capacity ? set->capacity * 2 : 16;
  while (next < count) next *= 2;

  struct pollfd *fds = realloc(set->fds, next * sizeof(*fds));
  if (!fds) return;
  set->fds = fds;

  sh_io_op_t **ops = realloc(set->ops, next * sizeof(*ops));
  if (!ops) return;
  set->ops = ops;
  set->capacity = next;
}

static void sh_io_collect(sh_io_worker_t *worker, sh_io_pollset_t *set) {
  size_t count = 1;
  for (sh_io_op_t *op = worker->active; op; op = op->next) count++;
  sh_io_reserve(set, count);

  set->count = 1;
  set->fds[0] = (struct pollfd){ .fd = worker->wake[0], .events = POLLIN };

  for (sh_io_op_t *op = worker->active; op && set->count < set->capacity; op = op->next) {
    set->fds[set->count] = (struct pollfd){ .fd = op->request->fd, .events = op->write ? POLLOUT : POLLIN };
    set->ops[set->count++] = op;
  }
}

static sh_io_op_t **sh_io_find(sh_io_worker_t *worker, sh_io_op_t *op) {
  sh_io_op_t **link = &worker->active;
  while (*link && *link != op) link = &(*link)->next;
  return *link ? link : NULL;
}

static void sh_io_complete(sh_io_worker_t *worker, sh_io_op_t **link) {
  sh_io_op_t *op = *link;
  *link = op->next;
  op->next = worker->completed;
  worker->completed = op;
}

static bool sh_io_read_op(sh_io_op_t *op) {
  sh_fs_request_t *request = op->request;
  ssize_t got;

  do got = read(request->fd, request->data, request->len);
  while (got < 0 && errno == EINTR);

  op->result = got < 0 ? -errno : got;
  return true;
}

static bool sh_io_write_op(sh_io_op_t *op) {
  sh_fs_request_t *request = op->request;
  size_t remaining = request->len - request->offset;
  size_t chunk = remaining > PIPE_BUF ? PIPE_BUF : remaining;
  ssize_t wrote;

  do wrote = write(request->fd, request->data + request->offset, chunk);
  while (wrote < 0 && errno == EINTR);

  if (wrote < 0) {
    op->result = -errno;
    return true;
  }

  request->offset += (size_t)wrote;
  if (request->offset < request->len) return false;
  op->result = 0;

  return true;
}

static bool sh_io_perform(sh_io_op_t *op, short revents) {
  if (revents & POLLNVAL) {
    op->result = -EBADF;
    return true;
  }
  return op->write ? sh_io_write_op(op) : sh_io_read_op(op);
}

static bool sh_io_dispatch(sh_io_worker_t *worker, sh_io_pollset_t *set) {
  bool finished = false;

  for (size_t i = 1; i < set->count; i++) {
    short revents = set->fds[i].revents;
    if (!revents) continue;

    sh_io_op_t **link = sh_io_find(worker, set->ops[i]);
    if (!link || !sh_io_perform(*link, revents)) continue;

    sh_io_complete(worker, link);
    finished = true;
  }

  return finished;
}

static void sh_io_worker_main(void *arg) {
  sh_io_worker_t *worker = arg;
  sh_io_pollset_t set = {0};

  for (;;) {
    uv_mutex_lock(&worker->lock);
    sh_io_collect(worker, &set);
    uv_mutex_unlock(&worker->lock);

    if (poll(set.fds, (nfds_t)set.count, -1) < 0 && errno != EINTR) continue;
    if (set.fds[0].revents) sh_io_drain(worker->wake[0]);

    uv_mutex_lock(&worker->lock);
    bool finished = sh_io_dispatch(worker, &set);
    uv_mutex_unlock(&worker->lock);

    if (finished) uv_async_send(&worker->async);
  }
}

static ant_value_t sh_io_result(ant_t *js, sh_io_op_t *op) {
  if (op->result < 0) return sh_error_text(js, uv_translate_sys_error((int)-op->result));
  if (op->write) return Ant_Value_FromNumber(0);
  return sh_bytes_value(js, op->request->data, (size_t)op->result);
}

static void sh_io_async(uv_async_t *handle) {
  sh_io_worker_t *worker = handle->data;

  uv_mutex_lock(&worker->lock);
  sh_io_op_t *list = worker->completed;
  worker->completed = NULL;
  uv_mutex_unlock(&worker->lock);

  while (list) {
    sh_io_op_t *op = list;
    list = op->next;

    sh_fs_request_t *request = op->request;
    ant_value_t value = sh_io_result(request->js, op);
    free(op);

    sh_fs_request_settle(request, value);
    if (worker->pending > 0 && --worker->pending == 0) uv_unref((uv_handle_t *)&worker->async);
  }
}

static bool sh_io_worker_start(sh_io_worker_t *worker) {
  if (uv_mutex_init(&worker->lock) != 0) return false;
  if (uv_pipe(worker->wake, UV_NONBLOCK_PIPE, UV_NONBLOCK_PIPE) != 0) return false;
  if (uv_async_init(worker->loop, &worker->async, sh_io_async) != 0) return false;

  worker->async.data = worker;
  uv_unref((uv_handle_t *)&worker->async);

  return uv_thread_create(&worker->thread, sh_io_worker_main, worker) == 0;
}

static sh_io_worker_t *sh_io_worker_create(uv_loop_t *loop) {
  sh_io_worker_t *worker = calloc(1, sizeof(*worker));
  if (!worker) return NULL;

  worker->loop = loop;
  if (!sh_io_worker_start(worker)) {
    free(worker);
    return NULL;
  }

  worker->next = sh_io_workers;
  sh_io_workers = worker;

  return worker;
}

static sh_io_worker_t *sh_io_worker_lookup(uv_loop_t *loop, bool create) {
  uv_once(&sh_io_once, sh_io_registry_init);
  uv_mutex_lock(&sh_io_registry);

  sh_io_worker_t *worker = sh_io_workers;
  while (worker && worker->loop != loop) worker = worker->next;
  if (!worker && create) worker = sh_io_worker_create(loop);

  uv_mutex_unlock(&sh_io_registry);
  return worker;
}

static ant_value_t sh_io_submit(ant_t *js, sh_fs_request_t *request, bool writing) {
  ant_value_t promise = request->promise;
  sh_io_worker_t *worker = sh_io_worker_lookup(js->loop, true);
  sh_io_op_t *op = worker ? calloc(1, sizeof(*op)) : NULL;

  if (!op) {
    sh_fs_request_settle(request, sh_error_text(js, UV_ENOMEM));
    return promise;
  }

  op->request = request;
  op->write = writing;
  if (worker->pending++ == 0) uv_ref((uv_handle_t *)&worker->async);

  uv_mutex_lock(&worker->lock);
  op->next = worker->active;
  worker->active = op;
  uv_mutex_unlock(&worker->lock);

  sh_io_wake(worker);
  return promise;
}

static bool sh_io_cancel(sh_io_worker_t *worker, int fd) {
  bool cancelled = false;

  for (sh_io_op_t **link = &worker->active; *link;) {
    if ((*link)->request->fd != fd) {
      link = &(*link)->next;
      continue;
    }
    (*link)->result = -EBADF;
    sh_io_complete(worker, link);
    cancelled = true;
  }

  return cancelled;
}

static void sh_io_close(sh_io_worker_t *worker, int fd) {
  uv_mutex_lock(&worker->lock);
  bool cancelled = sh_io_cancel(worker, fd);
  close(fd);
  uv_mutex_unlock(&worker->lock);

  if (cancelled) uv_async_send(&worker->async);
  sh_io_wake(worker);
}
#endif

static ant_value_t sh_ops_pread(ant_params_t) {
#ifdef _WIN32
  return sh_ops_read(js, args, nargs, call_new_target, call_this, call_func);
#else
  int fd = 0, max = 0;
  if (!sh_read_args(args, nargs, &fd, &max)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid shell read");

  sh_fs_request_t *request = sh_fs_request_new(js, fd);
  if (!request) return Ant_Error_CreateOutOfMemory(js);

  if (!sh_fs_request_alloc(request, NULL, (size_t)max)) {
    ant_value_t promise = request->promise;
    sh_fs_request_settle(request, sh_error_text(js, UV_ENOMEM));
    return promise;
  }

  return sh_io_submit(js, request, false);
#endif
}

static ant_value_t sh_ops_pwrite(ant_params_t) {
#ifdef _WIN32
  return sh_ops_write(js, args, nargs, call_new_target, call_this, call_func);
#else
  int fd = 0;
  const char *data = NULL;
  size_t len = 0;
  if (!sh_write_args(js, args, nargs, &fd, &data, &len)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid shell write");

  sh_fs_request_t *request = sh_fs_request_new(js, fd);
  if (!request) return Ant_Error_CreateOutOfMemory(js);
  ant_value_t promise = request->promise;

  if (len == 0) {
    sh_fs_request_settle(request, Ant_Value_FromNumber(0));
    return promise;
  }

  if (!sh_fs_request_alloc(request, data, len)) {
    sh_fs_request_settle(request, sh_error_text(js, UV_ENOMEM));
    return promise;
  }

  return sh_io_submit(js, request, true);
#endif
}

static ant_value_t sh_ops_close(ant_params_t) {
  int fd = 0;
  if (nargs < 1 || !sh_int_arg(args[0], 0, INT32_MAX, &fd)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid shell close");

#ifndef _WIN32
  sh_io_worker_t *worker = sh_io_worker_lookup(js->loop, false);
  if (worker) {
    sh_io_close(worker, fd);
    return Ant_Value_Undefined;
  }
#endif

  sh_close_sync(fd);
  return Ant_Value_Undefined;
}

ant_value_t shell_ops_library(ant_t *js) {
  ant_value_t lib = Ant_Object_CreateBare(js);

  js_set(js, lib, "plan", Ant_Function_Create(sh_ops_plan));
  js_set(js, lib, "capture", Ant_Function_Create(sh_ops_capture));
  js_set(js, lib, "external", Ant_Function_Create(sh_ops_external));
  js_set(js, lib, "native", Ant_Function_Create(sh_ops_native));
  js_set(js, lib, "fd", Ant_Function_Create(sh_ops_fd));
  js_set(js, lib, "submit", Ant_Function_Create(sh_ops_submit));
  js_set(js, lib, "spawned", Ant_Function_Create(sh_ops_spawned));
  js_set(js, lib, "open", Ant_Function_Create(sh_ops_open));
  js_set(js, lib, "write", Ant_Function_Create(sh_ops_write));
  js_set(js, lib, "close", Ant_Function_Create(sh_ops_close));
  js_set(js, lib, "pipe", Ant_Function_Create(sh_ops_pipe));
  js_set(js, lib, "read", Ant_Function_Create(sh_ops_read));
  js_set(js, lib, "rlimit", Ant_Function_Create(sh_ops_rlimit));
  js_set(js, lib, "pread", Ant_Function_Create(sh_ops_pread));
  js_set(js, lib, "pwrite", Ant_Function_Create(sh_ops_pwrite));
  js_set(js, lib, "debugEnabled", sv_debug_unlikely(SV_DEBUG_DUMP_SHELL) ? Ant_Value_True : Ant_Value_False);
  js_set_tostringtag(js, lib, js_mkstr(js, "shell ops", 9));

  return lib;
}
