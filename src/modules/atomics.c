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
#include "errors.h"
#include "internal.h"
#include "runtime.h"

#include "gc/modules.h"
#include "modules/buffer.h"
#include "modules/atomics.h"
#include "modules/symbol.h"
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
  free_array_buffer_data(entry->buffer);
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

void wait_queue_cleanup(WaitQueue *queue) {
  pthread_mutex_lock(&queue->lock);
  WaitQueueEntry *current = queue->head;
  while (current) {
    WaitQueueEntry *next = current->next;
    pthread_cond_destroy(&current->cond);
    pthread_mutex_destroy(&current->mutex);
    free(current);
    current = next;
  }
  queue->head = NULL;
  pthread_mutex_unlock(&queue->lock);
  pthread_mutex_destroy(&queue->lock);
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
  if (!ta_data || !ta_data->buffer) return false;
  
  *out_data = ta_data;
  *out_ptr = ta_data->buffer->data + ta_data->byte_offset;
  
  return true;
}

// Atomics.add(typedArray, index, value)
static ant_value_t js_atomics_add(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 3) {
    return js_mkerr(js, "Atomics.add requires 3 arguments");
  }
  
  TypedArrayData *ta_data;
  uint8_t *ptr;
  if (!get_atomic_array_data(js, args[0], &ta_data, &ptr)) {
    return js_mkerr(js, "First argument must be a TypedArray");
  }
  
  size_t index = (size_t)js_getnum(args[1]);
  if (index >= ta_data->length) {
    return js_mkerr(js, "Index out of bounds");
  }
  
  int32_t value = (int32_t)js_getnum(args[2]);
  int32_t old_value;
  
  switch (ta_data->type) {
    case TYPED_ARRAY_INT8: {
      _Atomic int8_t *atomic_ptr = (_Atomic int8_t *)(ptr + index);
      old_value = atomic_fetch_add(atomic_ptr, (int8_t)value);
      break;
    }
    case TYPED_ARRAY_UINT8: {
      _Atomic uint8_t *atomic_ptr = (_Atomic uint8_t *)(ptr + index);
      old_value = atomic_fetch_add(atomic_ptr, (uint8_t)value);
      break;
    }
    case TYPED_ARRAY_INT16: {
      _Atomic int16_t *atomic_ptr = (_Atomic int16_t *)(ptr + index * 2);
      old_value = atomic_fetch_add(atomic_ptr, (int16_t)value);
      break;
    }
    case TYPED_ARRAY_UINT16: {
      _Atomic uint16_t *atomic_ptr = (_Atomic uint16_t *)(ptr + index * 2);
      old_value = atomic_fetch_add(atomic_ptr, (uint16_t)value);
      break;
    }
    case TYPED_ARRAY_INT32: {
      _Atomic int32_t *atomic_ptr = (_Atomic int32_t *)(ptr + index * 4);
      old_value = atomic_fetch_add(atomic_ptr, value);
      break;
    }
    case TYPED_ARRAY_UINT32: {
      _Atomic uint32_t *atomic_ptr = (_Atomic uint32_t *)(ptr + index * 4);
      old_value = atomic_fetch_add(atomic_ptr, (uint32_t)value);
      break;
    }
    default:
      return js_mkerr(js, "TypedArray type not supported for atomic operations");
  }
  
  return js_mknum((double)old_value);
}

