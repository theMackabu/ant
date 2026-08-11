#include "shapes.h"
#include <stdlib.h>
#include <string.h>
#include <uthash.h>

uint32_t ant_ic_epoch_counter = 1;
uint32_t ant_ic_obj_epoch_counter = 1;

static size_t g_shape_bytes          = 0;
static size_t g_shape_entry_pool_len = 0;
static void  *g_shape_entry_pool     = NULL;

typedef struct shape_index_entry {
  uint64_t key;
  uint32_t slot;
  UT_hash_handle hh;
} shape_index_entry_t;

typedef struct shape_child_entry {
  uint64_t key;
  ant_shape_t *child;
  UT_hash_handle hh;
} shape_child_entry_t;

static constexpr size_t SHAPE_ENTRY_SIZE = sizeof(shape_index_entry_t);
static constexpr size_t SHAPE_ENTRY_POOL_MAX = 1024;
static constexpr uint32_t SHAPE_COMPACT_MIN_TOMBSTONES = 32;

static_assert(
  sizeof(shape_index_entry_t) == sizeof(shape_child_entry_t), 
  "entry pool requires index and child entries to be the same size"
);

static inline void *shape_entry_alloc(void) {
  if (g_shape_entry_pool) {
    void *p = g_shape_entry_pool;
    g_shape_entry_pool = *(void **)p;
    g_shape_entry_pool_len--;
    memset(p, 0, SHAPE_ENTRY_SIZE);
    return p;
  }
  return calloc(1, SHAPE_ENTRY_SIZE);
}

static inline void shape_entry_free(void *p) {
  if (!p) return;
  if (g_shape_entry_pool_len < SHAPE_ENTRY_POOL_MAX) {
    *(void **)p = g_shape_entry_pool;
    g_shape_entry_pool = p;
    g_shape_entry_pool_len++;
  } else free(p);
}

static inline void shape_entry_pool_trim(void) {
  size_t keep = g_shape_entry_pool_len / 2;
  while (g_shape_entry_pool_len > keep) {
    void *p = g_shape_entry_pool;
    g_shape_entry_pool = *(void **)p;
    g_shape_entry_pool_len--;
    free(p);
  }
}

struct ant_shape {
  uint32_t ref_count;
  uint32_t count;
  uint32_t cap;
  uint32_t deleted_count;
  uint8_t inobj_limit;
  uint16_t gc_mark;
  uint32_t absence_guard_epoch;
  
  ant_shape_prop_t *props;
  shape_index_entry_t *index;
  shape_child_entry_t *children;
  ant_shape_t *parent;
  
  uint64_t parent_key;
};

static inline void shape_invalidate_guarded_absence(ant_shape_t *shape) {
  if (!shape || shape->absence_guard_epoch == 0) return;
  if (shape->absence_guard_epoch != ant_ic_epoch_counter) {
    shape->absence_guard_epoch = 0;
    return;
  }
  shape->absence_guard_epoch = 0;
  ant_ic_epoch_bump();
}

static ant_shape_t *g_root_shapes[ANT_INOBJ_MAX_SLOTS + 1];
static inline uint8_t shape_clamp_inobj_limit(uint8_t limit) {
  return (limit > ANT_INOBJ_MAX_SLOTS) ? (uint8_t)ANT_INOBJ_MAX_SLOTS : limit;
}

static uint64_t shape_key_interned(const char *interned) {
  return ((uint64_t)(uintptr_t)interned << 1);
}

static uint64_t shape_key_symbol(ant_offset_t sym_off) {
  return ((uint64_t)sym_off << 1) | 1u;
}

