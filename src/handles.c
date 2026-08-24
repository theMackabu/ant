#include "handles.h"
#include "cage.h"
#include "hash.h"
#include "value.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

enum {
  CFUNC_SLAB_SIZE = 64 * 1024,
  EXTERNAL_CHUNK_BITS = 10,
  EXTERNAL_CHUNK_SIZE = 1 << EXTERNAL_CHUNK_BITS,
  EXTERNAL_CHUNK_COUNT = 4096,
  REGISTRY_BUCKET_COUNT = 2048,
};

typedef struct ant_cfunc_entry {
  ant_cfunc_meta_t meta;
  const ant_cfunc_meta_t *source;

  struct ant_cfunc_entry *source_next;
  struct ant_cfunc_entry *entrypoint_next;
} ant_cfunc_entry_t;

typedef struct ant_cfunc_slab {
  struct ant_cfunc_slab *next;
  size_t used;
} ant_cfunc_slab_t;

typedef struct ant_external_entry {
  void *ptr;
  uint64_t handle;
  struct ant_external_entry *next;
} ant_external_entry_t;

static uint64_t external_count = 0;
typedef _Atomic(void *) ant_external_slot_t;
static ant_cfunc_slab_t *cfunc_slabs = NULL;

static _Atomic(ant_external_slot_t *) external_chunks[EXTERNAL_CHUNK_COUNT];
static ant_cfunc_entry_t *cfunc_source_buckets[REGISTRY_BUCKET_COUNT];
static ant_cfunc_entry_t *cfunc_entrypoint_buckets[REGISTRY_BUCKET_COUNT];
static ant_external_entry_t *external_buckets[REGISTRY_BUCKET_COUNT];

#ifdef _WIN32
static SRWLOCK handles_lock = SRWLOCK_INIT;
static void handles_lock_acquire(void) { AcquireSRWLockExclusive(&handles_lock); }
static void handles_lock_release(void) { ReleaseSRWLockExclusive(&handles_lock); }
#else
static pthread_mutex_t handles_lock = PTHREAD_MUTEX_INITIALIZER;
static void handles_lock_acquire(void) { pthread_mutex_lock(&handles_lock); }
static void handles_lock_release(void) { pthread_mutex_unlock(&handles_lock); }
#endif

static size_t pointer_hash(const void *ptr) {
  uintptr_t value = (uintptr_t)ptr;
  return (size_t)hash_key((const char *)&value, sizeof(value))
    & (REGISTRY_BUCKET_COUNT - 1u);
}

static size_t entrypoint_hash(ant_cfunc_t fn) {
  return (size_t)hash_key((const char *)&fn, sizeof(fn))
    & (REGISTRY_BUCKET_COUNT - 1u);
}

static ant_cfunc_entry_t *cfunc_alloc_locked(void) {
  size_t align = _Alignof(ant_cfunc_entry_t);
  size_t offset = cfunc_slabs
    ? (cfunc_slabs->used + align - 1u) & ~(align - 1u)
    : CFUNC_SLAB_SIZE;

  if (offset > CFUNC_SLAB_SIZE - sizeof(ant_cfunc_entry_t)) {
    ant_cfunc_slab_t *slab = ant_cage_alloc(
      CFUNC_SLAB_SIZE, _Alignof(max_align_t));
    if (!slab) return NULL;
    slab->next = cfunc_slabs;
    slab->used = sizeof(*slab);
    cfunc_slabs = slab;
    offset = (slab->used + align - 1u) & ~(align - 1u);
  }

  ant_cfunc_entry_t *entry = (ant_cfunc_entry_t *)(
    (unsigned char *)cfunc_slabs + offset);
  cfunc_slabs->used = offset + sizeof(*entry);
  return entry;
}

static bool external_publish(uint64_t handle, void *ptr) {
  uint64_t index = handle - 1;
  
  size_t chunk_index = (size_t)(index >> EXTERNAL_CHUNK_BITS);
  size_t slot_index = (size_t)(index & (EXTERNAL_CHUNK_SIZE - 1u));
  if (chunk_index >= EXTERNAL_CHUNK_COUNT) return false;

  ant_external_slot_t *chunk = atomic_load_explicit(
    &external_chunks[chunk_index], 
    memory_order_acquire
  );
  
  if (!chunk) {
    chunk = calloc(EXTERNAL_CHUNK_SIZE, sizeof(*chunk));
    if (!chunk) return false;
    atomic_store_explicit(&external_chunks[chunk_index], chunk, memory_order_release);
  }
  
  atomic_store_explicit(&chunk[slot_index], ptr, memory_order_release);
  return true;
}

