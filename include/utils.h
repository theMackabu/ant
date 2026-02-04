#ifndef ANT_UTILS_H
#define ANT_UTILS_H
#define ARGTABLE_COUNT 10

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

typedef struct {
  char *ptr;
  char *heap;
} cstr_buf_t;

const char *ant_semver(void);
uint64_t hash_key(const char *key, size_t len);

int is_typescript_file(const char *filename);
char *resolve_js_file(const char *filename);
int ant_version(void *argtable[]);

void *try_oom(size_t size);
void cstr_free(cstr_buf_t *buf);

char *cstr_init(
  cstr_buf_t *buf,
  char *stack,
  size_t stack_size,
  const char *src,
  size_t len
);

#define CSTR_BUF(name, size) \
  char name##_stack[size]; \
  cstr_buf_t name = {0}

#define CSTR_INIT(buf, src, len) \
  cstr_init(&(buf), buf##_stack, sizeof(buf##_stack), (src), (len))

#endif