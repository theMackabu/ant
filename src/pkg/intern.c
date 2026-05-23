#include "intern.h"
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY 64
#define LOAD_FACTOR 0.70

static uint32_t djb2_hash(const char *str, size_t len) {
  uint32_t hash = 5381;
  for (size_t i = 0; i < len; i++) {
    hash = ((hash << 5) + hash) + (uint8_t)str[i];
  }
  return hash;
}

void string_pool_init(string_pool_t *pool) {
  pool->entries = NULL;
  pool->capacity = 0;
  pool->size = 0;
  pool->storage = NULL;
  pool->storage_capacity = 0;
  pool->storage_size = 0;
}

void string_pool_deinit(string_pool_t *pool) {
  if (pool->entries) {
    free(pool->entries);
  }
  for (size_t i = 0; i < pool->storage_size; i++) {
    free(pool->storage[i]);
  }
  if (pool->storage) {
    free(pool->storage);
  }
  memset(pool, 0, sizeof(*pool));
}

static bool string_pool_resize(string_pool_t *pool, size_t new_capacity) {
  string_pool_entry_t *new_entries =
      calloc(new_capacity, sizeof(string_pool_entry_t));
  if (!new_entries) {
    return false;
  }

  for (size_t i = 0; i < pool->capacity; i++) {
    string_pool_entry_t *entry = &pool->entries[i];
    if (entry->key.ptr == NULL) {
      continue;
    }

    uint32_t hash = djb2_hash(entry->key.ptr, entry->key.len);
    size_t index = hash % new_capacity;
    while (new_entries[index].key.ptr != NULL) {
      index = (index + 1) % new_capacity;
    }
    new_entries[index] = *entry;
  }

  free(pool->entries);
  pool->entries = new_entries;
  pool->capacity = new_capacity;
  return true;
}

static interned_string_t
string_pool_insert_helper(string_pool_t *pool, const char *ptr, size_t len) {
  if (pool->capacity == 0) {
    if (!string_pool_resize(pool, INITIAL_CAPACITY)) {
      return interned_string_empty();
    }
  } else if ((double)pool->size >= (double)pool->capacity * LOAD_FACTOR) {
    if (!string_pool_resize(pool, pool->capacity * 2)) {
      return interned_string_empty();
    }
  }

  uint32_t hash = djb2_hash(ptr, len);
  size_t index = hash % pool->capacity;

  while (pool->entries[index].key.ptr != NULL) {
    if (pool->entries[index].key.len == len &&
        memcmp(pool->entries[index].key.ptr, ptr, len) == 0) {
      return pool->entries[index].value;
    }
    index = (index + 1) % pool->capacity;
  }

  interned_string_t interned = {.ptr = ptr, .len = (uint32_t)len};

  pool->entries[index].key = interned;
  pool->entries[index].value = interned;
  pool->size++;

  return interned;
}

interned_string_t string_pool_intern(string_pool_t *pool, const char *str,
                                     size_t len) {
  if (len == 0) {
    return interned_string_empty();
  }

  // First check if it already exists to avoid allocation
  if (pool->capacity > 0) {
    uint32_t hash = djb2_hash(str, len);
    size_t index = hash % pool->capacity;
    while (pool->entries[index].key.ptr != NULL) {
      if (pool->entries[index].key.len == len &&
          memcmp(pool->entries[index].key.ptr, str, len) == 0) {
        return pool->entries[index].value;
      }
      index = (index + 1) % pool->capacity;
    }
  }

  // Not found, duplicate and insert
  char *dup = malloc(len + 1);
  if (!dup) {
    return interned_string_empty();
  }
  memcpy(dup, str, len);
  dup[len] = '\0';

  if (pool->storage_size >= pool->storage_capacity) {
    size_t new_cap =
        pool->storage_capacity == 0 ? 16 : pool->storage_capacity * 2;
    char **new_storage = realloc(pool->storage, new_cap * sizeof(char *));
    if (!new_storage) {
      free(dup);
      return interned_string_empty();
    }
    pool->storage = new_storage;
    pool->storage_capacity = new_cap;
  }

  pool->storage[pool->storage_size++] = dup;

  return string_pool_insert_helper(pool, dup, len);
}

interned_string_t string_pool_intern_owned(string_pool_t *pool, char *owned,
                                           size_t len) {
  if (len == 0) {
    if (owned)
      free(owned);
    return interned_string_empty();
  }

  // First check if it already exists
  if (pool->capacity > 0) {
    uint32_t hash = djb2_hash(owned, len);
    size_t index = hash % pool->capacity;
    while (pool->entries[index].key.ptr != NULL) {
      if (pool->entries[index].key.len == len &&
          memcmp(pool->entries[index].key.ptr, owned, len) == 0) {
        free(owned);
        return pool->entries[index].value;
      }
      index = (index + 1) % pool->capacity;
    }
  }

  // Not found, take ownership
  if (pool->storage_size >= pool->storage_capacity) {
    size_t new_cap =
        pool->storage_capacity == 0 ? 16 : pool->storage_capacity * 2;
    char **new_storage = realloc(pool->storage, new_cap * sizeof(char *));
    if (!new_storage) {
      free(owned);
      return interned_string_empty();
    }
    pool->storage = new_storage;
    pool->storage_capacity = new_cap;
  }

  pool->storage[pool->storage_size++] = owned;

  return string_pool_insert_helper(pool, owned, len);
}

bool common_strings_init(common_strings_t *common, string_pool_t *pool) {
  common->pool = pool;
  common->lodash = string_pool_intern(pool, "lodash", 6);
  common->react = string_pool_intern(pool, "react", 5);
  common->typescript = string_pool_intern(pool, "typescript", 10);
  common->webpack = string_pool_intern(pool, "webpack", 7);
  common->babel = string_pool_intern(pool, "@babel/core", 11);
  common->eslint = string_pool_intern(pool, "eslint", 6);
  common->jest = string_pool_intern(pool, "jest", 4);
  common->express = string_pool_intern(pool, "express", 7);
  common->caret = string_pool_intern(pool, "^", 1);
  common->tilde = string_pool_intern(pool, "~", 1);

  // Check if any interning failed
  if (common->lodash.ptr == NULL || common->react.ptr == NULL ||
      common->typescript.ptr == NULL || common->webpack.ptr == NULL ||
      common->babel.ptr == NULL || common->eslint.ptr == NULL ||
      common->jest.ptr == NULL || common->express.ptr == NULL ||
      common->caret.ptr == NULL || common->tilde.ptr == NULL) {
    return false;
  }
  return true;
}
