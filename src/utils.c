#include "utils.h"
#include "messages.h"

#include <oxc.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <crprintf.h>

const char *const module_resolve_extensions[] = {
  ".js", ".mjs", ".cjs",
  ".ts", ".mts", ".cts",
  ".json", ".node", NULL
};

int hex_digit(char c) {
  static const int8_t lookup[256] = {
    ['0']=0, ['1']=1, ['2']=2, ['3']=3, ['4']=4, ['5']=5, ['6']=6, ['7']=7, ['8']=8, ['9']=9,
    ['a']=10, ['b']=11, ['c']=12, ['d']=13, ['e']=14, ['f']=15,
    ['A']=10, ['B']=11, ['C']=12, ['D']=13, ['E']=14, ['F']=15,
  };
  int8_t val = lookup[(unsigned char)c];
  return val ? val : (c == '0' ? 0 : -1);
}

char hex_char(int v) {
  return "0123456789abcdef"[v & 0x0f];
}

uint64_t hash_key(const char *key, size_t len) {
  uint64_t hash = 14695981039346656037ULL;
  size_t i = 0;
  
  for (; i + 8 <= len; i += 8) {
    uint64_t word;
    memcpy(&word, key + i, 8);
    hash ^= word;
    hash *= 1099511628211ULL;
  }
  
  for (; i < len; i++) {
    hash ^= (uint8_t)key[i];
    hash *= 1099511628211ULL;
  }
  
  return hash;
}

double half_to_double(uint16_t bits16) {
  uint32_t sign = ((uint32_t)bits16 & 0x8000u) << 16;
  uint32_t exp = ((uint32_t)bits16 >> 10) & 0x1fu;
  uint32_t mant = (uint32_t)bits16 & 0x03ffu;
  
  uint32_t bits32 = 0;
  float out = 0.0f;

  if (exp == 0) {
  if (mant == 0) {
    bits32 = sign;
  } else {
    uint32_t exp32 = 113;
    while ((mant & 0x0400u) == 0) {
      mant <<= 1;
      exp32--;
    }
    mant &= 0x03ffu;
    bits32 = sign | (exp32 << 23) | (mant << 13);
  }} 
  
  else if (exp == 0x1fu) bits32 = sign | 0x7f800000u | (mant << 13);
  else bits32 = sign | ((exp + 112u) << 23) | (mant << 13);

  memcpy(&out, &bits32, sizeof(out));
  return (double)out;
}

uint16_t double_to_half(double value) {
  float input = (float)value;
  
  uint32_t bits32 = 0;
  uint32_t sign = 0;
  uint32_t exp = 0;
  uint32_t mant = 0;
  int32_t exp16 = 0;
  uint16_t out = 0;

  memcpy(&bits32, &input, sizeof(bits32));
  sign = (bits32 >> 16) & 0x8000u;
  exp = (bits32 >> 23) & 0xffu;
  mant = bits32 & 0x007fffffu;

  if (exp == 0xffu) {
    if (mant != 0) return (uint16_t)(sign | 0x7e00u);
    return (uint16_t)(sign | 0x7c00u);
  }

  exp16 = (int32_t)exp - 127 + 15;
  if (exp16 >= 0x1f) return (uint16_t)(sign | 0x7c00u);

  if (exp16 <= 0) {
    if (exp16 < -10) return (uint16_t)sign;
    mant |= 0x00800000u;
    uint32_t shift = (uint32_t)(14 - exp16);
    uint16_t sub = (uint16_t)(mant >> shift);
    if ((mant >> (shift - 1)) & 1u) sub++;
    return (uint16_t)(sign | sub);
  }

  out = (uint16_t)(sign | ((uint32_t)exp16 << 10) | (mant >> 13));
  if (mant & 0x00001000u) out++;
  
  return out;
}

int is_typescript_file(const char *filename) {
  if (filename == NULL) return 0;
  size_t len = strlen(filename);
  if (len < 3) return 0;
  
  const char *ext = filename + len;
  while (ext > filename && *(ext - 1) != '.' && *(ext - 1) != '/') ext--;
  if (ext == filename || *(ext - 1) != '.') return 0;
  ext--;
  
  return (strcmp(ext, ".ts") == 0 || strcmp(ext, ".mts") == 0 || strcmp(ext, ".cts") == 0);
}

