#include "handles.h"
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
  HANDLE_CHUNK_BITS = 10,
  HANDLE_CHUNK_SIZE = 1 << HANDLE_CHUNK_BITS,
  HANDLE_CHUNK_COUNT = 4096,
  HANDLE_BUCKET_COUNT = 2048,
};

typedef struct ant_cfunc_entry {
  ant_cfunc_meta_t meta;
  const ant_cfunc_meta_t *source;
  uint64_t handle;
  
  struct ant_cfunc_entry *source_next;
  struct ant_cfunc_entry *entrypoint_next;
} ant_cfunc_entry_t;

typedef struct ant_external_entry {
  void *ptr;
  uint64_t handle;
  struct ant_external_entry *next;
} ant_external_entry_t;

typedef _Atomic(const ant_cfunc_meta_t *) ant_cfunc_slot_t;
typedef _Atomic(void *) ant_external_slot_t;

static _Atomic(ant_cfunc_slot_t *) cfunc_chunks[HANDLE_CHUNK_COUNT];
static _Atomic(ant_external_slot_t *) external_chunks[HANDLE_CHUNK_COUNT];
static ant_cfunc_entry_t *cfunc_source_buckets[HANDLE_BUCKET_COUNT];
static ant_cfunc_entry_t *cfunc_entrypoint_buckets[HANDLE_BUCKET_COUNT];
static ant_external_entry_t *external_buckets[HANDLE_BUCKET_COUNT];

static uint64_t cfunc_count = 0;
static uint64_t external_count = 0;

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
  value ^= value >> 33;
  value *= UINT64_C(0xff51afd7ed558ccd);
  value ^= value >> 33;
  return (size_t)value & (HANDLE_BUCKET_COUNT - 1u);
}

static size_t entrypoint_hash(ant_cfunc_t fn) {
  const unsigned char *bytes = (const unsigned char *)&fn;
  uint64_t hash = UINT64_C(1469598103934665603);
  for (size_t i = 0; i < sizeof(fn); i++) {
    hash ^= bytes[i];
    hash *= UINT64_C(1099511628211);
  }
  return (size_t)hash & (HANDLE_BUCKET_COUNT - 1u);
}

static bool cfunc_publish(uint64_t handle, const ant_cfunc_meta_t *meta) {
  uint64_t index = handle - 1;
  size_t chunk_index = (size_t)(index >> HANDLE_CHUNK_BITS);
  size_t slot_index = (size_t)(index & (HANDLE_CHUNK_SIZE - 1u));
  if (chunk_index >= HANDLE_CHUNK_COUNT) return false;

  ant_cfunc_slot_t *chunk = atomic_load_explicit(
    &cfunc_chunks[chunk_index], memory_order_acquire
  );
  if (!chunk) {
    chunk = calloc(HANDLE_CHUNK_SIZE, sizeof(*chunk));
    if (!chunk) return false;
    atomic_store_explicit(&cfunc_chunks[chunk_index], chunk, memory_order_release);
  }
  atomic_store_explicit(&chunk[slot_index], meta, memory_order_release);
  return true;
}

static bool external_publish(uint64_t handle, void *ptr) {
  uint64_t index = handle - 1;
  size_t chunk_index = (size_t)(index >> HANDLE_CHUNK_BITS);
  size_t slot_index = (size_t)(index & (HANDLE_CHUNK_SIZE - 1u));
  if (chunk_index >= HANDLE_CHUNK_COUNT) return false;

  ant_external_slot_t *chunk = atomic_load_explicit(
    &external_chunks[chunk_index], memory_order_acquire
  );
  if (!chunk) {
    chunk = calloc(HANDLE_CHUNK_SIZE, sizeof(*chunk));
    if (!chunk) return false;
    atomic_store_explicit(&external_chunks[chunk_index], chunk, memory_order_release);
  }
  atomic_store_explicit(&chunk[slot_index], ptr, memory_order_release);
  return true;
}

