#include "stack.h"
#include "arena.h"

#include <stdint.h>
#include <stdlib.h>

call_stack_t global_call_stack = {NULL, 0, 0};

void pop_call_frame(void) {
  if (global_call_stack.depth > 0) global_call_stack.depth--;
}

bool push_call_frame(const char *filename, const char *function_name, const char *code, uint32_t pos) {
  if (global_call_stack.depth >= global_call_stack.capacity) {
    int new_capacity = global_call_stack.capacity == 0 ? 32 : global_call_stack.capacity * 2;
    if ((size_t)new_capacity > SIZE_MAX / sizeof(call_frame_t)) return false;
    
    call_frame_t *new_stack = (call_frame_t *)ant_realloc(global_call_stack.frames, new_capacity * sizeof(call_frame_t));
    if (!new_stack) return false;
    
    global_call_stack.frames = new_stack;
    global_call_stack.capacity = new_capacity;
  }

  call_frame_t *frame = &global_call_stack.frames[global_call_stack.depth++];
  
  *frame = (call_frame_t) {
    .filename = filename,
    .function_name = function_name,
    .code = code,
    .pos = pos,
    .line = -1,
    .col = -1,
  };

  return true;
}