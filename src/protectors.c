#include "protectors.h"
#include "realm.h"
#include "internal.h"
#include "silver/engine.h"

typedef struct {
  ant_value_t global;
  ant_value_t object_proto;
  ant_value_t promise_ctor;
  ant_value_t promise_proto;
  ant_value_t array_ctor;
  ant_value_t array_proto;
  ant_value_t array_iterator_proto;
  ant_value_t string_proto;
  ant_value_t string_iterator_proto;
  ant_value_t map_iter_proto;
  ant_value_t set_iter_proto;
  uint32_t *invalid;
} protector_view_t;

#define PROTECTOR_VIEW(src) ((protector_view_t){                              \
  .global = (src)->global,                                                    \
  .object_proto = (src)->sym.object_proto,                                    \
  .promise_ctor = (src)->sym.promise_ctor,                                    \
  .promise_proto = (src)->sym.promise_proto,                                  \
  .array_ctor = (src)->sym.array_ctor,                                        \
  .array_proto = (src)->sym.array_proto,                                      \
  .array_iterator_proto = (src)->sym.array_iterator_proto,                    \
  .string_proto = (src)->sym.string_proto,                                    \
  .string_iterator_proto = (src)->sym.string_iterator_proto,                  \
  .map_iter_proto = (src)->builtins.map_iter_proto,                           \
  .set_iter_proto = (src)->builtins.set_iter_proto,                           \
  .invalid = &(src)->protectors_invalid,                                      \
})

static inline bool protector_holder_is(const ant_object_t *holder, ant_value_t value) {
  return is_object_type(value) && holder == js_obj_ptr(value);
}

static void protectors_apply_key(ant_t *js, const protector_view_t *v, ant_object_t *holder, const char *key) {
  if (key == js->intern.promise) {
    if (protector_holder_is(holder, v->global)) *v->invalid |= ANT_PROTECTOR_PROMISE_RESOLVE_LOOKUP;
    return;
  }

  if (key == js->intern.resolve) {
    if (protector_holder_is(holder, v->promise_ctor)) *v->invalid |= ANT_PROTECTOR_PROMISE_RESOLVE_LOOKUP;
    return;
  }

  if (key == js->intern.next) {
    if (protector_holder_is(holder, v->array_iterator_proto)) *v->invalid |= ANT_PROTECTOR_ARRAY_ITERATION;
    if (protector_holder_is(holder, v->string_iterator_proto)) *v->invalid |= ANT_PROTECTOR_STRING_ITERATION;
    if (protector_holder_is(holder, v->map_iter_proto) || protector_holder_is(holder, v->set_iter_proto))
      *v->invalid |= ANT_PROTECTOR_COLLECTION_ITERATION;
    return;
  }

  if (key == js->intern.ret) {
    if (holder->protector_holder & ANT_PROTECTOR_HOLDER_ITERATOR) *v->invalid |= ANT_PROTECTOR_ITERATOR_RETURN;
    return;
  }

  if (key == js->intern.then && protector_holder_is(holder, v->object_proto)) {
    *v->invalid |= ANT_PROTECTOR_OBJECT_THEN;
    return;
  }

  bool constructor = key == js->intern.constructor;
  
  if (
    constructor && (holder->type_tag == kTypeArray || 
    protector_holder_is(holder, v->array_proto))
  ) *v->invalid |= ANT_PROTECTOR_ARRAY_SPECIES;

  if (!ant_object_promise_state(holder) && !protector_holder_is(holder, v->promise_proto)) return;

  *v->invalid |= constructor
    ? ANT_PROTECTOR_PROMISE_CONSTRUCTOR 
    | ANT_PROTECTOR_PROMISE_SPECIES
    : ANT_PROTECTOR_PROMISE_THEN;
}

static void protectors_apply_symbol(ant_t *js, const protector_view_t *v, ant_object_t *holder, ant_offset_t sym_off) {
  if (sym_off == (ant_offset_t)vdata(js->sym.iterator_sym)) {
    if (holder->type_tag == kTypeArray || protector_holder_is(holder, v->array_proto))
      *v->invalid |= ANT_PROTECTOR_ARRAY_ITERATION;
    if (protector_holder_is(holder, v->string_proto)) *v->invalid |= ANT_PROTECTOR_STRING_ITERATION;
    return;
  }

  if (sym_off == (ant_offset_t)vdata(js->sym.isConcatSpreadable_sym)) {
    if (
      holder->type_tag == kTypeArray || 
      protector_holder_is(holder, v->array_proto) || 
      protector_holder_is(holder, v->object_proto)
    ) *v->invalid |= ANT_PROTECTOR_CONCAT_SPREADABLE;
    return;
  }

  if (sym_off == (ant_offset_t)vdata(js->sym.asyncIterator_sym)) {
    if (
      holder->type_tag == kTypeArray ||
      protector_holder_is(holder, v->array_proto) ||
      protector_holder_is(holder, v->object_proto)
    ) *v->invalid |= ANT_PROTECTOR_ARRAY_ASYNC_ITERATION;
    return;
  }

  if (protector_holder_is(holder, v->promise_ctor)) *v->invalid |= ANT_PROTECTOR_PROMISE_SPECIES;
  if (is_object_type(v->array_ctor) && holder == js_obj_ptr(js_func_obj(v->array_ctor))) *v->invalid |= ANT_PROTECTOR_ARRAY_SPECIES;
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

void ant_protector_break_all_realms(ant_t *js, uint32_t protectors) {
  js->protectors_invalid |= protectors;
  for (ant_realm_t *realm = js->realms; realm; realm = realm->next) realm->protectors_invalid |= protectors;
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

static void regexp_protector_apply(ant_regexp_realm_t *regexp, ant_object_t *holder, bool species) {
  if (holder == regexp->proto_ptr || (species && holder == regexp->ctor_ptr)) regexp->protector_invalid = true;
}

void ant_regexp_protectors_invalidate(ant_t *js, ant_object_t *holder, bool species) {
  regexp_protector_apply(&js->regexp, holder, species);
  if (!ant_realm_multiple(js) || js->realm->initializing) return;

  for (ant_realm_t *realm = js->realms; realm; realm = realm->next)
    if (realm != js->realm) regexp_protector_apply(&realm->regexp, holder, species);
}
