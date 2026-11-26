#pragma once

#include "types.h"
#include "gc/weak.h"

struct ant_iterator_registry;
typedef void (*gc_mark_fn)(ant_t *js, ant_value_t v);

#define GC_MODULE_MARK(name) void gc_mark_##name(ant_t *js, gc_mark_fn mark);
#include "gc/module_hooks.h"

#define GC_MODULE_WEAK(name) void gc_clear_##name##_weak_refs(ant_t *js, gc_weak_key_alive_fn alive);
#include "gc/module_hooks.h"

#define GC_CORE_MARK(name) void gc_mark_##name(ant_t *js, gc_mark_fn mark);
#include "gc/module_hooks.h"

#define GC_NATIVE_MARK(name) void gc_mark_##name(ant_t *js, ant_value_t value, gc_mark_fn mark);
#include "gc/module_hooks.h"

#define GC_NATIVE_FINALIZE(name) void gc_finalize_##name(ant_t *js, ant_value_t value);
#include "gc/module_hooks.h"

void gc_mark_error_record(ant_t *js, ant_value_t value, gc_mark_fn mark);
void gc_mark_iterator_registry(ant_t *js, const struct ant_iterator_registry *registry, gc_mark_fn mark);
