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
  if (js && js->temp_roots == scope) js->temp_roots = scope->prev;

  free(scope->items);
  scope->items = NULL;
  scope->len = 0;
  scope->cap = 0;
  scope->prev = NULL;
  scope->js = NULL;
}

gc_temp_root_handle_t gc_temp_root_add(gc_temp_root_scope_t *scope, ant_value_t value) {
  gc_temp_root_handle_t invalid = {0};
  if (!scope) return invalid;

  if (scope->len >= scope->cap) {
    size_t new_cap = scope->cap ? scope->cap * 2 : 16;
    ant_value_t *next = realloc(scope->items, new_cap * sizeof(*next));
    if (!next) return invalid;
    scope->items = next;
    scope->cap = new_cap;
  }

  size_t index = scope->len++;
  scope->items[index] = value;
  gc_temp_root_handle_t handle = {
    .scope = scope,
    .index = index,
  };
  
  return handle;
}

bool gc_temp_root_set(gc_temp_root_handle_t handle, ant_value_t value) {
  if (!handle.scope || handle.index >= handle.scope->len) return false;
  handle.scope->items[handle.index] = value;
  return true;
}

ant_value_t gc_temp_root_get(gc_temp_root_handle_t handle) {
  if (!handle.scope || handle.index >= handle.scope->len) return js_mkundef();
  return handle.scope->items[handle.index];
}

void gc_visit_roots(ant_t *js, gc_root_visitor_t visitor) {
  for (size_t i = 0; i < g_root_count; i++) 
    if (g_roots[i] && *g_roots[i]) visitor(js, *g_roots[i]);
    
  if (!js) return;
  for (size_t i = 0; i < js->c_root_count; i++) {
    ant_value_t *slot = js->c_roots[i];
    if (slot && *slot) visitor(js, *slot);
  }

  for (gc_temp_root_scope_t *scope = js->temp_roots; scope; scope = scope->prev) 
    for (size_t i = 0; i < scope->len; i++) if (scope->items[i]) visitor(js, scope->items[i]);
}
