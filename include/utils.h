#ifndef ANT_UTILS_H
#define ANT_UTILS_H
#define ARGTABLE_COUNT 10

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
  char *ptr;
  char *heap;
} cstr_buf_t;

extern const char *const module_resolve_extensions[];
uint64_t hash_key(const char *key, size_t len);

double half_to_double(uint16_t bits16);
uint16_t double_to_half(double value);

char hex_char(int v);
char *resolve_js_file(const char *filename);
char *resolve_typescript_source_fallback(const char *filename);

int hex_digit(char c);
int is_typescript_file(const char *filename);

int ant_mkdir_p(const char *path);
int ant_user_bin_path(char *out, size_t out_size);

int ant_xdg_cache_path(char *out, size_t out_size, const char *suffix);
int ant_xdg_data_path(char *out, size_t out_size, const char *suffix);
int ant_xdg_state_path(char *out, size_t out_size, const char *suffix);

bool ant_env_bool(const char *value, bool default_value);

int strip_typescript_inplace(
  char **buffer,
  size_t len,
  const char *filename,
  int is_module,
  size_t *out_len,
  const char **error_detail
);

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

typedef struct {
  const char *ptr;
  size_t len;
} repl_capture_t;

bool repl_template(
  const char *repl, size_t repl_len,
  const char *matched, size_t matched_len,
  const char *str, size_t str_len, size_t position,
  const repl_capture_t *caps, int ncaptures,
  char **buf, size_t *buf_len, size_t *buf_cap
);

#endif
