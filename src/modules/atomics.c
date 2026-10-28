#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#include <uv.h>

#include "ant.h"
#include "cfunc.h"
#include "errors.h"
#include "internal.h"
#include "interrupt.h"

#include "gc/modules.h"
#include "index.h"
#include "modules/bigint.h"
#include "modules/buffer.h"
#include "modules/atomics.h"
#include "modules/timer.h"

typedef enum {
  ASYNC_WAIT_SETTLE_NONE = 0,
  ASYNC_WAIT_SETTLE_OK,
  ASYNC_WAIT_SETTLE_TIMED_OUT,
} async_wait_settle_t;

typedef struct AsyncWaitEntry {
  ant_t *js;
  ant_value_t promise;
  ArrayBufferData *buffer;
  int32_t *address;
  uv_timer_t timer;
  uv_async_t async;
  bool timer_initialized;
  bool async_initialized;
  uint8_t pending_handles;
  _Atomic int settle_state;
  _Atomic bool settle_drain_microtasks;
  struct AsyncWaitEntry *next;
  struct AsyncWaitEntry *prev;
} AsyncWaitEntry;

static WaitQueue global_wait_queue;
static AsyncWaitEntry *async_waiters_head = NULL;

static pthread_once_t wait_queue_init_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t async_waiters_lock = PTHREAD_MUTEX_INITIALIZER;

static inline bool async_waiter_is_linked_locked(AsyncWaitEntry *entry) {
  return entry && (entry == async_waiters_head || entry->next || entry->prev);
}

static void init_wait_queue(void) {
  wait_queue_init(&global_wait_queue);
}

void wait_queue_init(WaitQueue *queue) {
  queue->head = NULL;
  pthread_mutex_init(&queue->lock, NULL);
}

static void async_waiter_add_locked(AsyncWaitEntry *entry) {
  entry->next = async_waiters_head;
  entry->prev = NULL;
  if (async_waiters_head) async_waiters_head->prev = entry;
  async_waiters_head = entry;
}

static void async_waiter_remove_locked(AsyncWaitEntry *entry) {
  if (entry->prev) entry->prev->next = entry->next;
  else async_waiters_head = entry->next;
  if (entry->next) entry->next->prev = entry->prev;
  entry->next = NULL;
  entry->prev = NULL;
}

static void async_waiter_release_buffer(AsyncWaitEntry *entry) {
  if (!entry || !entry->buffer) return;
  free_array_buffer_data(entry->js, entry->buffer);
  entry->buffer = NULL;
}

static void async_waiter_release_handle(AsyncWaitEntry *entry) {
  if (!entry) return;
  if (entry->pending_handles > 0) entry->pending_handles--;
  if (entry->pending_handles == 0) {
    async_waiter_release_buffer(entry);
    free(entry);
  }
}

static void async_waiter_close_cb(uv_handle_t *handle) {
  AsyncWaitEntry *entry = handle ? handle->data : NULL;
  async_waiter_release_handle(entry);
}

static void async_waiter_close_handles(AsyncWaitEntry *entry) {
  bool closed = false;

  if (entry->timer_initialized && !uv_is_closing((uv_handle_t *)&entry->timer)) {
    uv_timer_stop(&entry->timer);
    uv_close((uv_handle_t *)&entry->timer, async_waiter_close_cb);
    entry->timer_initialized = false;
    closed = true;
  }

  if (entry->async_initialized && !uv_is_closing((uv_handle_t *)&entry->async)) {
    uv_close((uv_handle_t *)&entry->async, async_waiter_close_cb);
    entry->async_initialized = false;
    closed = true;
  }

  if (!closed) {
    async_waiter_release_buffer(entry);
    free(entry);
  }
}

static void async_waiter_queue_settle(AsyncWaitEntry *entry, async_wait_settle_t state, bool drain_microtasks) {
  if (!entry) return;
  atomic_store(&entry->settle_drain_microtasks, drain_microtasks);
  atomic_store(&entry->settle_state, state);
  if (entry->async_initialized) uv_async_send(&entry->async);
}

