#ifndef ANT_ISOLATE_INTERNAL_H
#define ANT_ISOLATE_INTERNAL_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <uv.h>

#include "types.h"
#include "gc/modules.h"
#include "modules/wire.h"

typedef enum {
  GUEST_THREAD_OWN,
  GUEST_THREAD_CURRENT,
} guest_thread_t;

typedef enum {
  GUEST_GLOBALS_BARE,
  GUEST_GLOBALS_ANT,
} guest_globals_t;

typedef enum {
  GUEST_OP_CONTEXT,
  GUEST_OP_MAIN_CONTEXT,
  GUEST_OP_COMPILE,
  GUEST_OP_RUN,
  GUEST_OP_EVAL,
  GUEST_OP_GET,
  GUEST_OP_SET,
  GUEST_OP_APPLY,
  GUEST_OP_COPY,
  GUEST_OP_HEAP,
} guest_op_t;

typedef enum {
  HANDLE_CONTEXT,
  HANDLE_SCRIPT,
  HANDLE_REFERENCE,
} isolate_handle_kind_t;

typedef struct {
  isolate_handle_kind_t kind;
  uint32_t id;
} guest_released_t;

typedef enum {
  GUEST_VALUE_UNDEFINED,
  GUEST_VALUE_WIRE,
  GUEST_VALUE_REF,
  GUEST_VALUE_CALLBACK,
} guest_value_kind_t;

typedef struct {
  guest_value_kind_t kind;
  ant_wire_t *wire;
  uint32_t id;
  uint8_t type;
} guest_value_t;

typedef enum {
  GUEST_STATUS_PENDING,
  GUEST_STATUS_OK,
  GUEST_STATUS_THROW,
  GUEST_STATUS_DISPOSED,
  GUEST_STATUS_OOM,
} guest_status_t;

typedef struct guest_call guest_call_t;
typedef struct guest_host_call guest_host_call_t;
typedef struct ant_guest ant_guest_t;

struct guest_call {
  guest_call_t *next;
  guest_call_t *pending_next;
  guest_op_t op;
  uint32_t target;
  uint32_t context;
  char *text;
  size_t text_len;
  char *filename;
  guest_value_t *args;
  uint32_t argc;
  uint32_t timeout_ms;
  bool want_ref;
  bool await_promise;

  guest_status_t status;
  guest_value_t result;
  uint32_t result_aux;
  double stats[4];

  bool sync;
  bool done;
  ant_value_t promise;
  ant_value_t owner;
};

struct guest_host_call {
  guest_host_call_t *next;
  guest_host_call_t *awaiting_next;
  uint32_t callback;
  guest_value_t *args;
  uint32_t argc;
  guest_status_t status;
  guest_value_t result;
  bool awaiting;
  bool done;
  bool taken;
  bool abandoned;
  ant_value_t promise;
};

typedef struct {
  char *source;
  size_t len;
  char *filename;
  uint32_t *compiled;
  uint32_t compiled_cap;
} guest_script_t;

struct ant_guest {
  _Atomic int refs;
  uv_mutex_t lock;
  uv_cond_t cond;

  guest_thread_t mode;
  guest_globals_t globals;
  size_t arena_bytes;

  ant_t *host;
  ant_t *child;
  ant_value_t obj;
  bool disposed;
  bool oom;
  bool started;
  bool failed;

  uv_thread_t thread;
  bool thread_started;
  uv_async_t child_async;
  bool child_async_open;
  uv_async_t host_async;
  bool host_async_open;

  guest_call_t *calls_head, *calls_tail;
  guest_call_t *done_head, *done_tail;
  guest_host_call_t *host_head, *host_tail;
  guest_call_t *pending;
  uint32_t pending_count;
  guest_released_t *released;
  uint32_t released_count, released_cap;

  void *transport;
  void (*complete)(ant_guest_t *g, guest_call_t *call);
  bool (*call_host)(ant_guest_t *g, guest_host_call_t *call);

  ant_value_t *handles;
  uint8_t *handle_live;
  uint32_t handle_count, handle_cap;
  uint32_t *handle_free_ids;
  uint32_t handle_free, handle_free_cap;
  ant_realm_t **contexts;
  uint32_t context_count, context_cap;
  guest_script_t *scripts;
  uint32_t script_count, script_cap;

  ant_value_t *callbacks;
  uint32_t callback_count, callback_cap;
  guest_host_call_t *awaiting;

  _Atomic uint64_t cpu_ns;
  _Atomic uint64_t wall_ns;

  ant_guest_t *next;
  ant_guest_t **pprev;
};

void guest_value_free(guest_value_t *v);
void guest_values_free(guest_value_t *values, uint32_t count);
void guest_call_free(guest_call_t *call);
void guest_host_call_free(guest_host_call_t *call);
ant_guest_t *guest_retain(ant_guest_t *g);
void guest_release(ant_guest_t *g);

bool guest_child_boot(ant_guest_t *g, ant_t *child);
void guest_child_execute(ant_guest_t *g, guest_call_t *call);
void guest_child_teardown(ant_guest_t *g);
void guest_child_mark(ant_t *js, gc_mark_fn mark);
ant_value_t guest_child_host_call(ant_t *js, ant_guest_t *g, uint32_t callback, ant_args_t);

ant_value_t guest_encode(ant_t *js, ant_guest_t *g, ant_value_t value, guest_value_t *out);
ant_value_t guest_decode_host(ant_t *js, ant_guest_t *g, guest_value_t *v);
ant_value_t guest_host_error(ant_t *js, guest_call_t *call);
void guest_host_run_callback(ant_t *js, ant_guest_t *g, guest_host_call_t *call);

ant_value_t guest_own_start(ant_t *js, ant_guest_t *g);
ant_value_t guest_own_submit(ant_t *js, ant_guest_t *g, guest_call_t *call);
ant_value_t guest_own_host_call(ant_t *js, ant_guest_t *g, guest_host_call_t *call);
void guest_own_dispose(ant_guest_t *g);
void guest_own_set_busy(ant_guest_t *g, bool busy);

ant_value_t guest_current_start(ant_t *js, ant_guest_t *g);
ant_value_t guest_current_submit(ant_t *js, ant_guest_t *g, guest_call_t *call);
ant_value_t guest_current_host_call(ant_t *js, ant_guest_t *g, guest_host_call_t *call);
void guest_current_dispose(ant_guest_t *g);

ant_value_t guest_finish_host(ant_t *js, ant_guest_t *g, guest_call_t *call);

#endif
