#pragma once

#include <stdint.h>
#include <string.h>

#include "gc.h"
#include "internal.h"

static inline uint32_t Stream_Queue_Len(ant_value_t arr, uint32_t head) {
  if (vtype(arr) != kTypeArray) return 0;
  uint32_t len = js_obj_ptr(arr)->u.array.len;
  return head < len ? len - head : 0;
}

static inline ant_value_t Stream_Queue_At(ant_value_t arr, uint32_t head, uint32_t i) {
  return js_obj_ptr(arr)->u.array.data[head + i];
}

static inline ant_value_t Stream_Queue_Peek(ant_value_t arr, uint32_t head) {
  return Stream_Queue_Len(arr, head) ? Stream_Queue_At(arr, head, 0) : Ant_Value_Undefined;
}

static inline void Stream_Queue_Clear(ant_value_t arr, uint32_t *head) {
  if (vtype(arr) == kTypeArray) js_obj_ptr(arr)->u.array.len = 0;
  *head = 0;
}

static inline ant_value_t Stream_Queue_Shift(ant_value_t arr, uint32_t *head) {
  if (!Stream_Queue_Len(arr, *head)) return Ant_Value_Undefined;
  ant_object_t *aobj = js_obj_ptr(arr);
  uint32_t len = aobj->u.array.len;

  ant_value_t val = aobj->u.array.data[*head];
  aobj->u.array.data[(*head)++] = Ant_Value_Undefined;

  if (*head == len) {
    aobj->u.array.len = 0;
    *head = 0;
  } else if (*head >= len - *head) {
    uint32_t live = len - *head;
    memmove(aobj->u.array.data, aobj->u.array.data + *head, live * sizeof(ant_value_t));
    gc_elements_moved(aobj);
    aobj->u.array.len = live;
    *head = 0;
  }

  return val;
}

static inline bool Stream_Sizes_Shift(double *sizes, uint32_t *len, uint32_t *head, double *out) {
  if (*head >= *len) return false;
  *out = sizes[(*head)++];

  if (*head == *len) {
    *len = 0;
    *head = 0;
  } else if (*head >= *len - *head) {
    *len -= *head;
    memmove(sizes, sizes + *head, *len * sizeof(double));
    *head = 0;
  }

  return true;
}
