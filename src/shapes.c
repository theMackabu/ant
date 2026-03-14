#include "shapes.h"

#include <stdlib.h>
#include <string.h>
#include <uthash.h>

typedef struct shape_index_entry {
  uint64_t key;
  uint32_t slot;
  UT_hash_handle hh;
} shape_index_entry_t;

struct ant_shape {
  uint32_t ref_count;
  uint32_t count;
  uint32_t cap;
  ant_shape_prop_t *props;
  shape_index_entry_t *index;
};

static uint64_t shape_key_interned(const char *interned) {
  return ((uint64_t)(uintptr_t)interned << 1);
}

static uint64_t shape_key_symbol(ant_offset_t sym_off) {
  return ((uint64_t)sym_off << 1) | 1u;
}

static shape_index_entry_t *shape_lookup(const ant_shape_t *shape, uint64_t key) {
  if (!shape) return NULL;
  shape_index_entry_t *entry = NULL;
  HASH_FIND(hh, shape->index, &key, sizeof(key), entry);
  return entry;
}

static bool shape_rebuild_index(ant_shape_t *shape) {
  if (!shape) return false;

  shape_index_entry_t *entry, *tmp;
  HASH_ITER(hh, shape->index, entry, tmp) {
    HASH_DEL(shape->index, entry);
    free(entry);
  }
  shape->index = NULL;

  for (uint32_t i = 0; i < shape->count; i++) {
    const ant_shape_prop_t *prop = &shape->props[i];
    uint64_t key = (prop->type == ANT_SHAPE_KEY_SYMBOL)
      ? shape_key_symbol(prop->key.sym_off)
      : shape_key_interned(prop->key.interned);

    shape_index_entry_t *idx = calloc(1, sizeof(*idx));
    if (!idx) return false;
    idx->key = key;
    idx->slot = i;
    HASH_ADD(hh, shape->index, key, sizeof(key), idx);
  }

  return true;
}

static bool shape_ensure_capacity(ant_shape_t *shape, uint32_t needed) {
  if (!shape) return false;
  if (needed <= shape->cap) return true;

  uint32_t new_cap = shape->cap ? shape->cap * 2 : 8;
  while (new_cap < needed) new_cap *= 2;

  ant_shape_prop_t *next = realloc(shape->props, sizeof(*next) * new_cap);
  if (!next) return false;

  shape->props = next;
  shape->cap = new_cap;
  return true;
}

static bool shape_add_key(
  ant_shape_t *shape,
  ant_shape_key_type_t type,
  const char *interned,
  ant_offset_t sym_off,
  uint8_t attrs,
  uint32_t *out_slot
) {
  if (!shape) return false;
  uint64_t key = (type == ANT_SHAPE_KEY_SYMBOL)
    ? shape_key_symbol(sym_off)
    : shape_key_interned(interned);

  shape_index_entry_t *found = shape_lookup(shape, key);
  if (found) {
    shape->props[found->slot].attrs = attrs;
    if (out_slot) *out_slot = found->slot;
    return true;
  }

  if (!shape_ensure_capacity(shape, shape->count + 1)) return false;

  uint32_t slot = shape->count++;
  ant_shape_prop_t *prop = &shape->props[slot];
  prop->type = type;
  prop->attrs = attrs;
  if (type == ANT_SHAPE_KEY_SYMBOL) prop->key.sym_off = sym_off;
  else prop->key.interned = interned;

  shape_index_entry_t *idx = calloc(1, sizeof(*idx));
  if (!idx) return false;
  idx->key = key;
  idx->slot = slot;
  HASH_ADD(hh, shape->index, key, sizeof(key), idx);

  if (out_slot) *out_slot = slot;
  return true;
}

ant_shape_t *ant_shape_new(void) {
  ant_shape_t *shape = calloc(1, sizeof(*shape));
  if (!shape) return NULL;
  shape->ref_count = 1;
  return shape;
}

void ant_shape_retain(ant_shape_t *shape) {
  if (!shape) return;
  shape->ref_count++;
}

void ant_shape_release(ant_shape_t *shape) {
  if (!shape) return;
  if (shape->ref_count == 0 || --shape->ref_count != 0) return;

  shape_index_entry_t *entry, *tmp;
  HASH_ITER(hh, shape->index, entry, tmp) {
    HASH_DEL(shape->index, entry);
    free(entry);
  }

  free(shape->props);
  free(shape);
}

int32_t ant_shape_lookup_interned(const ant_shape_t *shape, const char *interned) {
  shape_index_entry_t *entry = shape_lookup(shape, shape_key_interned(interned));
  return entry ? (int32_t)entry->slot : -1;
}

int32_t ant_shape_lookup_symbol(const ant_shape_t *shape, ant_offset_t sym_off) {
  shape_index_entry_t *entry = shape_lookup(shape, shape_key_symbol(sym_off));
  return entry ? (int32_t)entry->slot : -1;
}

bool ant_shape_add_interned(ant_shape_t *shape, const char *interned, uint8_t attrs, uint32_t *out_slot) {
  return shape_add_key(shape, ANT_SHAPE_KEY_STRING, interned, 0, attrs, out_slot);
}

bool ant_shape_add_symbol(ant_shape_t *shape, ant_offset_t sym_off, uint8_t attrs, uint32_t *out_slot) {
  return shape_add_key(shape, ANT_SHAPE_KEY_SYMBOL, NULL, sym_off, attrs, out_slot);
}

bool ant_shape_remove_slot(ant_shape_t *shape, uint32_t slot) {
  if (!shape || slot >= shape->count) return false;
  if (slot + 1 < shape->count) {
    memmove(
      &shape->props[slot],
      &shape->props[slot + 1],
      sizeof(*shape->props) * (shape->count - slot - 1)
    );
  }
  shape->count--;
  return shape_rebuild_index(shape);
}

uint32_t ant_shape_count(const ant_shape_t *shape) {
  return shape ? shape->count : 0;
}

const ant_shape_prop_t *ant_shape_prop_at(const ant_shape_t *shape, uint32_t slot) {
  if (!shape || slot >= shape->count) return NULL;
  return &shape->props[slot];
}

bool ant_shape_set_attrs_interned(ant_shape_t *shape, const char *interned, uint8_t attrs) {
  int32_t slot = ant_shape_lookup_interned(shape, interned);
  if (slot < 0) return false;
  shape->props[(uint32_t)slot].attrs = attrs;
  return true;
}

bool ant_shape_set_attrs_symbol(ant_shape_t *shape, ant_offset_t sym_off, uint8_t attrs) {
  int32_t slot = ant_shape_lookup_symbol(shape, sym_off);
  if (slot < 0) return false;
  shape->props[(uint32_t)slot].attrs = attrs;
  return true;
}

uint8_t ant_shape_get_attrs(const ant_shape_t *shape, uint32_t slot) {
  const ant_shape_prop_t *prop = ant_shape_prop_at(shape, slot);
  return prop ? prop->attrs : ANT_PROP_ATTR_DEFAULT;
}
