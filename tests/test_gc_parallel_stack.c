#include "internal.h"
#include "gc/parallel.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static ant_object_t *item(size_t n) {
  return (ant_object_t *)(uintptr_t)((n + 1) * 16);
}

static void push(gc_par_stack_t *stack, size_t n) {
  if (stack->len >= stack->cap) gc_par_stack_room(stack);
  stack->items[stack->len++] = item(n);
}

int main(void) {
  gc_par_stack_t stack = {0};
  ant_object_t *out[256];

  for (size_t i = 0; i < 6000; i++) push(&stack, i);
  assert(stack.cap == 8192 && stack.len == 6000 && stack.base == 0);

  size_t given = 0;
  while (stack.base < stack.cap / 4) {
    size_t count = gc_par_stack_give(&stack, out, 256);
    assert(count == 256);
    for (size_t i = 0; i < count; i++) assert(out[i] == item(given + i));
    given += count;
  }
  assert(stack.base == given && stack.len == 6000);

  size_t next = 6000;
  while (stack.len < stack.cap) push(&stack, next++);
  ant_object_t **items = stack.items;
  push(&stack, next++);
  assert(stack.items == items && stack.cap == 8192 && stack.base == 0);
  assert(stack.len == next - given);
  for (size_t i = 0; i < stack.len; i++) assert(stack.items[i] == item(given + i));

  assert(gc_par_stack_give(&stack, out, 256) == 256);
  assert(out[0] == item(given) && out[255] == item(given + 255));
  given += 256;
  assert(stack.base == 256);

  while (stack.len < stack.cap) push(&stack, next++);
  push(&stack, next++);
  assert(stack.cap == 16384 && stack.base == 256);
  for (size_t i = stack.base; i < stack.len; i++) assert(stack.items[i] == item(given + i - stack.base));

  stack.len = stack.base + 3;
  assert(gc_par_stack_give(&stack, out, 256) == 1 && out[0] == item(given));
  stack.len = stack.base + 1;
  assert(gc_par_stack_give(&stack, out, 256) == 0);

  free(stack.items);
  puts("PASS parallel mark stack: donation from the base, compaction and growth keep the held items");
}
