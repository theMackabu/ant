// meson test -C build typedarray-from-cleanup
#include "modules/iterator.h"
#include "internal.h"
#include "gc/roots.h"
#include <assert.h>
#include <stdlib.h>

static bool fail_realloc;
static unsigned root_calls, fail_root_at;

static void *FailingRealloc(void *ptr, size_t size) {
  if (fail_realloc) { fail_realloc = false; return NULL; }
  return realloc(ptr, size);
}

static gc_temp_root_handle_t FailingTempRootAdd(gc_temp_root_scope_t *scope, ant_value_t value) {
  if (++root_calls == fail_root_at) return (gc_temp_root_handle_t){0};
  return gc_temp_root_add(scope, value);
}

// Inject failures only in this module's calls, without runtime test hooks.
#define realloc FailingRealloc
#define gc_temp_root_add FailingTempRootAdd
#include "../src/modules/buffer.c"
#undef gc_temp_root_add
#undef realloc

static ant_value_t iterator, original_reason, close_reason;
static unsigned next_calls, close_calls, item_limit;
static bool fail_next, fail_close;

static ant_value_t IteratorSelf(ant_params_t) { return js->this_val; }

static ant_value_t IteratorNext(ant_params_t) {
  if (fail_next) return js_throw(js, original_reason);
  unsigned index = next_calls++;
  return js_iter_result(js, index < item_limit, js_mknum(index));
}

static ant_value_t IteratorReturn(ant_params_t) {
  close_calls++;
  assert(js->this_val == iterator);
  assert(!Ant_Exception_Pending(js));
  gc_run_minor(js);
  gc_run(js);
  return fail_close ? js_throw(js, close_reason) : js_mkobj(js);
}

static ant_value_t ThrowingMapper(ant_params_t) {
  return js_throw(js, original_reason);
}

enum { MAPPER, GROW, ITEM_ROOT, INITIAL_ROOT, RESULT_ROOT, NEXT, NORMAL };

static void CheckCleanup(ant_t *js, int failure, int close_kind) {
  next_calls = close_calls = root_calls = 0;
  item_limit = failure == GROW ? 32 : 2;
  fail_next = failure == NEXT;
  fail_close = close_kind != 0;
  if (close_kind == 2) {
    js_set_getter_desc(js, iterator, "return", 6, js_mkfun(IteratorReturn), JS_DESC_C);
  } else {
    js_delete_prop(js, iterator, "return", 6);
    js_set(js, iterator, "return", js_mkfun(IteratorReturn));
  }

  fail_realloc = failure == GROW;
  fail_root_at = failure == INITIAL_ROOT ? 1 : failure == ITEM_ROOT ? 4 : failure == RESULT_ROOT ? 6 : 0;
  gc_temp_root_scope_t *previous_roots = js->temp_roots;
  ant_value_t args[] = { iterator, js_mkfun(ThrowingMapper) };
  ant_value_t result = js_typedarray_from(js, args, failure == MAPPER ? 2 : 1, TYPED_ARRAY_UINT8, "Uint8Array");
  assert(js->temp_roots == previous_roots);
  assert(close_calls == (failure == INITIAL_ROOT ? 0 : 1));
  assert(!fail_realloc);
  if (fail_root_at) assert(root_calls >= fail_root_at);
  fail_root_at = 0;

  if (failure == NORMAL && !fail_close) {
    assert(!is_err(result) && !Ant_Exception_Pending(js));
    TypedArrayData *data = buffer_get_typedarray_data(result);
    assert(data && data->length == 2 && data->buffer->data[0] == 0 && data->buffer->data[1] == 1);
  } else {
    assert(is_err(result) && Ant_Exception_Peek(js) == result);
    ant_value_t reason = js_take_thrown(js, result);
    if (failure == MAPPER || failure == NEXT) assert(reason == original_reason);
    else if (failure == NORMAL) assert(reason == close_reason);
    else {
      ant_value_t message = js_get(js, reason, "message");
      assert(vtype(message) == kTypeString && strcmp(js_getstr(js, message, NULL), "oom") == 0);
    }
    assert(!Ant_Exception_Pending(js));
  }
}

int main(void) {
  ant_t *js = ant_create();
  assert(js);
  init_symbol_module(js);
  init_intrinsic_symbols(js);
  init_iterator_module(js);
  init_buffer_module(js);
  GC_ROOT_SAVE(mark, js);
  iterator = js_mkobj(js);
  GC_ROOT_PIN(js, iterator);
  original_reason = js_mkobj(js);
  GC_ROOT_PIN(js, original_reason);
  close_reason = js_mknull();
  js_set_sym(js, iterator, js->sym.iterator_sym, js_mkfun(IteratorSelf));
  js_set(js, iterator, "next", js_mkfun(IteratorNext));
  js_setstackbase(js, NULL);

  for (int close_kind = 0; close_kind < 3; close_kind++) {
    for (int failure = MAPPER; failure <= NORMAL; failure++) {
      // The result-root failure happens after the normal close succeeds.
      if (failure == RESULT_ROOT && close_kind != 0) continue;
      CheckCleanup(js, failure, close_kind);
    }
  }

  GC_ROOT_RESTORE(js, mark);
  js_destroy(js);
  cleanup_buffer_module();
  puts("PASS TypedArray.from closes once and preserves abrupt completions");
}