static uint64_t shape_child_key(uint64_t prop_key, uint8_t attrs) {
  return prop_key ^ ((uint64_t)attrs << 56);
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
    shape_entry_free(entry);
  }
  shape->index = NULL;

  for (uint32_t i = 0; i < shape->count; i++) {
    const ant_shape_prop_t *prop = &shape->props[i];
    if (prop->type == ANT_SHAPE_KEY_DELETED) continue;
    
    uint64_t key = (prop->type == ANT_SHAPE_KEY_SYMBOL)
      ? shape_key_symbol(prop->key.sym_off)
      : shape_key_interned(prop->key.interned);

    shape_index_entry_t *idx = shape_entry_alloc();
    if (!idx) return false;
    
    g_shape_bytes += sizeof(*idx);
    idx->key = key; idx->slot = i;
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

  g_shape_bytes += (new_cap - shape->cap) * sizeof(*next);
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
    ant_ic_epoch_bump();
    if (out_slot) *out_slot = found->slot;
    return true;
  }

  if (!shape_ensure_capacity(shape, shape->count + 1)) return false;
  uint32_t slot = shape->count++;
  ant_shape_prop_t *prop = &shape->props[slot];
  
  prop->type = type;
  prop->attrs = attrs;
  prop->has_getter = 0;
  prop->has_setter = 0;
  prop->getter = 0;
  prop->setter = 0;
  
  if (type == ANT_SHAPE_KEY_SYMBOL) prop->key.sym_off = sym_off;
  else prop->key.interned = interned;

  shape_index_entry_t *idx = shape_entry_alloc();
  if (!idx) return false;
  
  g_shape_bytes += sizeof(*idx);
  idx->key = key; idx->slot = slot;
  HASH_ADD(hh, shape->index, key, sizeof(key), idx);

  if (type == ANT_SHAPE_KEY_STRING)
    shape_invalidate_guarded_absence(shape);

  if (out_slot) *out_slot = slot;
  return true;
}

static ant_shape_t *shape_find_child(ant_shape_t *shape, uint64_t ckey) {
  if (!shape || !shape->children) return NULL;
  shape_child_entry_t *entry = NULL;
  HASH_FIND(hh, shape->children, &ckey, sizeof(ckey), entry);
  return entry ? entry->child : NULL;
}

static void shape_record_child(ant_shape_t *parent, uint64_t ckey, ant_shape_t *child) {
  shape_child_entry_t *existing = NULL;
  HASH_FIND(hh, parent->children, &ckey, sizeof(ckey), existing);
  if (existing) return;

  shape_child_entry_t *entry = shape_entry_alloc();
  if (!entry) return;
  
  entry->key = ckey;
  entry->child = child;
  ant_shape_retain(child);
  HASH_ADD(hh, parent->children, key, sizeof(entry->key), entry);
  child->parent = parent;
  child->parent_key = ckey;
}


static inline bool shape_is_in_tree(const ant_shape_t *shape) {
  return shape && (shape->ref_count > 1 || shape->parent != NULL);
}

bool ant_shape_add_interned_tr(ant_shape_t **shape_pp, const char *interned, uint8_t attrs, uint32_t *out_slot) {
  ant_shape_t *shape = *shape_pp;
  
  if (!shape) return false;
  if (!shape_is_in_tree(shape)) {
    return shape_add_key(shape, ANT_SHAPE_KEY_STRING, interned, 0, attrs, out_slot);
  }

  uint64_t prop_key = shape_key_interned(interned);
  uint64_t ckey = shape_child_key(prop_key, attrs);
  ant_shape_t *child = shape_find_child(shape, ckey);

  if (child) {
    int32_t slot = ant_shape_lookup_interned(child, interned);
    if (slot >= 0) {
      if (out_slot) *out_slot = (uint32_t)slot;
      shape_invalidate_guarded_absence(shape);
      ant_shape_retain(child); ant_shape_release(shape);
      *shape_pp = child; return true;
    }
  }

  ant_shape_t *shared = ant_shape_clone(shape);
  if (!shared) return false;
  if (!shape_add_key(shared, ANT_SHAPE_KEY_STRING, interned, 0, attrs, out_slot)) {
    ant_shape_release(shared);
    return false;
  }
  
  shape_record_child(shape, ckey, shared);
  ant_shape_release(shared);

  ant_shape_t *next = ant_shape_clone(shared);
  if (!next) return false;

  shape_invalidate_guarded_absence(shape);
  ant_shape_release(shape);
  *shape_pp = next;
  
  return true;
}