// Atomics.and(typedArray, index, value)
static ant_value_t js_atomics_and(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 3) {
    return js_mkerr(js, "Atomics.and requires 3 arguments");
  }
  
  TypedArrayData *ta_data;
  uint8_t *ptr;
  if (!get_atomic_array_data(js, args[0], &ta_data, &ptr)) {
    return js_mkerr(js, "First argument must be a TypedArray");
  }
  
  size_t index = (size_t)js_getnum(args[1]);
  if (index >= ta_data->length) {
    return js_mkerr(js, "Index out of bounds");
  }
  
  int32_t value = (int32_t)js_getnum(args[2]);
  int32_t old_value;
  
  switch (ta_data->type) {
    case TYPED_ARRAY_INT8: {
      _Atomic int8_t *atomic_ptr = (_Atomic int8_t *)(ptr + index);
      old_value = atomic_fetch_and(atomic_ptr, (int8_t)value);
      break;
    }
    case TYPED_ARRAY_UINT8: {
      _Atomic uint8_t *atomic_ptr = (_Atomic uint8_t *)(ptr + index);
      old_value = atomic_fetch_and(atomic_ptr, (uint8_t)value);
      break;
    }
    case TYPED_ARRAY_INT16: {
      _Atomic int16_t *atomic_ptr = (_Atomic int16_t *)(ptr + index * 2);
      old_value = atomic_fetch_and(atomic_ptr, (int16_t)value);
      break;
    }
    case TYPED_ARRAY_UINT16: {
      _Atomic uint16_t *atomic_ptr = (_Atomic uint16_t *)(ptr + index * 2);
      old_value = atomic_fetch_and(atomic_ptr, (uint16_t)value);
      break;
    }
    case TYPED_ARRAY_INT32: {
      _Atomic int32_t *atomic_ptr = (_Atomic int32_t *)(ptr + index * 4);
      old_value = atomic_fetch_and(atomic_ptr, value);
      break;
    }
    case TYPED_ARRAY_UINT32: {
      _Atomic uint32_t *atomic_ptr = (_Atomic uint32_t *)(ptr + index * 4);
      old_value = atomic_fetch_and(atomic_ptr, (uint32_t)value);
      break;
    }
    default:
      return js_mkerr(js, "TypedArray type not supported for atomic bitwise operations");
  }
  
  return js_mknum((double)old_value);
}

// Atomics.compareExchange(typedArray, index, expectedValue, replacementValue)
static ant_value_t js_atomics_compareExchange(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 4) {
    return js_mkerr(js, "Atomics.compareExchange requires 4 arguments");
  }
  
  TypedArrayData *ta_data;
  uint8_t *ptr;
  if (!get_atomic_array_data(js, args[0], &ta_data, &ptr)) {
    return js_mkerr(js, "First argument must be a TypedArray");
  }
  
  size_t index = (size_t)js_getnum(args[1]);
  if (index >= ta_data->length) {
    return js_mkerr(js, "Index out of bounds");
  }
  
  int32_t expected = (int32_t)js_getnum(args[2]);
  int32_t replacement = (int32_t)js_getnum(args[3]);
  
  switch (ta_data->type) {
    case TYPED_ARRAY_INT8: {
      int8_t exp_i8 = (int8_t)expected;
      _Atomic int8_t *atomic_ptr = (_Atomic int8_t *)(ptr + index);
      atomic_compare_exchange_strong(atomic_ptr, &exp_i8, (int8_t)replacement);
      expected = (int32_t)exp_i8;
      break;
    }
    case TYPED_ARRAY_UINT8: {
      uint8_t exp_u8 = (uint8_t)expected;
      _Atomic uint8_t *atomic_ptr = (_Atomic uint8_t *)(ptr + index);
      atomic_compare_exchange_strong(atomic_ptr, &exp_u8, (uint8_t)replacement);
      expected = (int32_t)exp_u8;
      break;
    }
    case TYPED_ARRAY_INT16: {
      int16_t exp_i16 = (int16_t)expected;
      _Atomic int16_t *atomic_ptr = (_Atomic int16_t *)(ptr + index * 2);
      atomic_compare_exchange_strong(atomic_ptr, &exp_i16, (int16_t)replacement);
      expected = (int32_t)exp_i16;
      break;
    }
    case TYPED_ARRAY_UINT16: {
      uint16_t exp_u16 = (uint16_t)expected;
      _Atomic uint16_t *atomic_ptr = (_Atomic uint16_t *)(ptr + index * 2);
      atomic_compare_exchange_strong(atomic_ptr, &exp_u16, (uint16_t)replacement);
      expected = (int32_t)exp_u16;
      break;
    }
    case TYPED_ARRAY_INT32: {
      _Atomic int32_t *atomic_ptr = (_Atomic int32_t *)(ptr + index * 4);
      atomic_compare_exchange_strong(atomic_ptr, &expected, replacement);
      break;
    }
    case TYPED_ARRAY_UINT32: {
      uint32_t exp_u32 = (uint32_t)expected;
      _Atomic uint32_t *atomic_ptr = (_Atomic uint32_t *)(ptr + index * 4);
      atomic_compare_exchange_strong(atomic_ptr, &exp_u32, (uint32_t)replacement);
      expected = (int32_t)exp_u32;
      break;
    }
    default:
      return js_mkerr(js, "TypedArray type not supported for atomic operations");
  }
  
  return js_mknum((double)expected);
}