static void async_waiter_async_cb(uv_async_t *handle) {
  AsyncWaitEntry *entry = handle ? handle->data : NULL;
  if (!entry || !entry->js) return;

  async_wait_settle_t state = (async_wait_settle_t)atomic_exchange(&entry->settle_state, ASYNC_WAIT_SETTLE_NONE);
  if (state == ASYNC_WAIT_SETTLE_NONE) return;

  const char *result = state == ASYNC_WAIT_SETTLE_OK ? "ok" : "timed-out";
  js_resolve_promise(entry->js, entry->promise, js_mkstr(entry->js, result, strlen(result)));
  if (atomic_load(&entry->settle_drain_microtasks))
    js_maybe_drain_microtasks_after_async_settle(entry->js);

  async_waiter_close_handles(entry);
}

static void async_waiter_timeout_cb(uv_timer_t *timer) {
  AsyncWaitEntry *entry = timer ? timer->data : NULL;
  if (!entry) return;

  pthread_mutex_lock(&async_waiters_lock);
  bool linked = async_waiter_is_linked_locked(entry);
  if (linked) async_waiter_remove_locked(entry);
  pthread_mutex_unlock(&async_waiters_lock);

  if (linked) async_waiter_queue_settle(entry, ASYNC_WAIT_SETTLE_TIMED_OUT, true);
}

static int async_waiter_notify(int32_t *address, int count) {
  int notified = 0;
  AsyncWaitEntry *ready = NULL;

  pthread_mutex_lock(&async_waiters_lock);
  AsyncWaitEntry *current = async_waiters_head;
  while (current && (count == -1 || notified < count)) {
    AsyncWaitEntry *next = current->next;
    if (current->address == address) {
      async_waiter_remove_locked(current);
      current->next = ready;
      current->prev = NULL;
      ready = current;
      notified++;
    } current = next;
  }
  
  pthread_mutex_unlock(&async_waiters_lock);
  while (ready) {
    AsyncWaitEntry *next = ready->next;
    ready->next = NULL;
    async_waiter_queue_settle(ready, ASYNC_WAIT_SETTLE_OK, false);
    ready = next;
  }

  return notified;
}

void wait_queue_add(WaitQueue *queue, WaitQueueEntry *entry) {
  pthread_mutex_lock(&queue->lock);
  entry->next = queue->head;
  queue->head = entry;
  pthread_mutex_unlock(&queue->lock);
}

void wait_queue_remove(WaitQueue *queue, WaitQueueEntry *entry) {
  pthread_mutex_lock(&queue->lock);
  WaitQueueEntry **current = &queue->head;
  while (*current) {
    if (*current == entry) {
      *current = entry->next;
      break;
    }
    current = &(*current)->next;
  }
  pthread_mutex_unlock(&queue->lock);
}

int wait_queue_notify(WaitQueue *queue, int32_t *address, int count) {
  pthread_mutex_lock(&queue->lock);
  int notified = 0;
  WaitQueueEntry *current = queue->head;
  
  while (current && (count == -1 || notified < count)) {
    if (current->address == address) {
      pthread_mutex_lock(&current->mutex);
      current->notified = 1;
      pthread_cond_signal(&current->cond);
      pthread_mutex_unlock(&current->mutex);
      notified++;
    } current = current->next;
  }
  
  pthread_mutex_unlock(&queue->lock);
  if (count == -1 || notified < count)
    notified += async_waiter_notify(address, count == -1 ? -1 : count - notified);
    
  return notified;
}

void atomics_wake_isolate(ant_t *js) {
  pthread_once(&wait_queue_init_once, init_wait_queue);
  pthread_mutex_lock(&global_wait_queue.lock);
  
  for (WaitQueueEntry *entry = global_wait_queue.head; entry; entry = entry->next) {
    if (entry->js != js) continue;
    pthread_mutex_lock(&entry->mutex);
    pthread_cond_signal(&entry->cond);
    pthread_mutex_unlock(&entry->mutex);
  }
  
  pthread_mutex_unlock(&global_wait_queue.lock);
}

