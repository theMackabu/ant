// meson test -C build rope-mark
#include "internal.h"
#include "gc/ropes.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static ant_pool_block_t *test_block(size_t size) {
  ant_pool_block_t *block = calloc(1, sizeof(*block) + size);
  assert(block);
  block->used = block->cap = size;
  return block;
}

int main(void) {
  ant_t js = {0};
  const size_t size = sizeof(ant_rope_heap_t);
  const size_t align = _Alignof(ant_rope_heap_t);
  ant_pool_block_t *young = test_block(2 * size);
  ant_pool_block_t *old = test_block(size);
  ant_pool_block_t *misc = test_block(sizeof(ant_string_builder_t));
  js.rope_gc.young.head = young;
  js.rope_gc.old.head = old;
  js.pool.rope.head = misc;

  assert(gc_ropes_begin(&js, true) == GC_ROPES_BEGIN_NORMAL);
  assert(gc_ropes_mark(NULL, young->data, size, align) == GC_ROPE_MARK_INVALID);
  assert(gc_ropes_mark(&js, NULL, size, align) == GC_ROPE_MARK_INVALID);
  assert(gc_ropes_mark(&js, young, size, align) == GC_ROPE_MARK_INVALID);
  assert(gc_ropes_mark(&js, young->data, 0, align) == GC_ROPE_MARK_INVALID);
  assert(gc_ropes_mark(&js, young->data, size, 3) == GC_ROPE_MARK_INVALID);
  assert(gc_ropes_mark(&js, young->data + 1, size, align) == GC_ROPE_MARK_INVALID);
  assert(gc_ropes_mark(&js, young->data + align, size, align) == GC_ROPE_MARK_INVALID);
  assert(gc_ropes_mark(&js, young->data + size, size + 1, align) == GC_ROPE_MARK_INVALID);
  assert(gc_ropes_mark(&js, young->data + 2 * size, size, align) == GC_ROPE_MARK_INVALID);

  assert(gc_ropes_mark(&js, young->data, size, align) == GC_ROPE_MARK_TRACE);
  assert(gc_ropes_mark(&js, young->data, size, align) == GC_ROPE_MARK_SKIP);
  assert(gc_ropes_mark(&js, young->data + size, size, align) == GC_ROPE_MARK_TRACE);
  assert(gc_ropes_mark(&js, old->data, size, align) == GC_ROPE_MARK_SKIP);
  assert(((ant_rope_heap_t *)old->data)->mark_epoch == 0);

  // Builders/chunks use a mixed pool with block marking, not per-rope epochs.
  assert(gc_ropes_mark(&js, misc->data, misc->used, align) == GC_ROPE_MARK_TRACE);
  assert(gc_ropes_mark(&js, misc->data, misc->used, align) == GC_ROPE_MARK_TRACE);
  assert(gc_ropes_mark(&js, misc->data, misc->used + 1, align) == GC_ROPE_MARK_INVALID);
  assert(gc_ropes_mark(&js, misc->data + 1, 1, align) == GC_ROPE_MARK_INVALID);

  assert(gc_ropes_begin(&js, false) == GC_ROPES_BEGIN_NORMAL);
  assert(gc_ropes_mark(&js, young->data, size, align) == GC_ROPE_MARK_TRACE);
  assert(gc_ropes_mark(&js, old->data, size, align) == GC_ROPE_MARK_TRACE);
  assert(gc_ropes_mark(&js, old->data, size, align) == GC_ROPE_MARK_SKIP);

  js.rope_gc.mark_epoch = UINT32_MAX;
  assert(gc_ropes_begin(&js, false) == GC_ROPES_BEGIN_NORMAL);
  assert(js.rope_gc.mark_epoch == 1);
  assert(gc_ropes_mark(&js, young->data, size, align) == GC_ROPE_MARK_TRACE);
  assert(gc_ropes_mark(&js, old->data, size, align) == GC_ROPE_MARK_TRACE);

  // Rebuild the index after a block is retired; cached ranges cannot survive.
  js.rope_gc.old.head = NULL;
  assert(gc_ropes_begin(&js, false) == GC_ROPES_BEGIN_NORMAL);
  assert(gc_ropes_mark(&js, old->data, size, align) == GC_ROPE_MARK_INVALID);
  assert(gc_ropes_mark(&js, young->data, size, align) == GC_ROPE_MARK_TRACE);
  assert(gc_ropes_mark(&js, young->data + size, size + 1, align) == GC_ROPE_MARK_INVALID);

  assert(gc_ropes_mark(&js, misc->data, misc->used, align) == GC_ROPE_MARK_TRACE);
  gc_ropes_sweep(&js, false);
  assert(gc_ropes_mark(&js, misc->data, misc->used, align) == GC_ROPE_MARK_INVALID);
  assert(gc_ropes_begin(&js, true) == GC_ROPES_BEGIN_NORMAL);
  assert(gc_ropes_mark(&js, young->data, size, align) == GC_ROPE_MARK_SKIP);

  // A separate isolate has no ownership of the first isolate's pool blocks.
  ant_t other = {0};
  assert(gc_ropes_begin(&other, false) == GC_ROPES_BEGIN_NORMAL);
  assert(gc_ropes_mark(&other, young->data, size, align) == GC_ROPE_MARK_INVALID);
  free(other.rope_gc.marks);

  free(js.rope_gc.marks);
  free(misc);
  free(old);
  free(young);
  puts("PASS rope marking validates ranges and preserves collection epochs");
}
