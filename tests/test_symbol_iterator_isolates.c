#include "internal.h"
#include "gc.h"
#include "gc/roots.h"
#include "modules/symbol.h"
#include "modules/iterator.h"
#include "modules/generator.h"

#include <assert.h>
#include <stdio.h>

static ant_value_t foreign_proto;

static void CheckRoot(ant_t *js, ant_value_t value) {
  (void)js;
  assert(value != foreign_proto);
}

static ant_t *Create(void) {
  ant_t *js = ant_create();
  assert(js);
  js_setstackbase(js, NULL);
  init_symbol_module(js);
  init_intrinsic_symbols(js);
  init_iterator_module(js);
  init_generator_module(js);
  assert(!Ant_Exception_Pending(js));
  return js;
}

static ant_value_t Next(ant_params_t) {
  return js_iter_result(js, false, js_mkundef());
}

static ant_value_t OtherNext(ant_params_t) {
  return js_iter_result(js, true, js_mknum(99));
}

static bool Advance(ant_t *js, iterator_t *it, ant_value_t *out) {
  (void)js;
  (void)it;
  *out = js_mknum(17);
  return true;
}

static void CheckArray(ant_t *js) {
  GC_ROOT_SAVE(mark, js);
  ant_value_t array = js_mkarr(js);
  GC_ROOT_PIN(js, array);
  js_arr_push(js, array, js_mknum(42));
  iterator_t it;
  assert(js_iter_open(js, array, &it));
  GC_ROOT_PIN(js, it.iterator);
  assert(it.advance);
  ant_value_t value;
  assert(js_iter_next(js, &it, &value) && js_getnum(value) == 42);
  assert(!js_iter_next(js, &it, &value));
  ant_value_t ctor = js_get(js, js->global, "Symbol");
  assert(js_get(js, ctor, "iterator") == js->sym.iterator_sym);
  assert(js_mksym_for(js, "shared") == js_mksym_for(js, "shared"));
  assert(!Ant_Exception_Pending(js));
  GC_ROOT_RESTORE(js, mark);
}

static void CheckGrowth(ant_t *js) {
  ant_value_t protos[24];
  size_t before = js->iterators.len;
  for (size_t i = 0; i < 24; i++) {
    protos[i] = js_mkobj(js);
    js_set(js, protos[i], "next", js_mkfun(Next));
    js_set_sym(js, protos[i], js->sym.iterator_sym, js_mkfun(sym_this_cb));
    js_iter_register_advance(js, protos[i], Advance);
  }
  assert(js->iterators.len == before + 24);

  // The native registry is the sole root for these prototypes and methods.
  gc_run(js);
  gc_run(js);
  for (size_t i = 0; i < 24; i++) {
    GC_ROOT_SAVE(mark, js);
    ant_value_t iterator = js_mkobj(js);
    GC_ROOT_PIN(js, iterator);
    js_set_proto_init(iterator, protos[i]);
    iterator_t it;
    assert(js_iter_open(js, iterator, &it) && it.advance == Advance);
    ant_value_t value;
    assert(js_iter_next(js, &it, &value) && js_getnum(value) == 17);
    js_set(js, iterator, "next", js_mkfun(OtherNext));
    assert(js_iter_open(js, iterator, &it) && !it.advance);
    assert(js_iter_next(js, &it, &value) && js_getnum(value) == 99);
    GC_ROOT_RESTORE(js, mark);
  }
}

int main(void) {
  for (int destroy_first = 0; destroy_first < 2; destroy_first++) {
    ant_t *first = Create();
    ant_t *second = Create();
    assert(first->sym.iterator_sym != second->sym.iterator_sym);
    foreign_proto = second->sym.array_iterator_proto;
    gc_visit_roots(first, CheckRoot);
    CheckArray(first);
    CheckArray(second);
    gc_run(first);
    gc_run(second);
    CheckArray(first);
    CheckArray(second);
    ant_t *survivor = destroy_first ? second : first;
    js_destroy(destroy_first ? first : second);
    gc_run(survivor);
    CheckArray(survivor);
    CheckGrowth(survivor);
    js_destroy(survivor);
  }
  for (int i = 0; i < 12; i++) {
    ant_t *js = Create();
    CheckArray(js);
    js_destroy(js);
  }
  puts("PASS symbol and iterator isolate ownership, teardown, roots, and registry growth");
}