void cleanup_atomics_module(ant_t *js) {
  if (!js) return;
  AsyncWaitEntry *removed = NULL;

  pthread_mutex_lock(&async_waiters_lock);
  AsyncWaitEntry *current = async_waiters_head;
  
  while (current) {
    AsyncWaitEntry *next = current->next;
    if (current->js == js) {
      async_waiter_remove_locked(current);
      current->next = removed;
      current->prev = NULL;
      removed = current;
    } current = next;
  }
  
  pthread_mutex_unlock(&async_waiters_lock);
  while (removed) {
    AsyncWaitEntry *next = removed->next;
    removed->next = NULL;
    removed->prev = NULL;
    removed->js = NULL;
    removed->promise = js_mkundef();
    async_waiter_release_buffer(removed);
    async_waiter_close_handles(removed);
    removed = next;
  }
}

static bool get_atomic_array_data(ant_t *js, ant_value_t this_val, TypedArrayData **out_data, uint8_t **out_ptr) {
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  size_t len;
  
  *out_data = ta_data;
  return Ant_Buffer_GetViewBytes(ta_data, out_ptr, &len);
}

typedef enum {
#define ATOMICS_OP(name, op, arity) ATOMICS_##op,
#include "atomics/ops.h"
} atomics_op_t;

typedef enum {
  ATOMICS_KIND_NONE = 0,
  ATOMICS_KIND_INT,
  ATOMICS_KIND_BIGINT,
} atomics_kind_t;

#define ATOMICS_APPLY(name, ctype, result_t)                                                \
  static inline result_t atomics_apply_##name(                                              \
    uint8_t *ptr, size_t index, atomics_op_t op, uint64_t value, uint64_t replacement       \
  ) {                                                                                       \
    _Atomic ctype *slot = (_Atomic ctype *)ptr + index;                                     \
    ctype expected = (ctype)value;                                                          \
    switch (op) {                                                                           \
      case ATOMICS_ADD:      return (result_t)atomic_fetch_add(slot, (ctype)value);         \
      case ATOMICS_AND:      return (result_t)atomic_fetch_and(slot, (ctype)value);         \
      case ATOMICS_EXCHANGE: return (result_t)atomic_exchange(slot, (ctype)value);          \
      case ATOMICS_LOAD:     return (result_t)atomic_load(slot);                            \
      case ATOMICS_OR:       return (result_t)atomic_fetch_or(slot, (ctype)value);          \
      case ATOMICS_STORE:    atomic_store(slot, (ctype)value); return 0;                    \
      case ATOMICS_SUB:      return (result_t)atomic_fetch_sub(slot, (ctype)value);         \
      case ATOMICS_XOR:      return (result_t)atomic_fetch_xor(slot, (ctype)value);         \
      case ATOMICS_COMPARE_EXCHANGE:                                                        \
        atomic_compare_exchange_strong(slot, &expected, (ctype)replacement);                \
        return (result_t)expected;                                                          \
    }                                                                                       \
    return 0;                                                                               \
  }

#define ATOMICS_APPLY_INT(name, ctype)    ATOMICS_APPLY(name, ctype, double)
#define ATOMICS_APPLY_FLOAT(name, ctype)
#define ATOMICS_APPLY_BIGINT(name, ctype) ATOMICS_APPLY(name, ctype, ctype)
#define BUFFER_TYPED_ARRAY(name, ctor, ctype, kind, load, store) ATOMICS_APPLY_##kind(name, ctype)

#include "buffer/typed_arrays.h"

#undef ATOMICS_APPLY
#undef ATOMICS_APPLY_INT
#undef ATOMICS_APPLY_FLOAT
#undef ATOMICS_APPLY_BIGINT

static inline int32_t atomics_int32(double value) {
#ifdef __ARM_FEATURE_JCVT
  return __builtin_arm_jcvt(value);
#else
  return value >= INT32_MIN && value <= INT32_MAX ? (int32_t)value : js_to_int32(value);
#endif
}

static inline double atomics_integer(double value) {
  return isnan(value) ? 0 : trunc(value) + 0.0;
}

static inline atomics_kind_t atomics_kind(TypedArrayType type) {
  static const uint8_t kinds[] = {
    #define ATOMICS_KIND_OF_INT    ATOMICS_KIND_INT
    #define ATOMICS_KIND_OF_FLOAT  ATOMICS_KIND_NONE
    #define ATOMICS_KIND_OF_BIGINT ATOMICS_KIND_BIGINT
    #define BUFFER_TYPED_ARRAY(name, ctor, ctype, kind, ...) ATOMICS_KIND_OF_##kind,
    #include "buffer/typed_arrays.h"
    #undef ATOMICS_KIND_OF_INT
    #undef ATOMICS_KIND_OF_FLOAT
    #undef ATOMICS_KIND_OF_BIGINT
  };

  if (type >= TYPED_ARRAY_COUNT || type == TYPED_ARRAY_UINT8_CLAMPED) return ATOMICS_KIND_NONE;
  return (atomics_kind_t)kinds[type];
}

