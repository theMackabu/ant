#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

#include "ant.h"
#include "errors.h"
#include "internal.h"
#include "runtime.h"

#include "modules/buffer.h"
#include "modules/atomics.h"
#include "modules/symbol.h"

static WaitQueue global_wait_queue;
static pthread_once_t wait_queue_init_once = PTHREAD_ONCE_INIT;

static void init_wait_queue(void) {
  wait_queue_init(&global_wait_queue);
}

void wait_queue_init(WaitQueue *queue) {
  queue->head = NULL;
  pthread_mutex_init(&queue->lock, NULL);
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
    }
    current = current->next;
  }
  
  pthread_mutex_unlock(&queue->lock);
  return notified;
}

static bool get_atomic_array_data(struct js *js, jsval_t this_val, TypedArrayData **out_data, uint8_t **out_ptr) {
  jsval_t ta_data_val = js_get_slot(js, this_val, SLOT_BUFFER);
  TypedArrayData *ta_data = (TypedArrayData *)js_gettypedarray(ta_data_val);
  if (!ta_data || !ta_data->buffer) return false;
  
  *out_data = ta_data;
  *out_ptr = ta_data->buffer->data + ta_data->byte_offset;
  return true;
}

// Atomics.add(typedArray, index, value)
static jsval_t js_atomics_add(struct js *js, jsval_t *args, int nargs) {
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
static jsval_t js_atomics_and(struct js *js, jsval_t *args, int nargs) {
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
static jsval_t js_atomics_compareExchange(struct js *js, jsval_t *args, int nargs) {
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
static jsval_t js_atomics_exchange(struct js *js, jsval_t *args, int nargs) {
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
static jsval_t js_atomics_isLockFree(struct js *js, jsval_t *args, int nargs) {
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
  
  return is_lock_free ? js_mktrue() : js_mkfalse();
}

// Atomics.load(typedArray, index)
static jsval_t js_atomics_load(struct js *js, jsval_t *args, int nargs) {
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
static jsval_t js_atomics_or(struct js *js, jsval_t *args, int nargs) {
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
static jsval_t js_atomics_store(struct js *js, jsval_t *args, int nargs) {
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
static jsval_t js_atomics_sub(struct js *js, jsval_t *args, int nargs) {
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
static jsval_t js_atomics_xor(struct js *js, jsval_t *args, int nargs) {
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
static jsval_t js_atomics_wait(struct js *js, jsval_t *args, int nargs) {
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
static jsval_t js_atomics_notify(struct js *js, jsval_t *args, int nargs) {
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
static jsval_t js_atomics_waitAsync(struct js *js, jsval_t *args, int nargs) {
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
  
  jsval_t result_obj = js_mkobj(js);
  if (current_value != expected_value) {
    js_set(js, result_obj, "async", js_mkfalse());
    js_set(js, result_obj, "value", js_mkstr(js, "not-equal", 9));
    return result_obj;
  }
  
  jsval_t promise = js_mkpromise(js);
  js_set(js, result_obj, "async", js_mktrue());
  js_set(js, result_obj, "value", promise);
  js_resolve_promise(js, promise, js_mkstr(js, "ok", 2));
  
  return result_obj;
}

// Atomics.pause()
static jsval_t js_atomics_pause(struct js *js, jsval_t *args, int nargs) {
  (void)js;
  (void)args;
  (void)nargs;
  
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield");
#endif
  
  return js_mkundef();
}

void init_atomics_module(void) {
  struct js *js = rt->js;
  
  jsval_t glob = js_glob(js);
  jsval_t atomics = js_mkobj(js);
  
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
  
  js_set(js, atomics, get_toStringTag_sym_key(), js_mkstr(js, "Atomics", 7));
  js_set(js, glob, "Atomics", atomics);
}
