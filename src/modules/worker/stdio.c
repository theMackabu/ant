#include <compat.h> // IWYU pragma: keep
#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "cfunc.h"
#include "errors.h"
#include "internal.h"
#include "output.h"
#include "silver/call.h"

#include "modules/buffer.h"
#include "modules/events.h"
#include "modules/stream.h"
#include "modules/timer.h"
#include "worker_internal.h"

bool worker_stdio_writer(FILE *stream, const void *data, size_t len, void *user) {
  worker_event_t *e = malloc(sizeof(*e) + len);
  if (!e) return false;
  *e = (worker_event_t){ .kind = stream == stderr ? WORKER_EVENT_STDERR : WORKER_EVENT_STDOUT, .len = len };
  memcpy(e->bytes, data, len);
  worker_post_event(user, e);
  return true;
}

static ant_value_t worker_stdio_bytes(ant_t *js, const char *bytes, size_t len) {
  ArrayBufferData *ab = create_array_buffer_data(js, len);
  if (!ab) return Ant_Error_CreateOutOfMemory(js);
  if (len) memcpy(ab->data, bytes, len);
  return create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, len, js->builtins.buffer_proto);
}

static ant_value_t worker_make_readable(ant_t *js, ant_value_t read) {
  ant_value_t obj = Ant_Object_CreateBare(js);
  if (is_err(obj)) return obj;
  js_set_proto_init(obj, stream_readable_prototype(js));
  stream_init_readable_object(js, obj, js_mkundef());
  js_set(js, obj, "_read", read);
  return obj;
}

void worker_stdio_child_wake(ant_t *js, ant_worker_t *w) {
  if (!is_object_type(w->child_stdin) || w->child_stdin_done) return;
  ant_mutex_lock(&w->lock);

  worker_event_t *e = w->stdin_head;
  bool ended = w->stdin_ended || !w->stdin_enabled;
  w->stdin_head = w->stdin_tail = NULL;
  ant_mutex_unlock(&w->lock);

  GC_ROOT_SAVE(mark, js);
  ant_value_t stream = w->child_stdin;
  GC_ROOT_PIN(js, stream);

  for (worker_event_t *next; e; e = next) {
    next = e->next;
    ant_value_t chunk = worker_stdio_bytes(js, e->bytes, e->len);
    free(e);
    if (is_err(chunk)) js_take_thrown(js, chunk);
    else stream_readable_push(js, stream, chunk, js_mkundef());
  }

  if (ended) {
    w->child_stdin_done = true;
    if (w->child_stdin_reading) uv_unref((uv_handle_t *)&w->child_async);
    w->child_stdin_reading = false;
    stream_readable_push(js, stream, js_mknull(), js_mkundef());
  }

  GC_ROOT_RESTORE(js, mark);
  process_microtasks(js);
}

static ant_value_t worker_stdin_read(ant_params_t) {
  ant_worker_t *w = js->worker;
  if (!w || w->child_stdin_done || w->child_stdin_reading || !w->child_async_open) return js_mkundef();
  w->child_stdin_reading = true;
  uv_ref((uv_handle_t *)&w->child_async);
  return js_mkundef();
}

void worker_stdio_child_init(ant_t *js, ant_worker_t *w) {
  ant_value_t stream = worker_make_readable(js, Ant_Function_Create(worker_stdin_read));
  if (is_err(stream)) {
    js_take_thrown(js, stream);
    return;
  }
  w->child_stdin = stream;
  js_set(js, js_get(js, js_glob(js), "process"), "stdin", stream);
  worker_stdio_child_wake(js, w);
}

static void worker_stdin_call(ant_t *js, ant_value_t callback) {
  if (!is_callable(callback)) return;
  ant_value_t r = sv_vm_call(js->vm, js, callback, js_mkundef(), NULL, 0, NULL, js_mkundef());
  if (is_err(r)) js_take_thrown(js, r);
}