int strip_typescript_inplace(
  char **buffer,
  size_t len,
  const char *filename,
  size_t *out_len,
  const char **error_detail
) {
  if (out_len) *out_len = len;
  if (error_detail) *error_detail = NULL;
  if (!is_typescript_file(filename)) return 0;

  if (!buffer || !*buffer) {
    if (error_detail) *error_detail = "null input/output passed";
    return OXC_ERR_NULL_INPUT;
  }
  
  char *input = *buffer;
  char error_buf[256] = {0};
  size_t stripped_len = 0;
  
  int strip_error = OXC_ERR_TRANSFORM_FAILED;
  char *stripped = OXC_strip_types_owned(
    input, filename,
    &stripped_len, &strip_error,
    error_buf, sizeof(error_buf)
  );

  if (!stripped) {
    if (error_buf[0] != '\0') {
      size_t msg_len = strlen(error_buf);
      size_t copy_len = msg_len > len ? len : msg_len;
      memcpy(input, error_buf, copy_len);
      input[copy_len] = '\0';
    } else input[0] = '\0';

    if (error_detail) {
      *error_detail = input[0] != '\0' ? input : "unknown strip error";
    }
    
    return strip_error;
  }

  char *next = realloc(input, stripped_len + 1);
  if (!next) {
    free(stripped);
    if (error_detail) *error_detail = "out of memory while resizing strip output buffer";
    return OXC_ERR_OUTPUT_TOO_LARGE;
  }

  memcpy(next, stripped, stripped_len + 1);
  free(stripped);

  *buffer = next;
  if (out_len) *out_len = stripped_len;
  
  return 0;
}

static bool is_entrypoint_script_extension(const char *ext) {
  return 
    ext && 
    strcmp(ext, ".json") != 0 &&
    strcmp(ext, ".node") != 0;
}

static bool has_js_extension(const char *filename) {
  const char *slash = strrchr(filename, '/');
  const char *base = slash ? slash + 1 : filename;
  const char *dot = strrchr(base, '.');
  if (!dot) return false;
  for (const char *const *ext = module_resolve_extensions; *ext; ext++) {
    if (!is_entrypoint_script_extension(*ext)) continue;
    if (strcmp(dot, *ext) == 0) return true;
  }
  return false;
}

char *resolve_js_file(const char *filename) {
  extern bool esm_is_url(const char *path);
  if (!filename) return NULL;
  if (esm_is_url(filename)) return strdup(filename);
  
  struct stat st;
  if (stat(filename, &st) == 0) {
    if (S_ISREG(st.st_mode)) {
      const char *slash = strrchr(filename, '/');
      const char *base = slash ? slash + 1 : filename;
      const char *dot = strrchr(base, '.');
      if (dot && !has_js_extension(filename)) return NULL;
      return strdup(filename);
    }
    if (!S_ISDIR(st.st_mode)) return NULL;

    size_t len = strlen(filename);
    int has_slash = (len > 0 && filename[len - 1] == '/');
    
    for (const char *const *ext = module_resolve_extensions; *ext; ext++) {
      if (!is_entrypoint_script_extension(*ext)) continue;
      size_t ext_len = strlen(*ext);
      char *index_path = try_oom(len + 7 + ext_len + 1);
      sprintf(index_path, "%s%sindex%s", filename, has_slash ? "" : "/", *ext);
      if (stat(index_path, &st) == 0 && S_ISREG(st.st_mode)) return index_path;
      free(index_path);
    }
    
    return NULL;
  }
  
  if (has_js_extension(filename)) return NULL;
  size_t base_len = strlen(filename);
  
  for (const char *const *ext = module_resolve_extensions; *ext; ext++) {
    if (!is_entrypoint_script_extension(*ext)) continue;
    size_t ext_len = strlen(*ext);
    char *test_path = try_oom(base_len + ext_len + 1);
    
    memcpy(test_path, filename, base_len);
    memcpy(test_path + base_len, *ext, ext_len + 1);
    
    if (stat(test_path, &st) == 0 && S_ISREG(st.st_mode)) {
      return test_path;
    } free(test_path);
  }
  
  return NULL;
}

typedef struct {
  const char *repl; size_t repl_len; size_t *ri;
  const char *matched; size_t matched_len;
  const char *str; size_t str_len; size_t position;
  const repl_capture_t *caps; int ncaptures;
  char **buf; size_t *buf_len; size_t *buf_cap;
} rt_ctx_t;

