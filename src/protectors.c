#include "protectors.h"
#include "realm.h"
#include "internal.h"
#include "silver/engine.h"

typedef struct {
  ant_value_t global;
  ant_value_t promise_ctor;
  ant_value_t promise_proto;
  ant_value_t array_ctor;
  ant_value_t array_proto;
  ant_value_t array_iterator_proto;
  
  bool *promise_resolve_lookup;
  bool *promise_constructor;
  bool *promise_species;
  bool *promise_then;
  bool *array_species;
  bool *array_iteration;
} protector_view_t;

#define PROTECTOR_VIEW(src) ((protector_view_t){                              \
  .global = (src)->global,                                                    \
  .promise_ctor = (src)->sym.promise_ctor,                                    \
  .promise_proto = (src)->sym.promise_proto,                                  \
  .array_ctor = (src)->sym.array_ctor,                                        \
  .array_proto = (src)->sym.array_proto,                                      \
  .array_iterator_proto = (src)->sym.array_iterator_proto,                    \
  .promise_resolve_lookup = &(src)->promise_resolve_lookup_protector_invalid, \
  .promise_constructor = &(src)->promise_constructor_protector_invalid,       \
  .promise_species = &(src)->promise_species_protector_invalid,               \
  .promise_then = &(src)->promise_then_protector_invalid,                     \
  .array_species = &(src)->array_species_protector_invalid,                   \
  .array_iteration = &(src)->array_iteration_protector_invalid,               \
})

static inline bool protector_holder_is(const ant_object_t *holder, ant_value_t value) {
  return is_object_type(value) && holder == js_obj_ptr(value);
}

static void protectors_apply_key(ant_t *js, const protector_view_t *v, ant_object_t *holder, const char *key) {
  if (key == js->intern.promise) {
    if (protector_holder_is(holder, v->global)) *v->promise_resolve_lookup = true;
    return;
  }

  if (key == js->intern.resolve) {
    if (protector_holder_is(holder, v->promise_ctor)) *v->promise_resolve_lookup = true;
    return;
  }

  if (key == js->intern.next) {
    if (protector_holder_is(holder, v->array_iterator_proto)) *v->array_iteration = true;
    return;
  }

  bool constructor = key == js->intern.constructor;
  if (constructor && (holder->type_tag == kTypeArray || protector_holder_is(holder, v->array_proto)))
    *v->array_species = true;

  if (!ant_object_promise_state(holder) && !protector_holder_is(holder, v->promise_proto)) return;

  if (constructor) {
    *v->promise_constructor = true;
    *v->promise_species = true;
  } else *v->promise_then = true;
}

static void protectors_apply_symbol(ant_t *js, const protector_view_t *v, ant_object_t *holder, ant_offset_t sym_off) {
  if (sym_off == (ant_offset_t)vdata(js->sym.iterator_sym)) {
    if (holder->type_tag == kTypeArray || protector_holder_is(holder, v->array_proto))
      *v->array_iteration = true;
    return;
  }

  if (protector_holder_is(holder, v->promise_ctor)) *v->promise_species = true;
  if (is_object_type(v->array_ctor) && holder == js_obj_ptr(js_func_obj(v->array_ctor)))
    *v->array_species = true;
}

void ant_protectors_invalidate(ant_t *js, ant_object_t *holder, const char *key) {
  protector_view_t view = PROTECTOR_VIEW(js);
  protectors_apply_key(js, &view, holder, key);
  if (!ant_realm_multiple(js) || js->realm->initializing) return;

  for (ant_realm_t *realm = js->realms; realm; realm = realm->next) {
    if (realm == js->realm) continue;
    view = PROTECTOR_VIEW(realm);
    protectors_apply_key(js, &view, holder, key);
  }
}

void ant_symbol_protectors_invalidate(ant_t *js, ant_object_t *holder, ant_offset_t sym_off) {
  protector_view_t view = PROTECTOR_VIEW(js);
  protectors_apply_symbol(js, &view, holder, sym_off);
  if (!ant_realm_multiple(js) || js->realm->initializing) return;

  for (ant_realm_t *realm = js->realms; realm; realm = realm->next) {
    if (realm == js->realm) continue;
    view = PROTECTOR_VIEW(realm);
    protectors_apply_symbol(js, &view, holder, sym_off);
  }
}
