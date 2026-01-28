#include "stack.h"
#include "arena.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

call_stack_t global_call_stack = {NULL, 0, 0};

void pop_call_frame(void) {
  if (global_call_stack.depth == 0) return;
  call_frame_t *frame = &global_call_stack.frames[--global_call_stack.depth];
  free((void *)frame->function_name);
}

bool push_call_frame(const char *filename, const char *function_name, const char *code, uint32_t pos) {
  if (global_call_stack.depth >= global_call_stack.capacity) {
    size_t new_capacity = global_call_stack.capacity == 0 ? 32U : (size_t)global_call_stack.capacity * 2U;
    if (new_capacity > SIZE_MAX / sizeof(call_frame_t)) abort();

    size_t alloc_size = new_capacity * sizeof(call_frame_t);
    call_frame_t *new_stack = (call_frame_t *)ant_realloc(global_call_stack.frames, alloc_size);
    if (!new_stack) abort();

    global_call_stack.frames = new_stack;
    global_call_stack.capacity = (int)new_capacity;
  }

  char *func_name_copy = NULL;
  if (function_name) {
    size_t len = strlen(function_name) + 1;
    func_name_copy = (char *)ant_realloc(NULL, len);
    if (!func_name_copy) abort();
    memcpy(func_name_copy, function_name, len);
  }
  
  call_frame_t *frame = 
    &global_call_stack.frames[global_call_stack.depth++];
  
  *frame = (call_frame_t) {
    .filename = filename,
    .function_name = func_name_copy,
    .code = code,
    .pos = pos, .line = -1, .col = -1,
  };

  return true;
}
