#ifndef ANT_WORKER_WORKER_INTERNAL_H
#define ANT_WORKER_WORKER_INTERNAL_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <uv.h>

#include "sync.h"
#include "types.h"
#include "vfs_bundle.h"
#include "gc/modules.h"
#include "modules/message_port.h"
#include "modules/wire.h"

typedef enum {
  WORKER_EVENT_ONLINE,
  WORKER_EVENT_ERROR,
  WORKER_EVENT_STDOUT,
  WORKER_EVENT_STDERR,
  WORKER_EVENT_STDIN,
  WORKER_EVENT_EXIT,
} worker_event_kind_t;

typedef enum {
  WORKER_ERROR_THROWN,
  WORKER_ERROR_INIT,
  WORKER_ERROR_OOM,
} worker_error_kind_t;

typedef enum {
  WORKER_ENTRY_NODE,
  WORKER_ENTRY_WEB,
} worker_entry_mode_t;

typedef struct worker_event {
  struct worker_event *next;
  worker_event_kind_t kind;
  int code;
  ant_wire_t *wire;
  size_t len;
  char bytes[];
} worker_event_t;

enum {
  WORKER_LIMIT_YOUNG,
  WORKER_LIMIT_OLD,
  WORKER_LIMIT_CODE,
  WORKER_LIMIT_STACK,
  WORKER_LIMIT_COUNT,
};

typedef struct {
  ant_port_message_fn on_message;
  void (*online)(ant_t *js, ant_worker_t *w);
  void (*error)(ant_t *js, ant_worker_t *w, ant_value_t error);
  void (*exit)(ant_t *js, ant_worker_t *w, int code);
  void (*child_init)(ant_t *js, ant_worker_t *w);
} worker_ops_t;

struct ant_worker {
  _Atomic int refs;
  ant_mutex_t lock;
  uint64_t thread_id;
  const worker_ops_t *ops;

  ant_t *parent;
  const ant_bundle_t *bundle;

  ant_value_t obj;
  ant_value_t exit_promise;
  uv_async_t parent_async;

  bool parent_async_open;
  bool refed;
  bool exited;
  bool pipe_stdout;
  bool pipe_stderr;
  bool stdin_enabled;
  bool stdin_ended;

  int exit_code;
  ant_value_t stdin_obj;
  ant_value_t stdout_obj;
  ant_value_t stderr_obj;
  ant_port_binding_t *binding;

  worker_event_t *events_head;
  worker_event_t *events_tail;
  worker_event_t *stdin_head;
  worker_event_t *stdin_tail;
  ant_worker_t *next;
  ant_worker_t **pprev;

  uv_thread_t thread;
  bool thread_started;
  ant_t *child;
  _Atomic uint64_t loop_start_ns;
  uv_async_t child_async;
  bool child_async_open;

  ant_value_t child_stdin;
  bool child_stdin_reading;
  bool child_stdin_done;

  bool terminate_requested;
  int requested_exit_code;
  ant_port_end_t *child_end;

  char *entry;
  bool is_eval;
  int argc;
  char **argv;
  char *name;
  char **env;
  bool share_env;

  ant_wire_t *worker_data;
  ant_wire_t *environment_data;
  double limits[WORKER_LIMIT_COUNT];
  double requested_limits[WORKER_LIMIT_COUNT];
};

extern const char *const worker_limit_names[WORKER_LIMIT_COUNT];

ant_worker_t *worker_new(ant_t *js, ant_value_t obj, const worker_ops_t *ops);
ant_worker_t *worker_data(ant_value_t obj);
void worker_release(ant_worker_t *w);

void worker_post(ant_worker_t *w, worker_event_kind_t kind, int code, ant_wire_t *wire);
void worker_post_event(ant_worker_t *w, worker_event_t *e);
void worker_request_stop(ant_worker_t *w, int code);
bool worker_stop_requested(ant_worker_t *w, int *code);
void worker_set_ref(ant_worker_t *w, bool refed);
void worker_flush_online(ant_t *js, ant_worker_t *w);

char *worker_resolve_entry(ant_t *js, ant_value_t spec, worker_entry_mode_t mode, ant_value_t *error);
ant_value_t worker_read_options(ant_t *js, ant_worker_t *w, ant_value_t options);
ant_value_t worker_limits_object(ant_t *js, const double limits[WORKER_LIMIT_COUNT], double unset);

ant_value_t worker_environment_map(ant_t *js);
ant_value_t worker_transfer_arg(ant_native_params_t);
ant_value_t worker_start(ant_t *js, ant_worker_t *w);
ant_value_t worker_post_message_to_thread(ant_native_params_t);

ant_value_t worker_parent_port(ant_t *js);
ant_value_t worker_error_value(ant_t *js, worker_event_t *e);

bool worker_stdio_writer(FILE *stream, const void *data, size_t len, void *user);
void worker_stdio_child_init(ant_t *js, ant_worker_t *w);
void worker_stdio_child_wake(ant_t *js, ant_worker_t *w);
void worker_stdio_parent_init(ant_t *js, ant_worker_t *w);
void worker_stdio_forward(ant_t *js, ant_worker_t *w, worker_event_t *e);
void worker_stdio_end(ant_t *js, ant_worker_t *w);

void worker_messaging_close(ant_t *js);
void worker_messaging_gc_mark(ant_t *js, gc_mark_fn mark);

#endif
