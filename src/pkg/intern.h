#ifndef PKG_INTERN_H
#define PKG_INTERN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  const char *ptr;
  uint32_t len;
} interned_string_t;

// An empty interned string helper
inline interned_string_t interned_string_empty(void) {
  return (interned_string_t){.ptr = "", .len = 0};
}

inline bool interned_string_eql(interned_string_t a, interned_string_t b) {
  return a.ptr == b.ptr && a.len == b.len;
}

inline uint64_t interned_string_hash(interned_string_t s) {
  return (uint64_t)(uintptr_t)s.ptr;
}

typedef struct {
  interned_string_t key;
  interned_string_t value;
} string_pool_entry_t;

typedef struct {
  string_pool_entry_t *entries;
  size_t capacity;
  size_t size;

  char **storage;
  size_t storage_capacity;
  size_t storage_size;
} string_pool_t;

void string_pool_init(string_pool_t *pool);
void string_pool_deinit(string_pool_t *pool);

// Interns a string by copying it. Returns the interned string, or an empty
// string on error.
interned_string_t string_pool_intern(string_pool_t *pool, const char *str,
                                     size_t len);

// Interns a string and takes ownership of the malloc'd pointer `owned`. Returns
// the interned string. If the string is already interned, `owned` is freed.
interned_string_t string_pool_intern_owned(string_pool_t *pool, char *owned,
                                           size_t len);

typedef struct {
  string_pool_t *pool;
  interned_string_t lodash;
  interned_string_t react;
  interned_string_t typescript;
  interned_string_t webpack;
  interned_string_t babel;
  interned_string_t eslint;
  interned_string_t jest;
  interned_string_t express;
  interned_string_t caret;
  interned_string_t tilde;
} common_strings_t;

bool common_strings_init(common_strings_t *common, string_pool_t *pool);

#endif // PKG_INTERN_H