// Atomics.exchange(typedArray, index, value)
static ant_value_t js_atomics_exchange(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 3) {
    return js_mkerr(js, "Atomics.exchange requires 3 arguments");
  }
  
  TypedArrayData *ta_data;
  uint8_t *ptr;
  if (!get_atomic_array_data(js, args[0], &ta_data, &ptr)) {
    return js_mkerr(js, "First argument must be a TypedArray");
  }
  
  size_t index = (size_t)js_getnum(args[1]);
  if (index >= ta_data->length) {
    return js_mkerr(js, "Index out of bounds");
  }
  
  int32_t value = (int32_t)js_getnum(args[2]);
  int32_t old_value;
  
  switch (ta_data->type) {
    case TYPED_ARRAY_INT8: {
      _Atomic int8_t *atomic_ptr = (_Atomic int8_t *)(ptr + index);
      old_value = atomic_exchange(atomic_ptr, (int8_t)value);
      break;
    }
    case TYPED_ARRAY_UINT8: {
      _Atomic uint8_t *atomic_ptr = (_Atomic uint8_t *)(ptr + index);
      old_value = atomic_exchange(atomic_ptr, (uint8_t)value);
      break;
    }
    case TYPED_ARRAY_INT16: {
      _Atomic int16_t *atomic_ptr = (_Atomic int16_t *)(ptr + index * 2);
      old_value = atomic_exchange(atomic_ptr, (int16_t)value);
      break;
    }
    case TYPED_ARRAY_UINT16: {
      _Atomic uint16_t *atomic_ptr = (_Atomic uint16_t *)(ptr + index * 2);
      old_value = atomic_exchange(atomic_ptr, (uint16_t)value);
      break;
    }
    case TYPED_ARRAY_INT32: {
      _Atomic int32_t *atomic_ptr = (_Atomic int32_t *)(ptr + index * 4);
      old_value = atomic_exchange(atomic_ptr, value);
      break;
    }
    case TYPED_ARRAY_UINT32: {
      _Atomic uint32_t *atomic_ptr = (_Atomic uint32_t *)(ptr + index * 4);
      old_value = atomic_exchange(atomic_ptr, (uint32_t)value);
      break;
    }
    default:
      return js_mkerr(js, "TypedArray type not supported for atomic operations");
  }
  
  return js_mknum((double)old_value);
}

// Atomics.isLockFree(size)
static ant_value_t js_atomics_isLockFree(ant_t *js, ant_value_t *args, int nargs) {
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

// Atomics.load(typedArray, index)
static ant_value_t js_atomics_load(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) {
    return js_mkerr(js, "Atomics.load requires 2 arguments");
  }
  
  TypedArrayData *ta_data;
  uint8_t *ptr;
  if (!get_atomic_array_data(js, args[0], &ta_data, &ptr)) {
    return js_mkerr(js, "First argument must be a TypedArray");
  }
  
  size_t index = (size_t)js_getnum(args[1]);
  if (index >= ta_data->length) {
    return js_mkerr(js, "Index out of bounds");
  }
  
  int32_t value;
  
  switch (ta_data->type) {
    case TYPED_ARRAY_INT8: {
      _Atomic int8_t *atomic_ptr = (_Atomic int8_t *)(ptr + index);
      value = atomic_load(atomic_ptr);
      break;
    }
    case TYPED_ARRAY_UINT8: {
      _Atomic uint8_t *atomic_ptr = (_Atomic uint8_t *)(ptr + index);
      value = atomic_load(atomic_ptr);
      break;
    }
    case TYPED_ARRAY_INT16: {
      _Atomic int16_t *atomic_ptr = (_Atomic int16_t *)(ptr + index * 2);
      value = atomic_load(atomic_ptr);
      break;
    }
    case TYPED_ARRAY_UINT16: {
      _Atomic uint16_t *atomic_ptr = (_Atomic uint16_t *)(ptr + index * 2);
      value = atomic_load(atomic_ptr);
      break;
    }
    case TYPED_ARRAY_INT32: {
      _Atomic int32_t *atomic_ptr = (_Atomic int32_t *)(ptr + index * 4);
      value = atomic_load(atomic_ptr);
      break;
    }
    case TYPED_ARRAY_UINT32: {
      _Atomic uint32_t *atomic_ptr = (_Atomic uint32_t *)(ptr + index * 4);
      value = atomic_load(atomic_ptr);
      break;
    }
    default:
      return js_mkerr(js, "TypedArray type not supported for atomic operations");
  }
  
  return js_mknum((double)value);
}