static inline __attribute__((always_inline)) ant_value_t atomics_dispatch(
  ant_t *js, TypedArrayType type, uint8_t *ptr, size_t index, atomics_op_t op, uint64_t operand, uint64_t swap
) {
  switch (type) {
    #define ATOMICS_CASE_INT(name, ctype, load) case TYPED_ARRAY_##name: \
      return js_mknum(atomics_apply_##name(ptr, index, op, operand, swap));
    #define ATOMICS_CASE_FLOAT(name, ctype, load)
    #define ATOMICS_CASE_BIGINT(name, ctype, load) case TYPED_ARRAY_##name: { \
      ctype result = atomics_apply_##name(ptr, index, op, operand, swap);    \
      return op == ATOMICS_STORE ? js_mkundef() : load(js, result);          \
    }
    #define BUFFER_TYPED_ARRAY(name, ctor, ctype, kind, load, store) ATOMICS_CASE_##kind(name, ctype, load)
    #include "buffer/typed_arrays.h"
    #undef ATOMICS_CASE_INT
    #undef ATOMICS_CASE_FLOAT
    #undef ATOMICS_CASE_BIGINT
    default: return js_mkundef();
  }
}

static ant_value_t atomics_validate(ant_native_params_t, TypedArrayData **out, size_t *index) {
  TypedArrayData *ta = buffer_get_typedarray_data(nargs > 0 ? args[0] : js_mkundef());
  
  if (!ta) return js_mkerr_typed(js, JS_ERR_TYPE, "Argument is not a typed array");
  if (!atomics_kind(ta->type)) return js_mkerr_typed(js, JS_ERR_TYPE, "Atomics operations require an integer typed array");
  if (!Ant_Buffer_ViewIsLive(ta)) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot perform Atomics operation on a detached ArrayBuffer");

  ant_value_t result = js_to_index_fast(js, nargs > 1 ? args[1] : js_mkundef(), index);
  if (is_err(result)) return result;
  if (*index >= ta->length) return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid atomic access index");

  *out = ta;
  return js_mkundef();
}

static ant_value_t atomics_coerce(ant_t *js, const TypedArrayData *ta, ant_value_t value, uint64_t *bits) {
  if (atomics_kind(ta->type) == ATOMICS_KIND_BIGINT) {
    value = bigint_to_bigint(js, value);
    if (is_err(value)) return value;
    if (!bigint_to_uint64_wrapping(js, value, bits)) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert to BigInt");
    return value;
  }

  if (is_object_type(value) || vtype(value) == kTypeBuiltin) {
    value = js_to_primitive(js, value, 2);
    if (is_err(value)) return value;
  }
  
  if (vtype(value) == kTypeBigInt) 
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert a BigInt value to a number");

  double number = vtype(value) == kTypeNumber ? tod(value) : js_to_number(js, value);
  if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
  
  number = atomics_integer(number);
  *bits = (uint32_t)atomics_int32(number);
  
  return tov(number);
}

static ant_value_t atomics_bytes(ant_t *js, const TypedArrayData *ta, size_t index, uint8_t **out) {
  size_t byte_length;
  if (!Ant_Buffer_GetViewBytes(ta, out, &byte_length))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot perform Atomics operation on a detached ArrayBuffer");
  if (index >= ta->length) return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid atomic access index");
  return js_mkundef();
}

