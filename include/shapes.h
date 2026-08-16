#ifndef ANT_SHAPES_H
#define ANT_SHAPES_H

#include "types.h"

#include <stdbool.h>
#include <stdint.h>

#ifndef ANT_INOBJ_MAX_SLOTS
#define ANT_INOBJ_MAX_SLOTS 4u
#endif

// TODO: constexpr
#define ANT_PROP_ATTR_WRITABLE     (1u << 0)
#define ANT_PROP_ATTR_ENUMERABLE   (1u << 1)
#define ANT_PROP_ATTR_CONFIGURABLE (1u << 2)
#define ANT_PROP_ATTR_DEFAULT      (ANT_PROP_ATTR_WRITABLE | ANT_PROP_ATTR_ENUMERABLE | ANT_PROP_ATTR_CONFIGURABLE)

typedef enum {
  ANT_SHAPE_KEY_STRING = 0,
  ANT_SHAPE_KEY_SYMBOL = 1,
  ANT_SHAPE_KEY_DELETED = 2,
} ant_shape_key_type_t;

typedef struct {
  ant_shape_key_type_t type;
  union {
    const char *interned;
    ant_offset_t sym_off;
  } key;
  uint8_t attrs;
  uint8_t has_getter;
  uint8_t has_setter;
  ant_value_t getter;
  ant_value_t setter;
} ant_shape_prop_t;

ant_shape_t *ant_shape_new(void);
ant_shape_t *ant_shape_new_with_inobj_limit(uint8_t inobj_limit);
ant_shape_t *ant_shape_clone(const ant_shape_t *shape);

void ant_shape_retain(ant_shape_t *shape);
void ant_shape_release(ant_shape_t *shape);
void ant_shape_guard_absence(ant_shape_t *shape);

uint8_t ant_shape_get_inobj_limit(const ant_shape_t *shape);
int32_t ant_shape_lookup_interned(const ant_shape_t *shape, const char *interned);
int32_t ant_shape_lookup_symbol(const ant_shape_t *shape, ant_offset_t sym_off);

bool ant_shape_add_interned(ant_shape_t *shape, const char *interned, uint8_t attrs, uint32_t *out_slot);
bool ant_shape_add_symbol(ant_shape_t *shape, ant_offset_t sym_off, uint8_t attrs, uint32_t *out_slot);

bool ant_shape_add_interned_tr(ant_shape_t **shape_pp, const char *interned, uint8_t attrs, uint32_t *out_slot);
bool ant_shape_add_symbol_tr(ant_shape_t **shape_pp, ant_offset_t sym_off, uint8_t attrs, uint32_t *out_slot);

// Marks a slot deleted without moving later properties. Slot order is property order,
// and the spec requires string keys in insertion order. Reusing or swapping the hole
// would make a re-added property appear in its old position. Deleted slots are compacted
// stably once enough accumulate, so live property order is preserved.
//
// NOTE: deletion invalidates the removed property's ant_prop_loc_t, and compaction
// invalidates locations for moved properties. Never hold a location across a setter,
// proxy trap, finalizer or other user callable; resolve it again afterwards. Every
// caller here does.
bool ant_shape_remove_slot(ant_shape_t *shape, uint32_t slot);
bool ant_shape_should_compact(const ant_shape_t *shape);
bool ant_shape_is_shared(const ant_shape_t *shape);

uint32_t ant_shape_count(const ant_shape_t *shape);
uint32_t ant_shape_compact(ant_shape_t *shape);
uint8_t ant_shape_get_attrs(const ant_shape_t *shape, uint32_t slot);

const ant_shape_prop_t *ant_shape_prop_at(const ant_shape_t *shape, uint32_t slot);
ant_shape_prop_t *ant_shape_prop_mut_at(ant_shape_t *shape, uint32_t slot);

bool ant_shape_set_attrs_interned(ant_shape_t *shape, const char *interned, uint8_t attrs);
bool ant_shape_set_attrs_symbol(ant_shape_t *shape, ant_offset_t sym_off, uint8_t attrs);
bool ant_shape_clear_accessor_slot(ant_shape_t *shape, uint32_t slot);

void ant_gc_shapes_begin(void);
void ant_gc_shapes_mark(ant_shape_t *shape);
bool ant_gc_shapes_sweep(void);

size_t ant_shape_total_bytes(void);
extern uint32_t ant_ic_epoch_counter;
extern uint32_t ant_ic_obj_epoch_counter;

static inline void ant_ic_epoch_bump(void) {
  ant_ic_epoch_counter++;
  if (ant_ic_epoch_counter == 0) ant_ic_epoch_counter = 1;
}

static inline void ant_ic_obj_epoch_bump(void) {
  ant_ic_obj_epoch_counter++;
  if (ant_ic_obj_epoch_counter == 0) ant_ic_obj_epoch_counter = 1;
}

#endif