bool ant_shape_add_symbol_tr(ant_shape_t **shape_pp, ant_offset_t sym_off, uint8_t attrs, uint32_t *out_slot) {
  ant_shape_t *shape = *shape_pp;
  
  if (!shape) return false;
  if (!shape_is_in_tree(shape)) {
    return shape_add_key(shape, ANT_SHAPE_KEY_SYMBOL, NULL, sym_off, attrs, out_slot);
  }

  uint64_t prop_key = shape_key_symbol(sym_off);
  uint64_t ckey = shape_child_key(prop_key, attrs);
  ant_shape_t *child = shape_find_child(shape, ckey);

  if (child) {
    int32_t slot = ant_shape_lookup_symbol(child, sym_off);
    if (slot >= 0) {
      if (out_slot) *out_slot = (uint32_t)slot;
      ant_shape_retain(child); ant_shape_release(shape);
      *shape_pp = child; return true;
    }
  }

  ant_shape_t *shared = ant_shape_clone(shape);
  if (!shared) return false;
  if (!shape_add_key(shared, ANT_SHAPE_KEY_SYMBOL, NULL, sym_off, attrs, out_slot)) {
    ant_shape_release(shared);
    return false;
  }
  
  shape_record_child(shape, ckey, shared);
  ant_shape_release(shared);

  ant_shape_t *next = ant_shape_clone(shared);
  if (!next) return false;
  
  ant_shape_release(shape);
  *shape_pp = next;
  
  return true;
}

ant_shape_t *ant_shape_new_with_inobj_limit(uint8_t inobj_limit) {
  uint8_t clamped = shape_clamp_inobj_limit(inobj_limit);

  if (!g_root_shapes[clamped]) {
    ant_shape_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    g_shape_bytes += sizeof(*s);
    s->inobj_limit = clamped;
    s->ref_count = 1;
    g_root_shapes[clamped] = s;
  }

  ant_shape_retain(g_root_shapes[clamped]);
  return g_root_shapes[clamped];
}

ant_shape_t *ant_shape_new(void) {
  return ant_shape_new_with_inobj_limit((uint8_t)ANT_INOBJ_MAX_SLOTS);
}

ant_shape_t *ant_shape_clone(const ant_shape_t *shape) {
  if (!shape) return NULL;

  ant_shape_t *copy = calloc(1, sizeof(*copy));
  if (!copy) return NULL;
  
  g_shape_bytes += sizeof(*copy);
  copy->ref_count = 1;
  copy->deleted_count = shape->deleted_count;
  copy->inobj_limit = shape_clamp_inobj_limit(shape->inobj_limit);

  if (shape->count > 0) {
  if (!shape_ensure_capacity(copy, shape->count)) {
    ant_shape_release(copy);
    return NULL;
  }
  
  memcpy(copy->props, shape->props, sizeof(*shape->props) * shape->count);
  copy->count = shape->count;
  
  if (!shape_rebuild_index(copy)) {
    ant_shape_release(copy);
    return NULL;
  }}

  return copy;
}

bool ant_shape_is_shared(const ant_shape_t *shape) {
  return shape && shape->ref_count > 1;
}

void ant_shape_retain(ant_shape_t *shape) {
  if (!shape) return;
  shape->ref_count++;
}

void ant_shape_release(ant_shape_t *shape) {
  if (!shape) return;
  if (shape->ref_count == 0 || --shape->ref_count != 0) return;

  if (shape->parent) {
    shape_child_entry_t *entry = NULL;
    HASH_FIND(hh, shape->parent->children, &shape->parent_key, sizeof(shape->parent_key), entry);
    if (entry) {
      HASH_DEL(shape->parent->children, entry);
      shape_entry_free(entry);
    }
    shape->parent = NULL;
  }

  shape_child_entry_t *ce, *ctmp;
  HASH_ITER(hh, shape->children, ce, ctmp) {
    if (ce->child) {
      ce->child->parent = NULL;
      ant_shape_release(ce->child);
    }
    HASH_DEL(shape->children, ce);
    shape_entry_free(ce);
  }

  shape_index_entry_t *entry, *tmp;
  HASH_ITER(hh, shape->index, entry, tmp) {
    HASH_DEL(shape->index, entry);
    g_shape_bytes -= sizeof(*entry);
    shape_entry_free(entry);
  }

  g_shape_bytes -= shape->cap * sizeof(*shape->props);
  g_shape_bytes -= sizeof(*shape);
  
  free(shape->props);
  free(shape);
}