static ant_cfunc_entry_t *cfunc_create_locked(const ant_cfunc_meta_t *meta) {
  ant_cfunc_entry_t *entry = cfunc_alloc_locked();
  if (!entry) return NULL;

  entry->meta = *meta;
  entry->source = NULL;
  entry->source_next = NULL;
  entry->entrypoint_next = NULL;
  
  return entry;
}

const ant_cfunc_meta_t *ant_cfunc_meta_intern(const ant_cfunc_meta_t *meta) {
  if (!meta) return NULL;
  size_t bucket = pointer_hash(meta);
  handles_lock_acquire();
  
  for (
    ant_cfunc_entry_t *entry = cfunc_source_buckets[bucket];
    entry;
    entry = entry->source_next
  ) if (entry->source == meta) {
    const ant_cfunc_meta_t *stored = &entry->meta;
    handles_lock_release();
    return stored;
  }

  ant_cfunc_entry_t *entry = cfunc_create_locked(meta);
  if (!entry) {
    handles_lock_release();
    return NULL;
  }
  
  entry->source = meta;
  entry->source_next = cfunc_source_buckets[bucket];
  cfunc_source_buckets[bucket] = entry;
  handles_lock_release();
  
  return &entry->meta;
}

const ant_cfunc_meta_t *ant_cfunc_meta_create(const ant_cfunc_meta_t *meta) {
  if (!meta) return NULL;
  handles_lock_acquire();
  ant_cfunc_entry_t *entry = cfunc_create_locked(meta);
  handles_lock_release();
  return entry ? &entry->meta : NULL;
}

const ant_cfunc_meta_t *ant_cfunc_meta_for_entrypoint(ant_cfunc_t fn) {
  if (!fn) return NULL;
  size_t bucket = entrypoint_hash(fn);
  handles_lock_acquire();
  
  for (
    ant_cfunc_entry_t *entry = cfunc_entrypoint_buckets[bucket];
    entry;
    entry = entry->entrypoint_next
  ) if (entry->meta.fn == fn) {
    const ant_cfunc_meta_t *stored = &entry->meta;
    handles_lock_release();
    return stored;
  }

  ant_cfunc_meta_t meta = {.fn = fn};
  ant_cfunc_entry_t *entry = cfunc_create_locked(&meta);
  
  if (!entry) {
    handles_lock_release();
    return NULL;
  }
  
  entry->entrypoint_next = cfunc_entrypoint_buckets[bucket];
  cfunc_entrypoint_buckets[bucket] = entry;
  handles_lock_release();
  
  return &entry->meta;
}

uint64_t ant_external_handle_intern(void *ptr) {
  if (!ptr) return 0;
  size_t bucket = pointer_hash(ptr);

  handles_lock_acquire();
  for (
    ant_external_entry_t *entry = external_buckets[bucket]; 
    entry; entry = entry->next
  ) if (entry->ptr == ptr) {
    uint64_t handle = entry->handle;
    handles_lock_release();
    return handle;
  }

  ant_external_entry_t *entry = malloc(sizeof(*entry));
  if (!entry || external_count == NANBOX_DATA_MASK) {
    free(entry);
    handles_lock_release();
    return 0;
  }

  entry->ptr = ptr;
  entry->handle = external_count + 1;
  if (!external_publish(entry->handle, ptr)) {
    free(entry);
    handles_lock_release();
    return 0;
  }

  external_count = entry->handle;
  entry->next = external_buckets[bucket];
  external_buckets[bucket] = entry;
  handles_lock_release();
  
  return entry->handle;
}

void *ant_external_handle_get(uint64_t handle) {
  if (!handle) return NULL;
  
  uint64_t index = handle - 1;
  size_t chunk_index = (size_t)(index >> EXTERNAL_CHUNK_BITS);
  size_t slot_index = (size_t)(index & (EXTERNAL_CHUNK_SIZE - 1u));
  if (chunk_index >= EXTERNAL_CHUNK_COUNT) return NULL;
  
  ant_external_slot_t *chunk = atomic_load_explicit(
    &external_chunks[chunk_index], 
    memory_order_acquire
  );
  
  return chunk ? atomic_load_explicit(&chunk[slot_index], memory_order_acquire) : NULL;
}