static __attribute__((noinline, cold)) 
ant_value_t atomics_run_slow(ant_native_params_t, atomics_op_t op, int arity) {
  TypedArrayData *ta;
  size_t index;
  
  uint64_t operand = 0, swap = 0;
  uint8_t *ptr;

  ant_value_t value = atomics_validate(js, args, nargs, &ta, &index);
  if (is_err(value)) return value;

  if (arity > 2) {
    value = atomics_coerce(js, ta, nargs > 2 ? args[2] : js_mkundef(), &operand);
    if (is_err(value)) return value;
  }

  if (arity > 3) {
    ant_value_t replacement = atomics_coerce(js, ta, nargs > 3 ? args[3] : js_mkundef(), &swap);
    if (is_err(replacement)) return replacement;
  }

  ant_value_t result = atomics_bytes(js, ta, index, &ptr);
  if (is_err(result)) return result;

  result = atomics_dispatch(js, ta->type, ptr, index, op, operand, swap);
  return op == ATOMICS_STORE ? value : result;
}

static inline __attribute__((always_inline)) 
ant_value_t atomics_run(ant_native_params_t, atomics_op_t op, int arity) {
  if (__builtin_expect(nargs < arity, 0)) goto slow;
  for (int i = 1; i < arity; i++) if (__builtin_expect(is_tagged(args[i]), 0)) goto slow;

  TypedArrayData *ta_data;
  uint8_t *ptr;
  if (__builtin_expect(!get_atomic_array_data(js, args[0], &ta_data, &ptr), 0)) goto slow;

  double index = tod(args[1]);
  if (__builtin_expect(!(index > -1 && index < (double)ta_data->length), 0)) goto slow;
  if (__builtin_expect(atomics_kind(ta_data->type) != ATOMICS_KIND_INT, 0)) goto slow;

  double value = arity > 2 ? tod(args[2]) : 0;
  uint32_t operand = arity > 2 ? (uint32_t)atomics_int32(value) : 0;
  uint32_t swap = arity > 3 ? (uint32_t)atomics_int32(tod(args[3])) : 0;

  ant_value_t result = atomics_dispatch(js, ta_data->type, ptr, (size_t)index, op, operand, swap);
  return op == ATOMICS_STORE ? js_mknum(atomics_integer(value)) : result;

slow:
  return atomics_run_slow(js, args, nargs, op, arity);
}

#define ATOMICS_OP(name, op, arity)                           \
  static ant_value_t js_atomics_##name(ant_params_t) {        \
    return atomics_run(js, args, nargs, ATOMICS_##op, arity); \
  }
#include "atomics/ops.h"

// Atomics.isLockFree(size)
static ant_value_t js_atomics_isLockFree(ant_params_t) {
  if (nargs < 1) {
    return js_mkerr(js, "Atomics.isLockFree requires 1 argument");
  }
  
  int size = (int)js_getnum(args[0]);
  bool is_lock_free = false;
  
  switch (size) {
    case 1:
      is_lock_free = ATOMIC_CHAR_LOCK_FREE == 2;
      break;
    case 2:
      is_lock_free = ATOMIC_SHORT_LOCK_FREE == 2;
      break;
    case 4:
      is_lock_free = ATOMIC_INT_LOCK_FREE == 2;
      break;
    case 8:
      is_lock_free = ATOMIC_LLONG_LOCK_FREE == 2;
      break;
    default:
      is_lock_free = false;
  }
  
  return js_bool(is_lock_free);
}