// Atomics.or(typedArray, index, value)
static ant_value_t js_atomics_or(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 3) {
    return js_mkerr(js, "Atomics.or requires 3 arguments");
  }
  
  TypedArrayData *ta_data;
  uint8_t *ptr;
  if (!get_atomic_array_data(js, args[0], &ta_data, &ptr)) {
    return js_mkerr(js, "First argument must be a TypedArray");
  }
  
  size_t index = (size_t)js_getnum(args[1]);
  if (index >= ta_data->length) {
    return js_mkerr(js, "Index out of bounds");
  }
  
  int32_t value = (int32_t)js_getnum(args[2]);
  int32_t old_value;
  
  switch (ta_data->type) {
    case TYPED_ARRAY_INT8: {
      _Atomic int8_t *atomic_ptr = (_Atomic int8_t *)(ptr + index);
      old_value = atomic_fetch_or(atomic_ptr, (int8_t)value);
      break;
    }
    case TYPED_ARRAY_UINT8: {
      _Atomic uint8_t *atomic_ptr = (_Atomic uint8_t *)(ptr + index);
      old_value = atomic_fetch_or(atomic_ptr, (uint8_t)value);
      break;
    }
    case TYPED_ARRAY_INT16: {
      _Atomic int16_t *atomic_ptr = (_Atomic int16_t *)(ptr + index * 2);
      old_value = atomic_fetch_or(atomic_ptr, (int16_t)value);
      break;
    }
    case TYPED_ARRAY_UINT16: {
      _Atomic uint16_t *atomic_ptr = (_Atomic uint16_t *)(ptr + index * 2);
      old_value = atomic_fetch_or(atomic_ptr, (uint16_t)value);
      break;
    }
    case TYPED_ARRAY_INT32: {
      _Atomic int32_t *atomic_ptr = (_Atomic int32_t *)(ptr + index * 4);
      old_value = atomic_fetch_or(atomic_ptr, value);
      break;
    }
    case TYPED_ARRAY_UINT32: {
      _Atomic uint32_t *atomic_ptr = (_Atomic uint32_t *)(ptr + index * 4);
      old_value = atomic_fetch_or(atomic_ptr, (uint32_t)value);
      break;
    }
    default:
      return js_mkerr(js, "TypedArray type not supported for atomic bitwise operations");
  }
  
  return js_mknum((double)old_value);
}