size_t ant_shape_total_bytes(void) { return g_shape_bytes; }
static uint16_t gc_shape_epoch = 0;

void ant_gc_shapes_begin(void) {
  gc_shape_epoch++;
  if (gc_shape_epoch == 0) gc_shape_epoch = 1;
}

void ant_gc_shapes_mark(ant_shape_t *shape) {
for (; shape; shape = shape->parent) {
  if (shape->gc_mark == gc_shape_epoch) break;
  shape->gc_mark = gc_shape_epoch;
}}

static bool shape_prune_dead_children(ant_shape_t *shape) {
  bool freed_any = false;
  shape_child_entry_t *ce, *ctmp;
  
  HASH_ITER(hh, shape->children, ce, ctmp) {
  if (shape_prune_dead_children(ce->child)) freed_any = true;
  if (ce->child->gc_mark != gc_shape_epoch && !ce->child->children) {
    ant_shape_t *child = ce->child;
    child->parent = NULL;
    HASH_DEL(shape->children, ce);
    shape_entry_free(ce);
    ant_shape_release(child);
    freed_any = true;
  }}
  
  return freed_any;
}

bool ant_gc_shapes_sweep(void) {
  bool freed_any = false;
  for (int i = 0; i <= (int)ANT_INOBJ_MAX_SLOTS; i++) {
    if (g_root_shapes[i] && shape_prune_dead_children(g_root_shapes[i])) freed_any = true;
  }
  shape_entry_pool_trim();
  return freed_any;
}

uint8_t ant_shape_get_inobj_limit(const ant_shape_t *shape) {
  if (!shape) return (uint8_t)ANT_INOBJ_MAX_SLOTS;
  return shape_clamp_inobj_limit(shape->inobj_limit);
}

int32_t ant_shape_lookup_interned(const ant_shape_t *shape, const char *interned) {
  shape_index_entry_t *entry = shape_lookup(shape, shape_key_interned(interned));
  return entry ? (int32_t)entry->slot : -1;
}

int32_t ant_shape_lookup_symbol(const ant_shape_t *shape, ant_offset_t sym_off) {
  shape_index_entry_t *entry = shape_lookup(shape, shape_key_symbol(sym_off));
  return entry ? (int32_t)entry->slot : -1;
}

void ant_shape_guard_absence(ant_shape_t *shape) {
  if (shape) shape->absence_guard_epoch = ant_ic_epoch_counter;
}

bool ant_shape_add_interned(ant_shape_t *shape, const char *interned, uint8_t attrs, uint32_t *out_slot) {
  return shape_add_key(shape, ANT_SHAPE_KEY_STRING, interned, 0, attrs, out_slot);
}

bool ant_shape_add_symbol(ant_shape_t *shape, ant_offset_t sym_off, uint8_t attrs, uint32_t *out_slot) {
  return shape_add_key(shape, ANT_SHAPE_KEY_SYMBOL, NULL, sym_off, attrs, out_slot);
}

