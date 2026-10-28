#ifndef ANT_PROTECTORS_H
#define ANT_PROTECTORS_H

#include "ant.h"
#include "value.h"
#include "object.h"
#include "isolate.h"

void ant_protectors_invalidate(ant_t *js, ant_object_t *holder, const char *key);
void ant_symbol_protectors_invalidate(ant_t *js, ant_object_t *holder, ant_offset_t sym_off);
void ant_regexp_protectors_invalidate(ant_t *js, ant_object_t *holder, bool species);

typedef enum: uint8_t {
#define ANT_PROTECTOR(name) ANT_PROTECTOR_BIT_##name,
#include "protectors/list.h"
  ANT_PROTECTOR_COUNT
} ant_protector_bit_t;

typedef enum: uint32_t {
#define ANT_PROTECTOR(name) ANT_PROTECTOR_##name = 1u << ANT_PROTECTOR_BIT_##name,
#include "protectors/list.h"
} ant_protector_t;

static_assert(
  ANT_PROTECTOR_COUNT <= 32, 
  "protectors_invalid holds at most 32 protectors"
);

static inline bool ant_protector_intact(const ant_t *js, uint32_t protectors) {
  return (js->protectors_invalid & protectors) == 0;
}

static inline void ant_protector_break(ant_t *js, uint32_t protectors) {
  js->protectors_invalid |= protectors;
}

static inline void ant_protector_rearm(ant_t *js, uint32_t protectors) {
  js->protectors_invalid &= ~protectors;
}

static inline void ant_prototype_write_epoch_bump(ant_t *js) {
  if (++js->prototype_write_epoch != 0) return;
  ant_ic_epoch_bump(js);
  js->prototype_write_epoch = 1;
}

static inline bool ant_property_key_is_protector(ant_t *js, const char *key) {
  return 
    key == js->intern.promise     || 
    key == js->intern.resolve     ||
    key == js->intern.constructor || 
    key == js->intern.then;
}

static inline bool ant_property_key_is_watched(ant_t *js, const char *key) {
  return 
    ant_property_key_is_protector(js, key) || 
    key == js->intern.prototype ||
    key == js->intern.exec      ||
    key == js->intern.replace;
}

static inline void ant_protector_holder_mark(ant_value_t value) {
  ant_object_t *obj = js_obj_ptr(value);
  if (obj) obj->protector_holder |= ANT_PROTECTOR_HOLDER;
}

static inline void ant_iterator_protector_holder_mark(ant_value_t value) {
  ant_object_t *obj = js_obj_ptr(value);
  if (obj) obj->protector_holder |= ANT_PROTECTOR_HOLDER | ANT_PROTECTOR_HOLDER_ITERATOR;
}

static inline void ant_regexp_protector_holder_mark(ant_value_t value) {
  ant_object_t *obj = js_obj_ptr(value);
  if (obj) obj->protector_holder |= ANT_PROTECTOR_HOLDER_REGEXP;
}

static inline void ant_property_mutation_invalidate(ant_t *js, ant_object_t *holder, const char *key) {
  if (!js || !holder || !key) return;
  ant_proto_watch_note_change(holder);
  
  if (__builtin_expect(holder->protector_holder & ANT_PROTECTOR_HOLDER_REGEXP, 0))
    ant_regexp_protectors_invalidate(js, holder, false);

  if (key == js->intern.prototype) {
    ant_prototype_write_epoch_bump(js);
    return;
  }

  if (!holder->protector_holder && holder->type_tag != kTypeArray && !ant_object_promise_state(holder)) return;
  if (!ant_property_key_is_protector(js, key) && key != js->intern.next && key != js->intern.ret) return;
  
  ant_protectors_invalidate(js, holder, key);
}

static inline void ant_symbol_property_mutation_invalidate(ant_t *js, ant_object_t *holder, ant_offset_t sym_off) {
  ant_proto_watch_note_change(holder);
  
  if (__builtin_expect(holder->protector_holder & ANT_PROTECTOR_HOLDER_REGEXP, 0))
    ant_regexp_protectors_invalidate(js, holder, sym_off == (ant_offset_t)vdata(js->sym.species_sym));
  
  if (!holder->protector_holder && holder->type_tag != kTypeArray) return;
  
  if (
    sym_off != (ant_offset_t)vdata(js->sym.iterator_sym) &&
    sym_off != (ant_offset_t)vdata(js->sym.asyncIterator_sym) &&
    sym_off != (ant_offset_t)vdata(js->sym.species_sym) &&
    sym_off != (ant_offset_t)vdata(js->sym.isConcatSpreadable_sym)
  ) return;
  
  ant_symbol_protectors_invalidate(js, holder, sym_off);
}

#endif