// Atomics.wait(typedArray, index, value, timeout)
static ant_value_t js_atomics_wait(ant_params_t) {
  if (nargs < 3) {
    return js_mkerr(js, "Atomics.wait requires at least 3 arguments");
  }
  
  pthread_once(&wait_queue_init_once, init_wait_queue);
  
  TypedArrayData *ta_data;
  uint8_t *ptr;
  if (!get_atomic_array_data(js, args[0], &ta_data, &ptr)) {
    return js_mkerr(js, "First argument must be a TypedArray");
  }
  
  if (ta_data->type != TYPED_ARRAY_INT32) {
    return js_mkerr(js, "Atomics.wait only works with Int32Array");
  }
  
  size_t index = (size_t)js_getnum(args[1]);
  if (index >= ta_data->length) {
    return js_mkerr(js, "Index out of bounds");
  }
  
  int32_t expected_value = (int32_t)js_getnum(args[2]);
  int64_t timeout_ms = -1;
  
  if (nargs > 3 && vtype(args[3]) == kTypeNumber) {
    timeout_ms = (int64_t)js_getnum(args[3]);
  }
  
  _Atomic int32_t *atomic_ptr = (_Atomic int32_t *)(ptr + index * 4);
  
  WaitQueueEntry entry;
  pthread_cond_init(&entry.cond, NULL);
  pthread_mutex_init(&entry.mutex, NULL);
  
  entry.js = js;
  entry.address = (int32_t *)atomic_ptr;
  entry.notified = 0;
  
  pthread_mutex_lock(&global_wait_queue.lock);
  if (atomic_load(atomic_ptr) != expected_value) {
    pthread_mutex_unlock(&global_wait_queue.lock);
    pthread_cond_destroy(&entry.cond);
    pthread_mutex_destroy(&entry.mutex);
    return js_mkstr(js, "not-equal", 9);
  }
  
  entry.next = global_wait_queue.head;
  global_wait_queue.head = &entry;
  
  pthread_mutex_lock(&entry.mutex);
  pthread_mutex_unlock(&global_wait_queue.lock);
  
  const char *result = "ok";
  if (timeout_ms < 0) {
    while (!entry.notified && !js_interrupt_pending(js)) pthread_cond_wait(&entry.cond, &entry.mutex);
  } else {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
      ts.tv_sec++;
      ts.tv_nsec -= 1000000000;
    }
    
    while (!entry.notified && !js_interrupt_pending(js))
      if (pthread_cond_timedwait(&entry.cond, &entry.mutex, &ts) == ETIMEDOUT) break;
    if (!entry.notified) result = "timed-out";
  }
  
  bool interrupted = !entry.notified && js_interrupt_pending(js);
  pthread_mutex_unlock(&entry.mutex);
  wait_queue_remove(&global_wait_queue, &entry);
  
  pthread_cond_destroy(&entry.cond);
  pthread_mutex_destroy(&entry.mutex);
  
  if (interrupted) return js_interrupt_raise(js);
  return js_mkstr(js, result, strlen(result));
}

// Atomics.notify(typedArray, index, count)
static ant_value_t js_atomics_notify(ant_params_t) {
  if (nargs < 2) {
    return js_mkerr(js, "Atomics.notify requires at least 2 arguments");
  }
  
  pthread_once(&wait_queue_init_once, init_wait_queue);
  
  TypedArrayData *ta_data;
  uint8_t *ptr;
  if (!get_atomic_array_data(js, args[0], &ta_data, &ptr)) {
    return js_mkerr(js, "First argument must be a TypedArray");
  }
  
  if (ta_data->type != TYPED_ARRAY_INT32) {
    return js_mkerr(js, "Atomics.notify only works with Int32Array");
  }
  
  size_t index = (size_t)js_getnum(args[1]);
  if (index >= ta_data->length) {
    return js_mkerr(js, "Index out of bounds");
  }
  
  int count = -1;
  if (nargs > 2 && vtype(args[2]) == kTypeNumber) {
    count = (int)js_getnum(args[2]);
  }
  
  int32_t *address = (int32_t *)(ptr + index * 4);
  int notified = wait_queue_notify(&global_wait_queue, address, count);
  
  return js_mknum((double)notified);
}