bool ant_shape_remove_slot(ant_shape_t *shape, uint32_t slot) {
  if (!shape || slot >= shape->count) return false;

  const ant_shape_prop_t *dp = &shape->props[slot];
  if (dp->type == ANT_SHAPE_KEY_DELETED) return false;
  uint64_t del_key = (dp->type == ANT_SHAPE_KEY_SYMBOL)
    ? shape_key_symbol(dp->key.sym_off)
    : shape_key_interned(dp->key.interned);

  shape_index_entry_t *del_entry = NULL;
  HASH_FIND(hh, shape->index, &del_key, sizeof(del_key), del_entry);
  if (del_entry) {
    HASH_DEL(shape->index, del_entry);
    shape_entry_free(del_entry);
  }

  memset(&shape->props[slot], 0, sizeof(shape->props[slot]));
  shape->props[slot].type = ANT_SHAPE_KEY_DELETED;
  shape->deleted_count++;

  while (
    shape->count > 0 &&
    shape->props[shape->count - 1].type == ANT_SHAPE_KEY_DELETED
  ) {
    shape->count--;
    shape->deleted_count--;
  }
  ant_ic_epoch_bump();

  return true;
}

uint32_t ant_shape_count(const ant_shape_t *shape) {
  return shape ? shape->count : 0;
}

bool ant_shape_should_compact(const ant_shape_t *shape) {
  if (!shape || shape->deleted_count < SHAPE_COMPACT_MIN_TOMBSTONES) return false;
  return shape->deleted_count >= shape->count - shape->deleted_count;
}

uint32_t ant_shape_compact(ant_shape_t *shape) {
  if (!shape || shape->deleted_count == 0) return shape ? shape->count : 0;

  uint32_t dst = 0;
  for (uint32_t src = 0; src < shape->count; src++) {
    ant_shape_prop_t *prop = &shape->props[src];
    if (prop->type == ANT_SHAPE_KEY_DELETED) continue;

    uint64_t key = (prop->type == ANT_SHAPE_KEY_SYMBOL)
      ? shape_key_symbol(prop->key.sym_off)
      : shape_key_interned(prop->key.interned);
    shape_index_entry_t *entry = NULL;
    HASH_FIND(hh, shape->index, &key, sizeof(key), entry);
    if (entry) entry->slot = dst;

    if (dst != src) shape->props[dst] = *prop;
    dst++;
  }

  if (dst < shape->count) {
    memset(
      &shape->props[dst],
      0,
      (shape->count - dst) * sizeof(*shape->props)
    );
  }
  shape->count = dst;
  shape->deleted_count = 0;
  return dst;
}

const ant_shape_prop_t *ant_shape_prop_at(const ant_shape_t *shape, uint32_t slot) {
  if (!shape || slot >= shape->count) return NULL;
  if (shape->props[slot].type == ANT_SHAPE_KEY_DELETED) return NULL;
  return &shape->props[slot];
}

ant_shape_prop_t *ant_shape_prop_mut_at(ant_shape_t *shape, uint32_t slot) {
  if (!shape || slot >= shape->count) return NULL;
  if (shape->props[slot].type == ANT_SHAPE_KEY_DELETED) return NULL;
  return &shape->props[slot];
}

bool ant_shape_set_attrs_interned(ant_shape_t *shape, const char *interned, uint8_t attrs) {
  int32_t slot = ant_shape_lookup_interned(shape, interned);
  if (slot < 0) return false;
  shape->props[(uint32_t)slot].attrs = attrs;
  ant_ic_epoch_bump();
  return true;
}

bool ant_shape_set_attrs_symbol(ant_shape_t *shape, ant_offset_t sym_off, uint8_t attrs) {
  int32_t slot = ant_shape_lookup_symbol(shape, sym_off);
  if (slot < 0) return false;
  shape->props[(uint32_t)slot].attrs = attrs;
  ant_ic_epoch_bump();
  return true;
}

uint8_t ant_shape_get_attrs(const ant_shape_t *shape, uint32_t slot) {
  const ant_shape_prop_t *prop = ant_shape_prop_at(shape, slot);
  return prop ? prop->attrs : ANT_PROP_ATTR_DEFAULT;
}

bool ant_shape_clear_accessor_slot(ant_shape_t *shape, uint32_t slot) {
  ant_shape_prop_t *prop = ant_shape_prop_mut_at(shape, slot);
  if (!prop) return false;
  prop->has_getter = 0;
  prop->has_setter = 0;
  prop->getter = 0;
  prop->setter = 0;
  ant_ic_epoch_bump();
  return true;
}
