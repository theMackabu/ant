#include "gc.h"
#include "gc/roots.h"
#include "internal.h" // IWYU pragma: keep

#include <stddef.h>
#include <stdlib.h>

static ant_value_t *g_roots[GC_MAX_STATIC_ROOTS];
static size_t g_root_count = 0;

void gc_register_root(ant_value_t *slot) {
  if (g_root_count < GC_MAX_STATIC_ROOTS)
    g_roots[g_root_count++] = slot;
}

size_t gc_root_scope(ant_t *js) {
  return js ? js->c_root_count : 0;
}

bool gc_push_root(ant_t *js, ant_value_t *slot) {
  if (!js || !slot) return false;

  if (js->c_root_count >= js->c_root_cap) {
    size_t new_cap = js->c_root_cap ? js->c_root_cap * 2 : 64;
    ant_value_t **next = realloc(js->c_roots, new_cap * sizeof(*next));
    if (!next) return false;
    js->c_roots = next;
    js->c_root_cap = new_cap;
  }

  js->c_roots[js->c_root_count++] = slot;
  return true;
}

bool gc_pin_permanent(ant_t *js, ant_value_t value) {
  if (!js || !is_tagged(value)) return false;

  uint8_t type = vtype_tagged(value);
  if (((1u << type) & GC_OBJ_TYPE_MASK) == 0) return false;
  if (type == kTypeError && vdata(value) < 2) return false;

  ant_object_t *obj = (ant_object_t *)vptr(value);
  if (!obj) return false;
  if (obj->flags.gc_permanent) return true;

  if (js->permanent_root_len >= js->permanent_root_cap) {
    size_t new_cap = js->permanent_root_cap ? js->permanent_root_cap * 2 : 64;
    ant_object_t **next = realloc(js->permanent_roots, new_cap * sizeof(*next));
    if (!next) return false;
    
    js->permanent_roots = next;
    js->permanent_root_cap = new_cap;
  }

  obj->flags.gc_permanent = 1;
  js->permanent_roots[js->permanent_root_len++] = obj;
  
  return true;
}

void gc_pop_roots(ant_t *js, size_t mark) {
  if (!js) return;
  js->c_root_count = (mark <= js->c_root_count) ? mark : 0;
}

void gc_temp_root_scope_begin(ant_t *js, gc_temp_root_scope_t *scope) {
  if (!scope) return;
  scope->js = js;
  scope->items = NULL;
  scope->len = 0;
  scope->cap = 0;
  scope->prev = js ? js->temp_roots : NULL;
  if (js) js->temp_roots = scope;
}

void gc_temp_root_scope_end(gc_temp_root_scope_t *scope) {
  if (!scope) return;

  ant_t *js = scope->js;
  if (js) for (gc_temp_root_scope_t **link = &js->temp_roots; *link; link = &(*link)->prev) {
    if (*link != scope) continue;
    *link = scope->prev;
    break;
  }

  free(scope->items);
  scope->items = NULL;
  scope->len = 0;
  scope->cap = 0;
  scope->prev = NULL;
  scope->js = NULL;
}

static bool gc_temp_root_grow(gc_temp_root_scope_t *scope, size_t extra) {
  if (extra > SIZE_MAX / sizeof(ant_value_t) - scope->len) return false;
  size_t needed = scope->len + extra;
  if (needed <= scope->cap) return true;

  size_t new_cap = scope->cap ? scope->cap : 16;
  while (new_cap < needed) new_cap = new_cap > SIZE_MAX / 2 / sizeof(ant_value_t) ? needed : new_cap * 2;
  
  ant_value_t *next = realloc(scope->items, new_cap * sizeof(*next));
  if (!next) return false;

  scope->items = next;
  scope->cap = new_cap;
  
  return true;
}

bool gc_temp_root_push(gc_temp_root_scope_t *scope, ant_value_t value) {
  if (!scope || !gc_temp_root_grow(scope, 1)) return false;
  scope->items[scope->len++] = value;
  return true;
}

ant_value_t *gc_temp_root_slots(gc_temp_root_scope_t *scope, size_t count) {
  if (!scope || !gc_temp_root_grow(scope, count ? count : 1)) return NULL;
  ant_value_t *slots = scope->items + scope->len;
  for (size_t i = 0; i < count; i++) slots[i] = js_mkundef();
  scope->len += count;
  return slots;
}

gc_temp_root_handle_t gc_temp_root_add(gc_temp_root_scope_t *scope, ant_value_t value) {
  gc_temp_root_handle_t handle = {0};
  if (!gc_temp_root_push(scope, value)) return handle;
  handle.scope = scope;
  handle.index = scope->len - 1;
  return handle;
}

void gc_temp_root_truncate(gc_temp_root_scope_t *scope, size_t mark) {
  if (scope && mark < scope->len) scope->len = mark;
}

bool gc_temp_root_set(gc_temp_root_handle_t handle, ant_value_t value) {
  if (!handle.scope || handle.index >= handle.scope->len) return false;
  handle.scope->items[handle.index] = value;
  return true;
}

static void gc_visit_value_slots(
  ant_t *js, gc_root_visitor_t visitor, 
  const ant_value_t *slots, size_t count
) {
  for (size_t i = 0; i < count; i++) if (slots[i]) visitor(js, slots[i]);
}

static void gc_visit_isolate_values(ant_t *js, gc_root_visitor_t visitor) {
  #define ANT_BUILTIN(name)             gc_visit_value_slots(js, visitor, &js->builtins.name, 1);
  #define ANT_BUILTIN_ARR(name, n)      gc_visit_value_slots(js, visitor, js->builtins.name, n);
  #define ANT_MUTABLE_ROOT(name)        gc_visit_value_slots(js, visitor, &js->mutable_roots.name, 1);
  #define ANT_MUTABLE_ROOT_ARR(name, n) gc_visit_value_slots(js, visitor, js->mutable_roots.name, n);
  #include "isolate_values.h"
}

void gc_visit_roots(ant_t *js, gc_root_visitor_t visitor) {
  for (size_t i = 0; i < g_root_count; i++)
    if (g_roots[i] && *g_roots[i]) visitor(js, *g_roots[i]);

  if (!js) return;
  gc_visit_isolate_values(js, visitor);

  for (size_t i = 0; i < js->c_root_count; i++) {
    ant_value_t *slot = js->c_roots[i];
    if (slot && *slot) visitor(js, *slot);
  }

  for (gc_temp_root_scope_t *scope = js->temp_roots; scope; scope = scope->prev) 
    for (size_t i = 0; i < scope->len; i++) if (scope->items[i]) visitor(js, scope->items[i]);
}