static ant_cfunc_entry_t *cfunc_create_locked(const ant_cfunc_meta_t *meta) {
  ant_cfunc_entry_t *entry = malloc(sizeof(*entry));
  if (!entry || cfunc_count == NANBOX_DATA_MASK) {
    free(entry);
    return NULL;
  }

  entry->meta = *meta;
  entry->source = NULL;
  entry->source_next = NULL;
  entry->entrypoint_next = NULL;
  entry->handle = cfunc_count + 1;
  if (!cfunc_publish(entry->handle, &entry->meta)) {
    free(entry);
    return NULL;
  }

  cfunc_count = entry->handle;
  return entry;
}

uint64_t ant_cfunc_handle_intern(const ant_cfunc_meta_t *meta) {
  if (!meta) return 0;
  size_t bucket = pointer_hash(meta);

  handles_lock_acquire();
  for (
    ant_cfunc_entry_t *entry = cfunc_source_buckets[bucket];
    entry;
    entry = entry->source_next
  ) if (entry->source == meta) {
    uint64_t handle = entry->handle;
    handles_lock_release();
    return handle;
  }

  ant_cfunc_entry_t *entry = cfunc_create_locked(meta);
  if (!entry) {
    handles_lock_release();
    return 0;
  }
  entry->source = meta;
  entry->source_next = cfunc_source_buckets[bucket];
  cfunc_source_buckets[bucket] = entry;
  handles_lock_release();
  return entry->handle;
}

uint64_t ant_cfunc_handle_create(const ant_cfunc_meta_t *meta) {
  if (!meta) return 0;
  handles_lock_acquire();
  ant_cfunc_entry_t *entry = cfunc_create_locked(meta);
  handles_lock_release();
  return entry ? entry->handle : 0;
}

uint64_t ant_cfunc_handle_for_entrypoint(ant_cfunc_t fn) {
  if (!fn) return 0;
  size_t bucket = entrypoint_hash(fn);
  handles_lock_acquire();
  for (
    ant_cfunc_entry_t *entry = cfunc_entrypoint_buckets[bucket];
    entry;
    entry = entry->entrypoint_next
  ) if (entry->meta.fn == fn) {
    uint64_t handle = entry->handle;
    handles_lock_release();
    return handle;
  }

  ant_cfunc_meta_t meta = {.fn = fn};
  ant_cfunc_entry_t *entry = cfunc_create_locked(&meta);
  if (!entry) {
    handles_lock_release();
    return 0;
  }
  entry->entrypoint_next = cfunc_entrypoint_buckets[bucket];
  cfunc_entrypoint_buckets[bucket] = entry;
  handles_lock_release();
  return entry->handle;
}

const ant_cfunc_meta_t *ant_cfunc_handle_get(uint64_t handle) {
  if (!handle) return NULL;
  uint64_t index = handle - 1;
  size_t chunk_index = (size_t)(index >> HANDLE_CHUNK_BITS);
  size_t slot_index = (size_t)(index & (HANDLE_CHUNK_SIZE - 1u));
  if (chunk_index >= HANDLE_CHUNK_COUNT) return NULL;
  ant_cfunc_slot_t *chunk = atomic_load_explicit(
    &cfunc_chunks[chunk_index], memory_order_acquire
  );
  return chunk ? atomic_load_explicit(&chunk[slot_index], memory_order_acquire) : NULL;
}

uint64_t ant_external_handle_intern(void *ptr) {
  if (!ptr) return 0;
  size_t bucket = pointer_hash(ptr);

  handles_lock_acquire();
  for (ant_external_entry_t *entry = external_buckets[bucket]; entry; entry = entry->next) {
    if (entry->ptr == ptr) {
      uint64_t handle = entry->handle;
      handles_lock_release();
      return handle;
    }
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
  size_t chunk_index = (size_t)(index >> HANDLE_CHUNK_BITS);
  size_t slot_index = (size_t)(index & (HANDLE_CHUNK_SIZE - 1u));
  if (chunk_index >= HANDLE_CHUNK_COUNT) return NULL;
  ant_external_slot_t *chunk = atomic_load_explicit(
    &external_chunks[chunk_index], memory_order_acquire
  );
  return chunk ? atomic_load_explicit(&chunk[slot_index], memory_order_acquire) : NULL;
}
