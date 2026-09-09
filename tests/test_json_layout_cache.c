// meson test -C build json-layout-cache
#include "internal.h"
#include "modules/json.h"
#include "gc.h"
#include "gc/roots.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static ant_value_t parse_record(ant_t *js, unsigned count, unsigned layout) {
  size_t cap = (size_t)count * 48 + 3;
  char *text = malloc(cap);
  assert(text);
  size_t len = 0;
  text[len++] = '{';
  for (unsigned i = 0; i < count; i++)
    len += snprintf(text + len, cap - len, "%s\"layout%u_key%u\":%u", i ? "," : "", layout, i, i);
  text[len++] = '}';
  ant_value_t source = js_mkstr(js, text, len);
  free(text);
  GC_ROOT_SAVE(roots, js);
  GC_ROOT_PIN(js, source);
  ant_value_t value = json_parse_value(js, source);
  GC_ROOT_RESTORE(js, roots);
  assert(vtype(value) == kTypeObject);
  return value;
}

static void check_budget(ant_t *js) {
  json_layout_cache_t *cache = js->json_layout_cache;
  assert(cache);
  assert(cache->count <= JSON_LAYOUT_CACHE_ENTRIES);
  assert(cache->bytes <= JSON_LAYOUT_CACHE_BYTES);
  size_t bytes = 0;
  for (unsigned i = 0; i < cache->count; i++) bytes += cache->entries[i].bytes;
  assert(bytes == cache->bytes);
}

int main(void) {
  char stack_base;
  ant_t *js = ant_create();
  assert(js);
  js_setstackbase(js, &stack_base);
  GC_ROOT_SAVE(roots, js);
  ant_value_t a = js_mkundef(), b = js_mkundef(), c = js_mkundef();
  GC_ROOT_PIN(js, a);
  GC_ROOT_PIN(js, b);
  GC_ROOT_PIN(js, c);

  for (unsigned width = 128; width <= 512; width *= 2) {
    a = parse_record(js, width, 0);
    b = parse_record(js, width, 0);
    assert(js_obj_ptr(a)->shape == js_obj_ptr(b)->shape);
    check_budget(js);
  }

  // Cached bulk layouts detach on append without recording a transition.
  ant_shape_t *original = js_obj_ptr(a)->shape;
  js_set(js, a, "extra", js_mknum(1));
  js_set(js, b, "extra", js_mknum(2));
  assert(js_getnum(js_get(js, a, "extra")) == 1);
  assert(js_getnum(js_get(js, b, "extra")) == 2);
  assert(js_obj_ptr(a)->shape != original);
  assert(js_obj_ptr(a)->shape != js_obj_ptr(b)->shape);
  assert(!ant_shape_is_shared(js_obj_ptr(a)->shape));
  assert(!ant_shape_is_shared(js_obj_ptr(b)->shape));
  c = parse_record(js, 512, 0);
  assert(js_obj_ptr(c)->shape == original);
  assert(vtype(js_get(js, c, "extra")) == kTypeUndefined);

  json_layout_cache_clear(js);
  a = parse_record(js, 128, 0);
  b = parse_record(js, 128, 1);
  // Force a fingerprint collision: exact key comparison must reject layout 1.
  js->json_layout_cache->entries[0].hash = js->json_layout_cache->entries[1].hash;
  c = parse_record(js, 128, 0);
  assert(js_obj_ptr(c)->shape == js_obj_ptr(a)->shape);
  assert(vtype(js_get(js, c, "layout1_key0")) == kTypeUndefined);
  json_layout_cache_clear(js);

  // More distinct layouts than cache entries evict the oldest, without changing
  // live records. Retaining a keeps pointer identity meaningful after eviction.
  a = parse_record(js, 128, 0);
  original = js_obj_ptr(a)->shape;
  for (unsigned i = 1; i <= JSON_LAYOUT_CACHE_ENTRIES; i++) {
    b = parse_record(js, 128, i);
    check_budget(js);
  }
  c = parse_record(js, 128, 0);
  assert(js_obj_ptr(c)->shape != original);
  assert(js_getnum(js_get(js, a, "layout0_key127")) == 127);

  // The byte budget also evicts before the entry limit for larger records.
  json_layout_cache_clear(js);
  for (unsigned i = 0; i < JSON_LAYOUT_CACHE_ENTRIES; i++) {
    b = parse_record(js, 1024, i);
    check_budget(js);
  }
  assert(js->json_layout_cache->count < JSON_LAYOUT_CACHE_ENTRIES);
  unsigned cached_count = js->json_layout_cache->count;
  size_t cached_bytes = js->json_layout_cache->bytes;
  b = parse_record(js, 10000, 0);
  assert(js->json_layout_cache->count == cached_count);
  assert(js->json_layout_cache->bytes == cached_bytes);

  // A layout survives collection of its records through the bounded strong
  // cache reference, and a subsequent parse can still use it safely.
  json_layout_cache_clear(js);
  a = parse_record(js, 128, 99);
  original = js_obj_ptr(a)->shape;
  a = b = c = js_mkundef();
  gc_run(js);
  a = parse_record(js, 128, 99);
  assert(js_obj_ptr(a)->shape == original);
  check_budget(js);
  json_layout_cache_clear(js);
  assert(!js->json_layout_cache);
  assert(js_getnum(js_get(js, a, "layout99_key127")) == 127);
  // Leave an entry for runtime teardown to release.
  b = parse_record(js, 128, 100);
  GC_ROOT_RESTORE(js, roots);
  js_destroy(js);
  puts("PASS JSON layout identity, collision checks, mutation, budgets, eviction and GC");
}