// Atomics.store(typedArray, index, value)
static ant_value_t js_atomics_store(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 3) {
    return js_mkerr(js, "Atomics.store requires 3 arguments");
  }
  
  TypedArrayData *ta_data;
  uint8_t *ptr;
  if (!get_atomic_array_data(js, args[0], &ta_data, &ptr)) {
    return js_mkerr(js, "First argument must be a TypedArray");
  }
  
  size_t index = (size_t)js_getnum(args[1]);
  if (index >= ta_data->length) {
    return js_mkerr(js, "Index out of bounds");
  }
  
  int32_t value = (int32_t)js_getnum(args[2]);
  
  switch (ta_data->type) {
    case TYPED_ARRAY_INT8: {
      _Atomic int8_t *atomic_ptr = (_Atomic int8_t *)(ptr + index);
      atomic_store(atomic_ptr, (int8_t)value);
      break;
    }
    case TYPED_ARRAY_UINT8: {
      _Atomic uint8_t *atomic_ptr = (_Atomic uint8_t *)(ptr + index);
      atomic_store(atomic_ptr, (uint8_t)value);
      break;
    }
    case TYPED_ARRAY_INT16: {
      _Atomic int16_t *atomic_ptr = (_Atomic int16_t *)(ptr + index * 2);
      atomic_store(atomic_ptr, (int16_t)value);
      break;
    }
    case TYPED_ARRAY_UINT16: {
      _Atomic uint16_t *atomic_ptr = (_Atomic uint16_t *)(ptr + index * 2);
      atomic_store(atomic_ptr, (uint16_t)value);
      break;
    }
    case TYPED_ARRAY_INT32: {
      _Atomic int32_t *atomic_ptr = (_Atomic int32_t *)(ptr + index * 4);
      atomic_store(atomic_ptr, value);
      break;
    }
    case TYPED_ARRAY_UINT32: {
      _Atomic uint32_t *atomic_ptr = (_Atomic uint32_t *)(ptr + index * 4);
      atomic_store(atomic_ptr, (uint32_t)value);
      break;
    }
    default:
      return js_mkerr(js, "TypedArray type not supported for atomic operations");
  }
  
  return js_mknum((double)value);
}

// Atomics.sub(typedArray, index, value)
static ant_value_t js_atomics_sub(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 3) {
    return js_mkerr(js, "Atomics.sub requires 3 arguments");
  }
  
  TypedArrayData *ta_data;
  uint8_t *ptr;
  if (!get_atomic_array_data(js, args[0], &ta_data, &ptr)) {
    return js_mkerr(js, "First argument must be a TypedArray");
  }
  
  size_t index = (size_t)js_getnum(args[1]);
  if (index >= ta_data->length) {
    return js_mkerr(js, "Index out of bounds");
  }
  
  int32_t value = (int32_t)js_getnum(args[2]);
  int32_t old_value;
  
  switch (ta_data->type) {
    case TYPED_ARRAY_INT8: {
      _Atomic int8_t *atomic_ptr = (_Atomic int8_t *)(ptr + index);
      old_value = atomic_fetch_sub(atomic_ptr, (int8_t)value);
      break;
    }
    case TYPED_ARRAY_UINT8: {
      _Atomic uint8_t *atomic_ptr = (_Atomic uint8_t *)(ptr + index);
      old_value = atomic_fetch_sub(atomic_ptr, (uint8_t)value);
      break;
    }
    case TYPED_ARRAY_INT16: {
      _Atomic int16_t *atomic_ptr = (_Atomic int16_t *)(ptr + index * 2);
      old_value = atomic_fetch_sub(atomic_ptr, (int16_t)value);
      break;
    }
    case TYPED_ARRAY_UINT16: {
      _Atomic uint16_t *atomic_ptr = (_Atomic uint16_t *)(ptr + index * 2);
      old_value = atomic_fetch_sub(atomic_ptr, (uint16_t)value);
      break;
    }
    case TYPED_ARRAY_INT32: {
      _Atomic int32_t *atomic_ptr = (_Atomic int32_t *)(ptr + index * 4);
      old_value = atomic_fetch_sub(atomic_ptr, value);
      break;
    }
    case TYPED_ARRAY_UINT32: {
      _Atomic uint32_t *atomic_ptr = (_Atomic uint32_t *)(ptr + index * 4);
      old_value = atomic_fetch_sub(atomic_ptr, (uint32_t)value);
      break;
    }
    default:
      return js_mkerr(js, "TypedArray type not supported for atomic operations");
  }
  
  return js_mknum((double)old_value);
}