// Atomics.waitAsync(typedArray, index, value, timeout)
static ant_value_t js_atomics_waitAsync(ant_params_t) {
  if (nargs < 3) {
    return js_mkerr(js, "Atomics.waitAsync requires at least 3 arguments");
  }
  
  TypedArrayData *ta_data;
  uint8_t *ptr;
  if (!get_atomic_array_data(js, args[0], &ta_data, &ptr)) {
    return js_mkerr(js, "First argument must be a TypedArray");
  }
  
  if (ta_data->type != TYPED_ARRAY_INT32) {
    return js_mkerr(js, "Atomics.waitAsync only works with Int32Array");
  }
  
  size_t index = (size_t)js_getnum(args[1]);
  if (index >= ta_data->length) {
    return js_mkerr(js, "Index out of bounds");
  }
  
  int32_t expected_value = (int32_t)js_getnum(args[2]);
  _Atomic int32_t *atomic_ptr = (_Atomic int32_t *)(ptr + index * 4);
  int32_t current_value = atomic_load(atomic_ptr);
  double timeout_ms = HUGE_VAL;

  if (nargs > 3 && vtype(args[3]) == kTypeNumber) {
    timeout_ms = js_getnum(args[3]);
    if (isnan(timeout_ms)) timeout_ms = HUGE_VAL;
    else if (timeout_ms < 0) timeout_ms = 0;
  }
  
  ant_value_t result_obj = Ant_Object_CreateBare(js);
  if (current_value != expected_value) {
    js_set(js, result_obj, "async", js_false);
    js_set(js, result_obj, "value", js_mkstr(js, "not-equal", 9));
    return result_obj;
  }
  
  if (timeout_ms == 0) {
    js_set(js, result_obj, "async", js_false);
    js_set(js, result_obj, "value", js_mkstr(js, "timed-out", 9));
    return result_obj;
  }

  ant_value_t promise = js_mkpromise(js);
  AsyncWaitEntry *entry = calloc(1, sizeof(*entry));
  if (!entry) return Ant_Error_CreateOutOfMemory(js);

  entry->js = js;
  entry->promise = promise;
  entry->buffer = ta_data->buffer;
  entry->buffer->ref_count++;
  entry->address = (int32_t *)atomic_ptr;
  atomic_store(&entry->settle_state, ASYNC_WAIT_SETTLE_NONE);
  atomic_store(&entry->settle_drain_microtasks, false);

  if (uv_async_init(js->loop, &entry->async, async_waiter_async_cb) != 0) {
    async_waiter_close_handles(entry);
    return js_mkerr(js, "Failed to initialize Atomics.waitAsync notifier");
  }
  
  entry->async_initialized = true;
  entry->async.data = entry;
  entry->pending_handles++;

  if (isfinite(timeout_ms)) {
    uint64_t delay = timeout_ms > (double)UINT64_MAX ? UINT64_MAX : (uint64_t)timeout_ms;
    if (uv_timer_init(js->loop, &entry->timer) != 0) {
      async_waiter_close_handles(entry);
      return js_mkerr(js, "Failed to initialize Atomics.waitAsync timer");
    }
    entry->timer_initialized = true;
    entry->timer.data = entry;
    entry->pending_handles++;
    if (uv_timer_start(&entry->timer, async_waiter_timeout_cb, delay, 0) != 0) {
      async_waiter_close_handles(entry);
      return js_mkerr(js, "Failed to start Atomics.waitAsync timer");
    }
  }

  pthread_mutex_lock(&async_waiters_lock);
  bool changed = atomic_load(atomic_ptr) != expected_value;
  if (!changed) async_waiter_add_locked(entry);
  
  pthread_mutex_unlock(&async_waiters_lock);
  if (changed) async_waiter_queue_settle(entry, ASYNC_WAIT_SETTLE_OK, false);

  js_set(js, result_obj, "async", js_true);
  js_set(js, result_obj, "value", promise);

  return result_obj;
}

// Atomics.pause()
static ant_value_t js_atomics_pause(ant_params_t) {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield");
#endif
  
  return js_mkundef();
}

void init_atomics_module(ant_t *js) {
  ant_value_t atomics = Ant_Object_Create(js);
  
  #define ATOMICS_OP(name, op, arity) defmethod(js, atomics, #name, sizeof(#name) - 1, Ant_Function_CreateWithArity(js_atomics_##name, arity));
  #include "atomics/ops.h"
  
  defmethod(js, atomics, "isLockFree", 10, Ant_Function_CreateWithArity(js_atomics_isLockFree, 1));
  defmethod(js, atomics, "notify", 6, Ant_Function_CreateWithArity(js_atomics_notify, 3));
  defmethod(js, atomics, "pause", 5, Ant_Function_Create(js_atomics_pause));
  defmethod(js, atomics, "wait", 4, Ant_Function_CreateWithArity(js_atomics_wait, 4));
  defmethod(js, atomics, "waitAsync", 9, Ant_Function_CreateWithArity(js_atomics_waitAsync, 4));
  
  js_set_tostringtag(js, atomics, js_mkstr(js, "Atomics", 7));
  js_set_global_builtin(js, "Atomics", atomics);
}

void gc_mark_atomics(ant_t *js, gc_mark_fn mark) {
  pthread_mutex_lock(&async_waiters_lock);
  for (AsyncWaitEntry *entry = async_waiters_head; entry; entry = entry->next) {
    if (entry->js == js) mark(js, entry->promise);
  }
  pthread_mutex_unlock(&async_waiters_lock);
}