static bool rt_append(rt_ctx_t *c, const char *data, size_t dlen) {
  if (dlen == 0) return true;
  if (*c->buf_len > SIZE_MAX - dlen - 1) return false;

  if (*c->buf_len + dlen >= *c->buf_cap) {
    size_t needed = *c->buf_len + dlen + 1;
    size_t new_cap = needed * 2;
    if (new_cap < needed) new_cap = needed;
    
    char *next = realloc(*c->buf, new_cap);
    if (!next) return false;
    *c->buf = next;
    *c->buf_cap = new_cap;
  }

  memcpy(*c->buf + *c->buf_len, data, dlen);
  *c->buf_len += dlen;
  return true;
}

static bool rt_dollar(rt_ctx_t *c) {
  *c->ri += 2;
  return rt_append(c, "$", 1);
}

static bool rt_match(rt_ctx_t *c) { 
  *c->ri += 2;
  return rt_append(c, c->matched, c->matched_len);
}

static bool rt_prefix(rt_ctx_t *c) { 
  *c->ri += 2;
  return rt_append(c, c->str, c->position);
}

static bool rt_suffix(rt_ctx_t *c) {
  size_t after = c->position + c->matched_len;
  bool ok = true;
  if (after < c->str_len)
    ok = rt_append(c, c->str + after, c->str_len - after);
  *c->ri += 2;
  return ok;
}

static bool rt_capture(rt_ctx_t *c) {
  char nc = c->repl[*c->ri + 1];
  int gn = nc - '0';
  *c->ri += 2;
  
  if (*c->ri < c->repl_len && c->repl[*c->ri] >= '0' && c->repl[*c->ri] <= '9') {
    int two = gn * 10 + (c->repl[*c->ri] - '0');
    if (two <= c->ncaptures) { gn = two; (*c->ri)++; }
  }
  
  if (gn > 0 && gn <= c->ncaptures && c->caps[gn - 1].ptr)
    return rt_append(c, c->caps[gn - 1].ptr, c->caps[gn - 1].len);
    
  if (gn == 0 || gn > c->ncaptures)
    return rt_append(c, "$", 1) && rt_append(c, &nc, 1);
    
  return true;
}

typedef bool (*rt_handler_t)(rt_ctx_t *);
static rt_handler_t rt_dispatch[128];
static bool rt_dispatch_init = false;

static void rt_init_dispatch(void) {
  if (rt_dispatch_init) return;
  rt_dispatch['$'] = rt_dollar;
  rt_dispatch['&'] = rt_match;
  rt_dispatch['`'] = rt_prefix;
  rt_dispatch['\''] = rt_suffix;
  for (int i = '0'; i <= '9'; i++) rt_dispatch[i] = rt_capture;
  rt_dispatch_init = true;
}

bool repl_template(
  const char *repl, size_t repl_len,
  const char *matched, size_t matched_len,
  const char *str, size_t str_len, size_t position,
  const repl_capture_t *caps, int ncaptures,
  char **buf, size_t *buf_len, size_t *buf_cap
) {
  rt_init_dispatch();
  rt_ctx_t c = {
    repl, repl_len, NULL,
    matched, matched_len,
    str, str_len, position,
    caps, ncaptures,
    buf, buf_len, buf_cap,
  };
  
  for (size_t ri = 0; ri < repl_len; ) {
    if (repl[ri] == '$' && ri + 1 < repl_len) {
      unsigned char nc = (unsigned char)repl[ri + 1];
      c.ri = &ri;
      rt_handler_t h = nc < 128 ? rt_dispatch[nc] : NULL;
      if (h) { if (!h(&c)) return false; continue; }
    }
    if (!rt_append(&c, &repl[ri], 1)) return false;
    ri++;
  }

  return true;
}

void *try_oom(size_t size) {
  void *p = malloc(size);
  if (!p) {
    crfprintf(stderr, msg.oom_fatal);
    exit(EXIT_FAILURE);
  } return p;
}

void cstr_free(cstr_buf_t *buf) {
  if (buf->heap) free(buf->heap);
}

char *cstr_init(cstr_buf_t *buf, char *stack, size_t stack_size, const char *src, size_t len) {
  if (len < stack_size) {
    buf->ptr = stack;
    buf->heap = NULL;
  } else {
    buf->heap = malloc(len + 1);
    if (!buf->heap) return NULL;
    buf->ptr = buf->heap;
  }
  memcpy(buf->ptr, src, len);
  buf->ptr[len] = '\0';
  return buf->ptr;
}
