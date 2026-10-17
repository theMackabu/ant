#ifndef ANT_PROTECTORS_H
#define ANT_PROTECTORS_H

#include "ant.h"
#include "value.h"
#include "object.h"
#include "isolate.h"

void ant_protectors_invalidate(ant_t *js, ant_object_t *holder, const char *key);
void ant_symbol_protectors_invalidate(ant_t *js, ant_object_t *holder, ant_offset_t sym_off);

static inline void ant_prototype_write_epoch_bump(ant_t *js) {
  if (++js->prototype_write_epoch == 0) {
    ant_ic_epoch_bump(js);
    js->prototype_write_epoch = 1;
  }
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
  if (obj) obj->protector_holder = 1;
}

static inline void ant_property_mutation_invalidate(ant_t *js, ant_object_t *holder, const char *key) {
  if (!js || !holder || !key) return;

  if (key == js->intern.prototype) {
    ant_prototype_write_epoch_bump(js);
    return;
  }

  if (!holder->protector_holder && holder->type_tag != kTypeArray && !ant_object_promise_state(holder)) return;
  if (!ant_property_key_is_protector(js, key) && key != js->intern.next) return;
  
  ant_protectors_invalidate(js, holder, key);
}

static inline void ant_symbol_property_mutation_invalidate(ant_t *js, ant_object_t *holder, ant_offset_t sym_off) {
  if (!holder->protector_holder && holder->type_tag != kTypeArray) return;
  
  if (
    sym_off != (ant_offset_t)vdata(js->sym.iterator_sym) &&
    sym_off != (ant_offset_t)vdata(js->sym.species_sym)
  ) return;
  
  ant_symbol_protectors_invalidate(js, holder, sym_off);
}

#endif