// Atomics.xor(typedArray, index, value)
static ant_value_t js_atomics_xor(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 3) {
    return js_mkerr(js, "Atomics.xor requires 3 arguments");
  }
  
  TypedArrayData *ta_data;
  uint8_t *ptr;
  if (!get_atomic_array_data(js, args[0], &ta_data, &ptr)) {
    return js_mkerr(js, "First argument must be a TypedArray");
  }
  
  size_t index = (size_t)js_getnum(args[1]);
  if (index >= ta_data->length) {
    return js_mkerr(js, "Index out of bounds");
  }
  
  int32_t value = (int32_t)js_getnum(args[2]);
  int32_t old_value;
  
  switch (ta_data->type) {
    case TYPED_ARRAY_INT8: {
      _Atomic int8_t *atomic_ptr = (_Atomic int8_t *)(ptr + index);
      old_value = atomic_fetch_xor(atomic_ptr, (int8_t)value);
      break;
    }
    case TYPED_ARRAY_UINT8: {
      _Atomic uint8_t *atomic_ptr = (_Atomic uint8_t *)(ptr + index);
      old_value = atomic_fetch_xor(atomic_ptr, (uint8_t)value);
      break;
    }
    case TYPED_ARRAY_INT16: {
      _Atomic int16_t *atomic_ptr = (_Atomic int16_t *)(ptr + index * 2);
      old_value = atomic_fetch_xor(atomic_ptr, (int16_t)value);
      break;
    }
    case TYPED_ARRAY_UINT16: {
      _Atomic uint16_t *atomic_ptr = (_Atomic uint16_t *)(ptr + index * 2);
      old_value = atomic_fetch_xor(atomic_ptr, (uint16_t)value);
      break;
    }
    case TYPED_ARRAY_INT32: {
      _Atomic int32_t *atomic_ptr = (_Atomic int32_t *)(ptr + index * 4);
      old_value = atomic_fetch_xor(atomic_ptr, value);
      break;
    }
    case TYPED_ARRAY_UINT32: {
      _Atomic uint32_t *atomic_ptr = (_Atomic uint32_t *)(ptr + index * 4);
      old_value = atomic_fetch_xor(atomic_ptr, (uint32_t)value);
      break;
    }
    default:
      return js_mkerr(js, "TypedArray type not supported for atomic bitwise operations");
  }
  
  return js_mknum((double)old_value);
}

// Atomics.wait(typedArray, index, value, timeout)
static ant_value_t js_atomics_wait(ant_t *js, ant_value_t *args, int nargs) {
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
  
  if (nargs > 3 && vtype(args[3]) == T_NUM) {
    timeout_ms = (int64_t)js_getnum(args[3]);
  }
  
  _Atomic int32_t *atomic_ptr = (_Atomic int32_t *)(ptr + index * 4);
  int32_t current_value = atomic_load(atomic_ptr);
  
  if (current_value != expected_value) {
    return js_mkstr(js, "not-equal", 9);
  }
  
  WaitQueueEntry entry;
  pthread_cond_init(&entry.cond, NULL);
  pthread_mutex_init(&entry.mutex, NULL);
  entry.address = (int32_t *)atomic_ptr;
  entry.notified = 0;
  entry.next = NULL;
  
  wait_queue_add(&global_wait_queue, &entry);
  pthread_mutex_lock(&entry.mutex);
  
  const char *result = "ok";
  if (timeout_ms < 0) {
    while (!entry.notified) pthread_cond_wait(&entry.cond, &entry.mutex);
  } else {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
      ts.tv_sec++;
      ts.tv_nsec -= 1000000000;
    }
    
    int wait_result = pthread_cond_timedwait(&entry.cond, &entry.mutex, &ts);
    if (wait_result == ETIMEDOUT && !entry.notified) result = "timed-out";
  }
  
  pthread_mutex_unlock(&entry.mutex);
  wait_queue_remove(&global_wait_queue, &entry);
  
  pthread_cond_destroy(&entry.cond);
  pthread_mutex_destroy(&entry.mutex);
  
  return js_mkstr(js, result, strlen(result));
}