static void worker_stdin_enqueue(ant_worker_t *w, worker_event_t *e) {
  ant_mutex_lock(&w->lock);
  if (e && !w->stdin_ended) {
    if (w->stdin_tail) w->stdin_tail->next = e;
    else w->stdin_head = e;
    w->stdin_tail = e;
    e = NULL;
  } else if (!e) w->stdin_ended = true;
  if (w->child_async_open) uv_async_send(&w->child_async);
  ant_mutex_unlock(&w->lock);
  free(e);
}

static ant_value_t worker_stdin_write(ant_params_t) {
  ant_worker_t *w = worker_data(js_func_data(js->current_func));
  ant_value_t chunk = nargs > 0 ? args[0] : js_mkundef();
  const uint8_t *bytes = NULL;
  size_t len = 0;

  if (vtype(chunk) == kTypeString) bytes = (const uint8_t *)js_getstr(js, chunk, &len);
  else if (!buffer_source_get_bytes(js, chunk, &bytes, &len))
    return Ant_Error_CreateInvalidArgType(js, "chunk", "of type string or an instance of Buffer or Uint8Array", chunk);

  if (w && !w->exited) {
    worker_event_t *e = malloc(sizeof(*e) + len);
    if (!e) return Ant_Error_CreateOutOfMemory(js);
    *e = (worker_event_t){ .kind = WORKER_EVENT_STDIN, .len = len };
    if (len) memcpy(e->bytes, bytes, len);
    worker_stdin_enqueue(w, e);
  }

  worker_stdin_call(js, nargs > 2 ? args[2] : js_mkundef());
  return js_mkundef();
}

static ant_value_t worker_stdin_final(ant_params_t) {
  ant_worker_t *w = worker_data(js_func_data(js->current_func));
  if (w) worker_stdin_enqueue(w, NULL);
  worker_stdin_call(js, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static ant_value_t worker_make_stdin(ant_t *js, ant_value_t worker_obj) {
  ant_value_t obj = Ant_Object_CreateBare(js);
  if (is_err(obj)) return obj;
  js_set_proto_init(obj, stream_writable_prototype(js));
  stream_init_writable_object(js, obj, js_mkundef());
  js_set(js, obj, "_write", js_heavy_mkfun(js, worker_stdin_write, worker_obj));
  js_set(js, obj, "_final", js_heavy_mkfun(js, worker_stdin_final, worker_obj));
  return obj;
}

void worker_stdio_parent_init(ant_t *js, ant_worker_t *w) {
  w->stdout_obj = worker_make_readable(js, Ant_Function_Create(js_cfunc_noop));
  w->stderr_obj = worker_make_readable(js, Ant_Function_Create(js_cfunc_noop));
  if (w->stdin_enabled) w->stdin_obj = worker_make_stdin(js, w->obj);
}

void worker_stdio_forward(ant_t *js, ant_worker_t *w, worker_event_t *e) {
  bool is_stderr = e->kind == WORKER_EVENT_STDERR;
  ant_value_t stream = is_stderr ? w->stderr_obj : w->stdout_obj;
  bool pipe = is_stderr ? w->pipe_stderr : w->pipe_stdout;

  if (pipe) {
    ant_output_stream_t *out = ant_output_stream(is_stderr ? stderr : stdout);
    ant_output_stream_begin(out);
    if (ant_output_stream_append(out, e->bytes, e->len)) ant_output_stream_flush(out);
  }

  if (!is_object_type(stream) || (pipe && eventemitter_listener_count(js, stream, "data") == 0)) return;
  ant_value_t chunk = worker_stdio_bytes(js, e->bytes, e->len);
  if (is_err(chunk)) {
    js_take_thrown(js, chunk);
    return;
  }

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, chunk);
  stream_readable_push(js, stream, chunk, js_mkundef());
  GC_ROOT_RESTORE(js, mark);
  process_microtasks(js);
}

void worker_stdio_end(ant_t *js, ant_worker_t *w) {
  if (is_object_type(w->stdout_obj)) stream_readable_push(js, w->stdout_obj, js_mknull(), js_mkundef());
  if (is_object_type(w->stderr_obj)) stream_readable_push(js, w->stderr_obj, js_mknull(), js_mkundef());
}
