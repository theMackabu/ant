#ifndef ANT_STACK_H
#define ANT_STACK_H

#include <stdbool.h>
#include <stdint.h>

typedef struct call_frame {
  const char *filename;
  const char *function_name;
  const char *code;
  uint32_t pos; int line; int col;
} call_frame_t;

typedef struct {
  call_frame_t *frames;
  int depth; int capacity;
} call_stack_t;

extern call_stack_t global_call_stack;

void pop_call_frame(void);

bool push_call_frame(
  const char *filename,
  const char *function_name,
  const char *code, 
  uint32_t pos
);

#endif