// Atomics.notify(typedArray, index, count)
static ant_value_t js_atomics_notify(ant_t *js, ant_value_t *args, int nargs) {
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
  if (nargs > 2 && vtype(args[2]) == T_NUM) {
    count = (int)js_getnum(args[2]);
  }
  
  int32_t *address = (int32_t *)(ptr + index * 4);
  int notified = wait_queue_notify(&global_wait_queue, address, count);
  
  return js_mknum((double)notified);
}

// Atomics.waitAsync(typedArray, index, value, timeout)
static ant_value_t js_atomics_waitAsync(ant_t *js, ant_value_t *args, int nargs) {
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

  if (nargs > 3 && vtype(args[3]) == T_NUM) {
    timeout_ms = js_getnum(args[3]);
    if (isnan(timeout_ms)) timeout_ms = HUGE_VAL;
    else if (timeout_ms < 0) timeout_ms = 0;
  }
  
  ant_value_t result_obj = js_mkobj(js);
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
  if (!entry) return js_mkerr(js, "Out of memory");

  entry->js = js;
  entry->promise = promise;
  entry->buffer = ta_data->buffer;
  entry->buffer->ref_count++;
  entry->address = (int32_t *)atomic_ptr;
  atomic_store(&entry->settle_state, ASYNC_WAIT_SETTLE_NONE);
  atomic_store(&entry->settle_drain_microtasks, false);

  if (uv_async_init(uv_default_loop(), &entry->async, async_waiter_async_cb) != 0) {
    async_waiter_close_handles(entry);
    return js_mkerr(js, "Failed to initialize Atomics.waitAsync notifier");
  }
  
  entry->async_initialized = true;
  entry->async.data = entry;
  entry->pending_handles++;

  if (isfinite(timeout_ms)) {
    uint64_t delay = timeout_ms > (double)UINT64_MAX ? UINT64_MAX : (uint64_t)timeout_ms;
    if (uv_timer_init(uv_default_loop(), &entry->timer) != 0) {
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
  async_waiter_add_locked(entry);
  pthread_mutex_unlock(&async_waiters_lock);

  js_set(js, result_obj, "async", js_true);
  js_set(js, result_obj, "value", promise);

  return result_obj;
}

// Atomics.pause()
static ant_value_t js_atomics_pause(ant_t *js, ant_value_t *args, int nargs) {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield");
#endif
  
  return js_mkundef();
}

void init_atomics_module(void) {
  ant_t *js = rt->js;
  
  ant_value_t glob = js_glob(js);
  ant_value_t atomics = js_mkobj(js);
  
  js_set(js, atomics, "add", js_mkfun(js_atomics_add));
  js_set(js, atomics, "and", js_mkfun(js_atomics_and));
  js_set(js, atomics, "compareExchange", js_mkfun(js_atomics_compareExchange));
  js_set(js, atomics, "exchange", js_mkfun(js_atomics_exchange));
  js_set(js, atomics, "isLockFree", js_mkfun(js_atomics_isLockFree));
  js_set(js, atomics, "load", js_mkfun(js_atomics_load));
  js_set(js, atomics, "notify", js_mkfun(js_atomics_notify));
  js_set(js, atomics, "or", js_mkfun(js_atomics_or));
  js_set(js, atomics, "pause", js_mkfun(js_atomics_pause));
  js_set(js, atomics, "store", js_mkfun(js_atomics_store));
  js_set(js, atomics, "sub", js_mkfun(js_atomics_sub));
  js_set(js, atomics, "wait", js_mkfun(js_atomics_wait));
  js_set(js, atomics, "waitAsync", js_mkfun(js_atomics_waitAsync));
  js_set(js, atomics, "xor", js_mkfun(js_atomics_xor));
  
  js_set_sym(js, atomics, get_toStringTag_sym(), js_mkstr(js, "Atomics", 7));
  js_set(js, glob, "Atomics", atomics);
}

void gc_mark_atomics(ant_t *js, gc_mark_fn mark) {
  pthread_mutex_lock(&async_waiters_lock);
  for (AsyncWaitEntry *entry = async_waiters_head; entry; entry = entry->next) {
    if (entry->js == js) mark(js, entry->promise);
  }
  pthread_mutex_unlock(&async_waiters_lock);
}
