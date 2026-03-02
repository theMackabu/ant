#if defined(__GNUC__) && !defined(__clang__)
  #pragma GCC optimize("O3,inline")
#endif

#include <compat.h> // IWYU pragma: keep

#include "ant.h"
#include "gc.h"
#include "utf8.h"
#include "debug.h"
#include "tokens.h"
#include "common.h"
#include "arena.h"
#include "utils.h"
#include "base64.h"
#include "runtime.h"
#include "internal.h"
#include "sugar.h"
#include "errors.h"
#include "descriptors.h"

#include "esm/remote.h"
#include "esm/loader.h"

#include "silver/ast.h"
#include "silver/lexer.h"
#include "silver/compiler.h"
#include "silver/engine.h"

#include <uv.h>
#include <oxc.h>
#include <assert.h>
#include <pcre2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <utarray.h>
#include <uthash.h>
#include <float.h>
#include <tlsuv/tlsuv.h>
#include <tlsuv/http.h>
#include <minicoro.h>

#ifdef _WIN32
#include <sys/stat.h>
#else
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/resource.h>
#endif

#include "modules/fs.h"
#include "modules/timer.h"
#include "modules/fetch.h"
#include "modules/symbol.h"
#include "modules/ffi.h"
#include "modules/child_process.h"
#include "modules/readline.h"
#include "modules/process.h"
#include "modules/date.h"
#include "modules/buffer.h"
#include "modules/collections.h"
#include "modules/navigator.h"
#include "modules/server.h"
#include "modules/events.h"
#include "modules/lmdb.h"
#include "modules/regex.h"

#define D(x) ((double)(x))

_Static_assert(sizeof(double) == 8, "NaN-boxing requires 64-bit IEEE 754 doubles");
_Static_assert(sizeof(uint64_t) == 8, "NaN-boxing requires 64-bit integers");
_Static_assert(sizeof(double) == sizeof(uint64_t), "double and uint64_t must have same size");

#if defined(__STDC_IEC_559__) || defined(__GCC_IEC_559)
#elif defined(__FAST_MATH__)
  #error "NaN-boxing is incompatible with -ffast-math"
#elif DBL_MANT_DIG != 53 || DBL_MAX_EXP != 1024
  #error "NaN-boxing requires IEEE 754 binary64 doubles"
#endif

static const char *INTERN_LENGTH = NULL;
static const char *INTERN_BUFFER = NULL;
static const char *INTERN_PROTOTYPE = NULL;
static const char *INTERN_CONSTRUCTOR = NULL;
static const char *INTERN_NAME = NULL;
static const char *INTERN_MESSAGE = NULL;
static const char *INTERN_VALUE = NULL;
static const char *INTERN_GET = NULL;
static const char *INTERN_SET = NULL;

static const char *INTERN_ARGUMENTS = NULL;
static const char *INTERN_CALLEE = NULL;
static const char *INTERN_IDX[10] = {NULL};

typedef struct interned_string {
  uint64_t hash;
  char *str;
  size_t len;
  struct interned_string *next;
} interned_string_t;

static size_t intern_count = 0;
static size_t intern_bytes = 0;
static interned_string_t *intern_buckets[ANT_LIMIT_SIZE_CACHE];

typedef struct promise_handler {
  jsval_t onFulfilled;
  jsval_t onRejected;
  jsval_t nextPromise;
} promise_handler_t;

static const UT_icd promise_handler_icd = {
  .sz = sizeof(promise_handler_t),
  .init = NULL,
  .copy = NULL,
  .dtor = NULL,
};

typedef struct {
  jsoff_t obj_off;
  const char *intern_ptr;
  jsoff_t prop_off;
  jsoff_t tail;
  uint64_t generation;
} intern_prop_cache_entry_t;

static uint64_t intern_prop_cache_gen = 1;
static intern_prop_cache_entry_t intern_prop_cache[ANT_LIMIT_SIZE_CACHE];

typedef struct promise_data_entry {
  jsval_t value;
  UT_array *handlers;
  uint32_t promise_id;
  uint32_t trigger_pid;
  jsoff_t obj_offset;
  int state;
  bool has_rejection_handler;
  bool processing;
  UT_hash_handle hh;
  UT_hash_handle hh_unhandled;
} promise_data_entry_t;

static promise_data_entry_t *promise_registry = NULL;
static promise_data_entry_t *unhandled_rejections = NULL;
static uint32_t next_promise_id = 1;

static promise_data_entry_t *get_promise_data(uint32_t promise_id, bool create);
static uint32_t get_promise_id(ant_t *js, jsval_t p);
static bool js_try_grow_memory(ant_t *js, size_t needed);

typedef struct proxy_data {
  jsoff_t obj_offset;
  jsval_t target;
  jsval_t handler;
  bool revoked;
  UT_hash_handle hh;
} proxy_data_t;

typedef struct dynamic_accessors {
  jsoff_t obj_offset;
  js_getter_fn getter;
  js_setter_fn setter;
  js_deleter_fn deleter;
  js_keys_fn keys;
  UT_hash_handle hh;
} dynamic_accessors_t;

static proxy_data_t *proxy_registry = NULL;
static proxy_data_t *get_proxy_data(jsval_t obj);
static dynamic_accessors_t *accessor_registry = NULL;

jsval_t tov(double d) {
  union { double d; jsval_t v; } u = {d};
  if (__builtin_expect(isnan(d), 0)) 
    return (u.v > NANBOX_PREFIX) 
    ? 0x7FF8000000000000ULL : u.v; // canonical NaN
  return u.v;
}

double tod(jsval_t v) {
  union { jsval_t v; double d; } u = {v}; return u.d;
}

static bool is_tagged(jsval_t v) {
  return v > NANBOX_PREFIX;
}

size_t vdata(jsval_t v) {
  return (size_t)(v & NANBOX_DATA_MASK);
}

static jsval_t get_slot(ant_t *js, jsval_t obj, internal_slot_t slot);
static void set_slot(ant_t *js, jsval_t obj, internal_slot_t slot, jsval_t value);

static jsval_t get_proto(ant_t *js, jsval_t obj);
static void set_proto(ant_t *js, jsval_t obj, jsval_t proto);

static inline jsoff_t offtolen(jsoff_t off) { return (off >> 3) - 1; }
static inline jsoff_t align64(jsoff_t v) { return (v + 7) & ~7ULL; }

static void saveoff(ant_t *js, jsoff_t off, jsoff_t val) {
  memcpy(&js->mem[off], &val, sizeof(val));
}

static void saveval(ant_t *js, jsoff_t off, jsval_t val) {
  memcpy(&js->mem[off], &val, sizeof(val));
}

const char *typestr(uint8_t t) {
  static const char *names[] = {
    [T_UNDEF] = "undefined", [T_NULL] = "object", [T_BOOL] = "boolean",
    [T_NUM] = "number", [T_BIGINT] = "bigint", [T_STR] = "string",
    [T_SYMBOL] = "symbol", [T_OBJ] = "object", [T_ARR] = "object",
    [T_FUNC] = "function", [T_CFUNC] = "function", [T_CLOSURE] = "closure",
    [T_PROMISE] = "promise", [T_GENERATOR] = "generator",
    [T_PROP] = "prop", [T_ERR] = "err", [T_TYPEDARRAY] = "typedarray",
    [T_FFI] = "ffi", [T_NTARG] = "ntarg"
  };

  return (t < sizeof(names) / sizeof(names[0])) ? names[t] : "??";
}

uint8_t vtype(jsval_t v) { 
  return is_tagged(v) ? ((v >> NANBOX_TYPE_SHIFT) & NANBOX_TYPE_MASK) : (uint8_t)T_NUM; 
}

jsval_t mkval(uint8_t type, uint64_t data) { 
  return NANBOX_PREFIX 
    | ((jsval_t)(type & NANBOX_TYPE_MASK) << NANBOX_TYPE_SHIFT) 
    | (data & NANBOX_DATA_MASK);
}

jsval_t js_obj_to_func(jsval_t obj) {
  sv_closure_t *closure = calloc(1, sizeof(sv_closure_t));
  closure->func_obj = (vtype(obj) == T_OBJ) ? obj : mkval(T_OBJ, vdata(obj));
  return mkval(T_FUNC, (uintptr_t)closure);
}

jsval_t js_mktypedarray(void *data) {
  return mkval(T_TYPEDARRAY, (uintptr_t)data);
}

void *js_gettypedarray(jsval_t val) {
  if (vtype(val) != T_TYPEDARRAY) return NULL;
  return (void *)vdata(val);
}

jsval_t js_get_slot(ant_t *js, jsval_t obj, internal_slot_t slot) { 
  return get_slot(js, js_as_obj(obj), slot); 
}

void js_set_slot(ant_t *js, jsval_t obj, internal_slot_t slot, jsval_t value) { 
  set_slot(js, js_as_obj(obj), slot, value); 
}

jsval_t js_mkffi(unsigned int index) {
  return mkval(T_FFI, (uint64_t)index);
}

int js_getffi(jsval_t val) {
  if (vtype(val) != T_FFI) return -1;
  return (int)vdata(val);
}

typedef enum {
  NTARG_INVALID = 0,
  NTARG_NEW_TARGET = 1
} ntarg_kind_t;

inline size_t js_getbrk(ant_t *js) { 
  return (size_t) js->brk;
}

static inline bool is_unboxed_obj(ant_t *js, jsval_t val, jsval_t expected_proto) {
  if (vtype(val) != T_OBJ) return false;
  if (vtype(get_slot(js, val, SLOT_PRIMITIVE)) != T_UNDEF) return false;
  jsval_t proto = get_slot(js, val, SLOT_PROTO);
  return vdata(proto) == vdata(expected_proto);
}

uint32_t js_to_uint32(double d) {
  if (!isfinite(d) || d == 0) return 0;
  double sign = (d < 0) ? -1.0 : 1.0;
  double posInt = sign * floor(fabs(d));
  double val = fmod(posInt, 4294967296.0);
  if (val < 0) val += 4294967296.0;
  return (uint32_t) val;
}

int32_t js_to_int32(double d) {
  uint32_t uint32 = js_to_uint32(d);
  if (uint32 >= 2147483648U) return (int32_t)(uint32 - 4294967296.0);
  return (int32_t) uint32;
}

static size_t strstring(ant_t *js, jsval_t value, char *buf, size_t len);
static size_t strkey(ant_t *js, jsval_t value, char *buf, size_t len);

jsoff_t vstrlen(ant_t *js, jsval_t v) { 
  jsoff_t off = (jsoff_t) vdata(v);
  jsoff_t header = loadoff(js, off);
  if (header & ROPE_FLAG) {
    return offtolen(header & ~(ROPE_FLAG | (ROPE_DEPTH_MASK << ROPE_DEPTH_SHIFT)));
  }
  return offtolen(header);
}

static jsval_t proxy_read_target(ant_t *js, jsval_t obj);
static jsoff_t proxy_aware_length(ant_t *js, jsval_t obj);
static jsval_t proxy_aware_get_elem(ant_t *js, jsval_t obj, const char *key, size_t key_len);
static bool bigint_is_zero(ant_t *js, jsval_t v);

static jsoff_t get_dense_buf(ant_t *js, jsval_t arr);
static jsoff_t dense_length(ant_t *js, jsoff_t doff);
static jsoff_t get_array_length(ant_t *js, jsval_t arr);
static jsval_t arr_get(ant_t *js, jsval_t arr, jsoff_t idx);
static bool arr_has(ant_t *js, jsval_t arr, jsoff_t idx);

static bool streq(const char *buf, size_t len, const char *p, size_t n);
static bool parse_func_params(ant_t *js, uint8_t *flags, int *out_count);
static bool try_dynamic_setter(ant_t *js, jsval_t obj, const char *key, size_t key_len, jsval_t value);
static jsoff_t lkp_with_setter(ant_t *js, jsval_t obj, const char *buf, size_t len, jsval_t *setter_out, bool *has_setter_out);
static jsval_t call_proto_accessor(ant_t *js, jsval_t prim, jsval_t accessor, bool has_accessor, jsval_t *arg, int arg_count, bool is_setter);
static jsval_t get_prototype_for_type(ant_t *js, uint8_t type);

static size_t strbigint(ant_t *js, jsval_t value, char *buf, size_t len);
static size_t tostr(ant_t *js, jsval_t value, char *buf, size_t len);
static size_t strpromise(ant_t *js, jsval_t value, char *buf, size_t len);
static jsval_t js_call_valueOf(ant_t *js, jsval_t value);
static jsval_t js_call_toString(ant_t *js, jsval_t value);
static jsval_t js_call_method(ant_t *js, jsval_t obj, const char *method, size_t method_len, jsval_t *args, int nargs);

static inline bool is_slot_prop(jsoff_t header);
static inline jsoff_t next_prop(jsoff_t header);

static jsval_t builtin_Object(ant_t *js, jsval_t *args, int nargs);
static jsval_t builtin_promise_then(ant_t *js, jsval_t *args, int nargs);
static jsval_t string_split_impl(ant_t *js, jsval_t str, jsval_t *args, int nargs);

static jsval_t proxy_get(ant_t *js, jsval_t proxy, const char *key, size_t key_len);
static jsval_t proxy_get_val(ant_t *js, jsval_t proxy, jsval_t key_val);
static jsval_t proxy_set(ant_t *js, jsval_t proxy, const char *key, size_t key_len, jsval_t value);
static jsval_t proxy_has(ant_t *js, jsval_t proxy, const char *key, size_t key_len);
static jsval_t proxy_has_val(ant_t *js, jsval_t proxy, jsval_t key_val);
static jsval_t proxy_delete(ant_t *js, jsval_t proxy, const char *key, size_t key_len);
static jsval_t proxy_delete_val(ant_t *js, jsval_t proxy, jsval_t key_val);

static jsval_t get_prototype_for_type(ant_t *js, uint8_t type);
static jsval_t get_ctor_proto(ant_t *js, const char *name, size_t len);
static jsoff_t lkp_interned(ant_t *js, jsval_t obj, const char *search_intern, size_t len);

static const char *bigint_digits(ant_t *js, jsval_t v, size_t *len);

typedef struct { jsval_t handle; bool is_new; } ctor_t;

static ctor_t get_constructor(ant_t *js, const char *name, size_t len) {
  ctor_t ctor;
  
  ctor.handle = get_ctor_proto(js, name, len);
  ctor.is_new = (vtype(js->new_target) != T_UNDEF);
  
  return ctor;
}

jsval_t unwrap_primitive(ant_t *js, jsval_t val) {
  if (vtype(val) != T_OBJ) return val;
  jsval_t prim = get_slot(js, val, SLOT_PRIMITIVE);
  if (vtype(prim) == T_UNDEF) return val;
  return prim;
}

static jsval_t to_string_val(ant_t *js, jsval_t val) {
  uint8_t t = vtype(val);
  if (t == T_STR) return val;
  if (t == T_OBJ) {
    jsval_t prim = get_slot(js, val, SLOT_PRIMITIVE);
    if (vtype(prim) == T_STR) return prim;
  }
  return js_call_toString(js, val);
}

bool js_truthy(ant_t *js, jsval_t v) {
  static const void *dispatch[] = {
    [T_OBJ]    = &&l_true,
    [T_FUNC]   = &&l_true,
    [T_CFUNC]  = &&l_true,
    [T_ARR]    = &&l_true,
    [T_SYMBOL] = &&l_true,
    [T_BOOL]   = &&l_bool,
    [T_STR]    = &&l_str,
    [T_BIGINT] = &&l_bigint,
    [T_NUM]    = &&l_num,
  };

  uint8_t t = vtype(v);
  if (t < sizeof(dispatch) / sizeof(*dispatch) && dispatch[t])
    goto *dispatch[t];
  return false;

  l_true:   return true;
  l_bool:   return vdata(v) != 0;
  l_str:    return vstrlen(js, v) > 0;
  l_bigint: return !bigint_is_zero(js, v);
  l_num: {
    double d = tod(v);
    return d != 0.0 && !isnan(d);
  }
}

static size_t cpy(char *dst, size_t dstlen, const char *src, size_t srclen) {
  if (dstlen == 0) return srclen;
  size_t len = srclen < dstlen - 1 ? srclen : dstlen - 1;
  memcpy(dst, src, len); dst[len] = '\0';
  return srclen;
}

size_t uint_to_str(char *buf, size_t bufsize, uint64_t val) {
  if (bufsize == 0) return 0;
  if (val == 0) {
    buf[0] = '0';
    buf[1] = '\0';
    return 1;
  }
  char temp[24];
  size_t len = 0;
  while (val > 0 && len < sizeof(temp)) {
    temp[len++] = '0' + (val % 10);
    val /= 10;
  }
  if (len >= bufsize) len = bufsize - 1;
  for (size_t i = 0; i < len; i++) {
    buf[i] = temp[len - 1 - i];
  }
  buf[len] = '\0';
  return len;
}

static jsval_t bigint_from_u64(ant_t *js, uint64_t value) {
  char buf[32];
  size_t len = uint_to_str(buf, sizeof(buf), value);
  return js_mkbigint(js, buf, len, false);
}

static jsval_t stringify_stack[MAX_STRINGIFY_DEPTH];
static int stringify_depth = 0;
static int stringify_indent = 0;

static jsval_t multiref_objs[MAX_MULTIREF_OBJS];
static int multiref_ids[MAX_MULTIREF_OBJS];
static int multiref_count = 0;
static int multiref_next_id = 0;

static void scan_refs(ant_t *js, jsval_t value);

static int find_multiref(jsval_t obj) {
  for (int i = 0; i < multiref_count; i++) {
    if (multiref_objs[i] == obj) return multiref_ids[i];
  }
  return 0;
}

static bool is_on_stack(jsval_t obj) {
  for (int i = 0; i < stringify_depth; i++) {
    if (stringify_stack[i] == obj) return true;
  }
  return false;
}

static void mark_multiref(jsval_t obj) {
  for (int i = 0; i < multiref_count; i++) {
    if (multiref_objs[i] == obj) {
      if (multiref_ids[i] == 0) multiref_ids[i] = ++multiref_next_id;
      return;
    }
  }
  if (multiref_count < MAX_MULTIREF_OBJS) {
    multiref_objs[multiref_count] = obj;
    multiref_ids[multiref_count] = 0;
    multiref_count++;
  }
}

static void scan_obj_refs(ant_t *js, jsval_t obj) {
  if (is_on_stack(obj)) {
    mark_multiref(obj);
    return;
  }
  
  if (stringify_depth >= MAX_STRINGIFY_DEPTH) return;
  stringify_stack[stringify_depth++] = obj;
  
  jsoff_t next = loadoff(js, (jsoff_t) vdata(js_as_obj(obj))) & ~(3U | FLAGMASK);
  while (next < js->brk && next != 0) {
    jsoff_t header = loadoff(js, next);
    if (!is_slot_prop(header)) {
      jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
      jsval_t val = loadval(js, next + (jsoff_t) (sizeof(next) + sizeof(koff)));
      scan_refs(js, val);
    }
    next = next_prop(header);
  }
  
  jsval_t proto_val = get_slot(js, obj, SLOT_PROTO);
  if (vtype(proto_val) == T_OBJ) scan_refs(js, proto_val);
  
  stringify_depth--;
}

static void scan_arr_refs(ant_t *js, jsval_t obj) {
  if (is_on_stack(obj)) {
    mark_multiref(obj);
    return;
  }
  
  if (stringify_depth >= MAX_STRINGIFY_DEPTH) return;
  stringify_stack[stringify_depth++] = obj;
  
  jsoff_t next = loadoff(js, (jsoff_t) vdata(js_as_obj(obj))) & ~(3U | FLAGMASK);
  while (next < js->brk && next != 0) {
    jsoff_t header = loadoff(js, next);
    if (!is_slot_prop(header)) {
      jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
      jsval_t val = loadval(js, next + (jsoff_t) (sizeof(next) + sizeof(koff)));
      scan_refs(js, val);
    }
    next = next_prop(header);
  }
  
  stringify_depth--;
}

static void scan_func_refs(ant_t *js, jsval_t value) {
  jsval_t func_obj = js_func_obj(value);
  
  if (is_on_stack(func_obj)) {
    mark_multiref(func_obj);
    return;
  }
  
  if (stringify_depth >= MAX_STRINGIFY_DEPTH) return;
  stringify_stack[stringify_depth++] = func_obj;
  
  jsoff_t next = loadoff(js, (jsoff_t) vdata(func_obj)) & ~(3U | FLAGMASK);
  while (next < js->brk && next != 0) {
    jsoff_t header = loadoff(js, next);
    if (!is_slot_prop(header)) {
      jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
      jsval_t val = loadval(js, next + (jsoff_t) (sizeof(next) + sizeof(koff)));
      scan_refs(js, val);
    }
    next = next_prop(header);
  }
  
  stringify_depth--;
}

static void scan_refs(ant_t *js, jsval_t value) {
  switch (vtype(value)) {
    case T_OBJ: scan_obj_refs(js, value); break;
    case T_ARR: scan_arr_refs(js, value); break;
    case T_FUNC: scan_func_refs(js, value); break;
    default: break;
  }
}

static int get_circular_ref(jsval_t obj) {
  if (is_on_stack(obj)) {
    int ref = find_multiref(obj);
    return ref ? ref : -1;
  }
  return 0;
}

static bool is_circular(jsval_t obj) {
  return is_on_stack(obj);
}

static int get_self_ref(jsval_t obj) {
  return find_multiref(obj);
}

static void push_stringify(jsval_t obj) {
  if (stringify_depth < MAX_STRINGIFY_DEPTH) {
    stringify_stack[stringify_depth++] = obj;
  }
}

static void pop_stringify(void) {
  if (stringify_depth > 0) stringify_depth--;
}

static size_t add_indent(char *buf, size_t len, int level) {
  size_t wanted = (size_t)(level * 2);
  size_t n = 0;
  for (int i = 0; i < level * 2 && n < len; i++) {
    buf[n++] = ' ';
  }
  return wanted;
}

static inline jsoff_t get_prop_koff(ant_t *js, jsoff_t prop) {
  return loadoff(js, prop + (jsoff_t) sizeof(prop));
}

static inline bool is_sym_key_prop(ant_t *js, jsoff_t prop) {
  jsoff_t koff = get_prop_koff(js, prop);
  return koff < js->brk && (loadoff(js, koff) & 0xF) == 0;
}

static void get_prop_key(ant_t *js, jsoff_t prop, const char **key, jsoff_t *klen) {
  jsoff_t koff = get_prop_koff(js, prop);
  *klen = offtolen(loadoff(js, koff));
  *key = (char *) &js->mem[koff + sizeof(koff)];
}

static jsval_t get_prop_val(ant_t *js, jsoff_t prop) {
  jsoff_t koff = get_prop_koff(js, prop);
  return loadval(js, prop + (jsoff_t) (sizeof(prop) + sizeof(koff)));
}

const char *get_str_prop(ant_t *js, jsval_t obj, const char *key, jsoff_t klen, jsoff_t *out_len) {
  jsoff_t off = lkp(js, obj, key, klen);
  if (off <= 0) return NULL;
  jsval_t v = resolveprop(js, mkval(T_PROP, off));
  if (vtype(v) != T_STR) return NULL;
  return (const char *)&js->mem[vstr(js, v, out_len)];
}

static bool is_small_array(ant_t *js, jsval_t obj, int *elem_count) {
  jsoff_t length = get_array_length(js, obj);
  if (length > 64) { if (elem_count) *elem_count = (int)length; return false; }
  
  int count = 0; bool has_nested = false;
  for (jsoff_t i = 0; i < length; i++) {
    jsval_t val = arr_get(js, obj, i); uint8_t t = vtype(val);
    if (t == T_OBJ || t == T_ARR || t == T_FUNC) has_nested = true;
    count++;
  }
  
  if (elem_count) *elem_count = count;
  return count <= 4 && !has_nested;
}

static inline bool is_array_index(const char *key, jsoff_t klen) {
  if (klen == 0 || (klen > 1 && key[0] == '0')) return false;
  for (jsoff_t i = 0; i < klen; i++) {
    if (key[i] < '0' || key[i] > '9') return false;
  }
  return true;
}

static inline bool parse_array_index(const char *key, size_t klen, jsoff_t max_len, unsigned long *out_idx) {
  if (klen == 0 || key[0] < '0' || key[0] > '9') return false;
  unsigned long parsed_idx = 0;
  
  for (size_t i = 0; i < klen; i++) {
    if (key[i] < '0' || key[i] > '9') return false;
    parsed_idx = parsed_idx * 10 + (key[i] - '0');
  }
  
  if (parsed_idx >= max_len) return false;
  *out_idx = parsed_idx;
  return true;
}

static jsoff_t get_array_length(ant_t *js, jsval_t arr) {
  jsoff_t doff = get_dense_buf(js, arr);
  jsoff_t dense_len = doff ? dense_length(js, doff) : 0;
  jsoff_t off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  if (off) {
    jsval_t val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(val) == T_NUM) {
      jsoff_t prop_len = (jsoff_t) tod(val);
      return prop_len > dense_len ? prop_len : dense_len;
    }
  }
  return dense_len;
}

static jsval_t get_obj_ctor(ant_t *js, jsval_t obj) {
  jsval_t ctor = get_slot(js, obj, SLOT_CTOR);
  if (vtype(ctor) == T_FUNC) return ctor;
  jsval_t proto = get_slot(js, obj, SLOT_PROTO);
  if (vtype(proto) != T_OBJ) return js_mkundef();
  jsoff_t off = lkp_interned(js, proto, INTERN_CONSTRUCTOR, 11);
  return off ? resolveprop(js, mkval(T_PROP, off)) : js_mkundef();
}

static const char *get_func_name(ant_t *js, jsval_t func, jsoff_t *out_len) {
  if (vtype(func) != T_FUNC) return NULL;
  jsoff_t off = lkp(js, js_func_obj(func), "name", 4);
  if (!off) return NULL;
  jsval_t name = resolveprop(js, mkval(T_PROP, off));
  if (vtype(name) != T_STR) return NULL;
  jsoff_t str_off = vstr(js, name, out_len);
  return (const char *) &js->mem[str_off];
}

static const char *get_class_name(ant_t *js, jsval_t obj, jsoff_t *out_len, const char *skip) {
  const char *name = get_func_name(js, get_obj_ctor(js, obj), out_len);
  if (!name) return NULL;
  if (skip && *out_len == (jsoff_t)strlen(skip) && memcmp(name, skip, *out_len) == 0) return NULL;
  return name;
}

static inline jsoff_t dense_iterable_length(ant_t *js, jsval_t obj) {
  jsoff_t doff = get_dense_buf(js, obj);
  return doff ? dense_length(js, doff) : 0;
}

static size_t strarr(ant_t *js, jsval_t obj, char *buf, size_t len) {
  int ref = get_circular_ref(obj);
  if (ref) return ref > 0 ? (size_t) snprintf(buf, len, "[Circular *%d]", ref) : cpy(buf, len, "[Circular]", 10);
  
  push_stringify(obj);
  jsoff_t first = loadoff(js, (jsoff_t) vdata(js_as_obj(obj))) & ~(3U | FLAGMASK);
  jsoff_t length = get_array_length(js, obj);
  jsoff_t d_len = dense_iterable_length(js, obj);
  jsoff_t iter_len = (d_len >= length) ? length : d_len;
  
  jsoff_t class_len = 0;
  const char *class_name = get_class_name(js, obj, &class_len, "Array");
  
  int elem_count = 0;
  bool inline_mode = is_small_array(js, obj, &elem_count);
  size_t n = 0;
  
  if (class_name) {
    n += cpy(buf + n, REMAIN(n, len), class_name, class_len);
    n += (size_t) snprintf(buf + n, REMAIN(n, len), "(%u) ", (unsigned) length);
  }
  
  if (length == 0) {
    n += cpy(buf + n, REMAIN(n, len), "[]", 2);
    pop_stringify();
    return n;
  }
  
  n += cpy(buf + n, REMAIN(n, len), inline_mode ? "[ " : "[\n", 2);
  if (!inline_mode) stringify_indent++;
  
  bool printed_first = false;
  for (jsoff_t i = 0; i < iter_len; i++) {
    if (printed_first) n += cpy(buf + n, REMAIN(n, len), inline_mode ? ", " : ",\n", 2);
    if (!inline_mode) n += add_indent(buf + n, REMAIN(n, len), stringify_indent);
    
    jsval_t val = arr_get(js, obj, i); bool found = arr_has(js, obj, i);
    n += found ? tostr(js, val, buf + n, REMAIN(n, len)) : cpy(buf + n, REMAIN(n, len), "undefined", 9);
    printed_first = true;
  }
  
  for (jsoff_t p = first; p < js->brk && p != 0; p = next_prop(loadoff(js, p))) {
    jsoff_t header = loadoff(js, p);
    if (is_slot_prop(header)) continue;
    
    const char *key; jsoff_t klen;
    get_prop_key(js, p, &key, &klen);
    if (streq(key, klen, "length", 6)) continue;
    
    if (printed_first) n += cpy(buf + n, REMAIN(n, len), inline_mode ? ", " : ",\n", 2);
    if (!inline_mode) n += add_indent(buf + n, REMAIN(n, len), stringify_indent);
    
    if (is_array_index(key, klen)) {
      n += tostr(js, get_prop_val(js, p), buf + n, REMAIN(n, len));
    } else {
      n += cpy(buf + n, REMAIN(n, len), key, klen);
      n += cpy(buf + n, REMAIN(n, len), ": ", 2);
      n += tostr(js, get_prop_val(js, p), buf + n, REMAIN(n, len));
    }
    printed_first = true;
  }
  
  if (!inline_mode) {
    stringify_indent--;
    n += cpy(buf + n, REMAIN(n, len), "\n", 1);
    n += add_indent(buf + n, REMAIN(n, len), stringify_indent);
  }
  
  n += cpy(buf + n, REMAIN(n, len), inline_mode ? " ]" : "]", inline_mode ? 2 : 1);
  pop_stringify();
  return n;
}

static size_t strdate(ant_t *js, jsval_t obj, char *buf, size_t len) {
  jsval_t time_val = js_get_slot(js, obj, SLOT_DATA);
  if (vtype(time_val) != T_NUM) return cpy(buf, len, "Invalid Date", 12);

  static const date_string_spec_t kSpec = {DATE_STRING_FMT_ISO, DATE_STRING_PART_ALL};
  jsval_t iso = get_date_string(js, obj, kSpec);
  if (is_err(iso) || vtype(iso) != T_STR) return cpy(buf, len, "Invalid Date", 12);

  jsoff_t slen;
  jsoff_t soff = vstr(js, iso, &slen);
  
  return cpy(buf, len, (const char *)&js->mem[soff], slen);
}

static bool is_valid_identifier(const char *str, jsoff_t slen) {
  if (slen == 0) return false;
  char c = str[0];
  if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$')) return false;
  for (jsoff_t i = 1; i < slen; i++) {
    c = str[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$')) return false;
  }
  return true;
}

static size_t strkey(ant_t *js, jsval_t value, char *buf, size_t len) {
  jsoff_t slen, off = vstr(js, value, &slen);
  const char *str = (const char *) &js->mem[off];
  
  if (is_valid_identifier(str, slen)) {
    return cpy(buf, len, str, slen);
  }
  return strstring(js, value, buf, len);
}

static bool is_small_object(ant_t *js, jsval_t obj, int *prop_count) {
  int count = 0;
  bool has_nested = false;
  
  jsoff_t obj_off = (jsoff_t)vdata(js_as_obj(obj));
  jsoff_t next = loadoff(js, obj_off) & ~(3U | FLAGMASK);
  
  while (next < js->brk && next != 0) {
    jsoff_t header = loadoff(js, next);
    if (is_slot_prop(header)) { next = next_prop(header); continue; }
    
    if (is_sym_key_prop(js, next)) {
      count++;
      next = next_prop(header);
      continue;
    }
    
    const char *key; jsoff_t klen;
    get_prop_key(js, next, &key, &klen);
    
    descriptor_entry_t *desc = lookup_descriptor(js, obj_off, key, klen);
    if (desc && !desc->enumerable) { next = next_prop(header); continue; }
    
    jsval_t val = get_prop_val(js, next);
    uint8_t t = vtype(val);
    if (t == T_OBJ || t == T_ARR || t == T_FUNC) has_nested = true;
    count++;
    
    next = next_prop(header);
  }
  
  descriptor_entry_t *desc, *tmp;
  HASH_ITER(hh, desc_registry, desc, tmp) {
    if (desc->obj_off != obj_off) continue;
    if (!desc->enumerable) continue;
    if (!desc->has_getter && !desc->has_setter) continue;
    count++;
  }
  
  if (prop_count) *prop_count = count;
  return count <= 4 && !has_nested;
}

// todo: split into smaller functions
static size_t strobj(ant_t *js, jsval_t obj, char *buf, size_t len) {
  jsval_t obj_proto = js_get_proto(js, obj);
  jsval_t date_proto = js_get_ctor_proto(js, "Date", 4);
  if (obj_proto == date_proto) return strdate(js, obj, buf, len);
  
  int ref = get_circular_ref(obj);
  if (ref) return ref > 0 ? (size_t) snprintf(buf, len, "[Circular *%d]", ref) : cpy(buf, len, "[Circular]", 10);
  
  push_stringify(obj);
  
  size_t n = 0;
  int self_ref = get_self_ref(obj);
  if (self_ref) {
    n += (size_t) snprintf(buf + n, REMAIN(n, len), "<ref *%d> ", self_ref);
  }
  
  jsval_t tag_sym = get_toStringTag_sym();
  jsoff_t tag_off = (vtype(tag_sym) == T_SYMBOL) ? lkp_sym_proto(js, obj, (jsoff_t)vdata(tag_sym)) : 0;
  bool is_map = false, is_set = false, is_arraybuffer = false;
  jsoff_t tlen = 0, toff = 0;
  const char *tag_str = NULL;
  int prop_count = 0;
  bool inline_mode = false;
  
  if (tag_off == 0) goto print_plain_object;
  
  jsval_t tag_val = resolveprop(js, mkval(T_PROP, tag_off));
  if (vtype(tag_val) != T_STR) goto print_plain_object;
  
  toff = vstr(js, tag_val, &tlen);
  tag_str = (const char *) &js->mem[toff];
  is_map = (tlen == 3 && memcmp(tag_str, "Map", 3) == 0);
  is_set = (tlen == 3 && memcmp(tag_str, "Set", 3) == 0);
  is_arraybuffer = (tlen >= 11 && memcmp(tag_str + tlen - 11, "ArrayBuffer", 11) == 0);
  
  jsval_t ta_slot = js_get_slot(js, obj, SLOT_BUFFER);
  if (vtype(ta_slot) == T_TYPEDARRAY) {
    TypedArrayData *ta = (TypedArrayData *)vdata(ta_slot);
    if (ta && ta->buffer) {
      static const char *ta_type_names[] = {
        "Int8Array", "Uint8Array", "Uint8ClampedArray",
        "Int16Array", "Uint16Array", "Int32Array", "Uint32Array",
        "Float32Array", "Float64Array", "BigInt64Array", "BigUint64Array"
      };
      
      const char *type_name = NULL;
      size_t type_len = 0;
      
      jsval_t proto = js_get_proto(js, obj);
      jsval_t buffer_proto = get_ctor_proto(js, "Buffer", 6);
      if (vtype(proto) == T_OBJ && vtype(buffer_proto) == T_OBJ && vdata(proto) == vdata(buffer_proto)) {
        type_name = "Buffer";
        type_len = 6;
      } else if (ta->type <= TYPED_ARRAY_BIGUINT64) {
        type_name = ta_type_names[ta->type];
        type_len = strlen(type_name);
      } else {
        type_name = "TypedArray";
        type_len = 10;
      }
      
      n += cpy(buf + n, REMAIN(n, len), type_name, type_len);
      n += (size_t) snprintf(buf + n, REMAIN(n, len), "(%zu) ", ta->length);
      n += cpy(buf + n, REMAIN(n, len), "[ ", 2);
      
      uint8_t *data = ta->buffer->data + ta->byte_offset;
      
      for (size_t i = 0; i < ta->length && i < 100; i++) {
        if (i > 0) n += cpy(buf + n, REMAIN(n, len), ", ", 2);
        
        switch (ta->type) {
          case TYPED_ARRAY_INT8:
            n += (size_t) snprintf(buf + n, REMAIN(n, len), "%d", (int)((int8_t*)data)[i]);
            break;
          case TYPED_ARRAY_UINT8:
          case TYPED_ARRAY_UINT8_CLAMPED:
            n += (size_t) snprintf(buf + n, REMAIN(n, len), "%u", (unsigned)data[i]);
            break;
          case TYPED_ARRAY_INT16:
            n += (size_t) snprintf(buf + n, REMAIN(n, len), "%d", (int)((int16_t*)data)[i]);
            break;
          case TYPED_ARRAY_UINT16:
            n += (size_t) snprintf(buf + n, REMAIN(n, len), "%u", (unsigned)((uint16_t*)data)[i]);
            break;
          case TYPED_ARRAY_INT32:
            n += (size_t) snprintf(buf + n, REMAIN(n, len), "%d", ((int32_t*)data)[i]);
            break;
          case TYPED_ARRAY_UINT32:
            n += (size_t) snprintf(buf + n, REMAIN(n, len), "%u", ((uint32_t*)data)[i]);
            break;
          case TYPED_ARRAY_FLOAT32:
            n += (size_t) snprintf(buf + n, REMAIN(n, len), "%g", (double)((float*)data)[i]);
            break;
          case TYPED_ARRAY_FLOAT64:
            n += (size_t) snprintf(buf + n, REMAIN(n, len), "%g", ((double*)data)[i]);
            break;
          case TYPED_ARRAY_BIGINT64:
            n += (size_t) snprintf(buf + n, REMAIN(n, len), "%lldn", (long long)((int64_t*)data)[i]);
            break;
          case TYPED_ARRAY_BIGUINT64:
            n += (size_t) snprintf(buf + n, REMAIN(n, len), "%llun", (unsigned long long)((uint64_t*)data)[i]);
            break;
          default:
            n += (size_t) snprintf(buf + n, REMAIN(n, len), "%u", (unsigned)data[i]);
            break;
        }
      }
      
      if (ta->length > 100) n += cpy(buf + n, REMAIN(n, len), ", ...", 5);
      n += cpy(buf + n, REMAIN(n, len), " ]", 2);
      pop_stringify();
      return n;
    }
  }
  
  if (is_arraybuffer) {
    jsval_t buf_val = js_get_slot(js, obj, SLOT_BUFFER);
    if (vtype(buf_val) == T_NUM) {
      ArrayBufferData *ab_data = (ArrayBufferData *)(uintptr_t)tod(buf_val);
      size_t bytelen = ab_data ? ab_data->length : 0;
      
      n += cpy(buf + n, REMAIN(n, len), tag_str, tlen);
      n += cpy(buf + n, REMAIN(n, len), " {\n", 3);
      n += cpy(buf + n, REMAIN(n, len), "  [Uint8Contents]: <", 20);
      
      if (ab_data && ab_data->data && bytelen > 0) {
        for (size_t i = 0; i < bytelen; i++) {
          if (i > 0) n += cpy(buf + n, REMAIN(n, len), " ", 1);
          n += (size_t) snprintf(buf + n, REMAIN(n, len), "%02x", ab_data->data[i]);
        }
      }
      
      n += cpy(buf + n, REMAIN(n, len), ">,\n", 3);
      n += cpy(buf + n, REMAIN(n, len), "  [byteLength]: ", 16);
      n += (size_t) snprintf(buf + n, REMAIN(n, len), "%zu", bytelen);
      n += cpy(buf + n, REMAIN(n, len), "\n}", 2);
      pop_stringify();
      return n;
    }
  }
  
  bool is_dataview = (tlen == 8 && memcmp(tag_str, "DataView", 8) == 0);
  if (is_dataview) {
    jsval_t dv_data_val = js_get_slot(js, obj, SLOT_DATA);
    if (vtype(dv_data_val) == T_NUM) {
      DataViewData *dv = (DataViewData *)(uintptr_t)tod(dv_data_val);
      if (dv && dv->buffer) {
        n += cpy(buf + n, REMAIN(n, len), "DataView {\n", 11);
        n += cpy(buf + n, REMAIN(n, len), "  [byteLength]: ", 16);
        n += (size_t) snprintf(buf + n, REMAIN(n, len), "%zu", dv->byte_length);
        n += cpy(buf + n, REMAIN(n, len), ",\n", 2);
        n += cpy(buf + n, REMAIN(n, len), "  [byteOffset]: ", 16);
        n += (size_t) snprintf(buf + n, REMAIN(n, len), "%zu", dv->byte_offset);
        n += cpy(buf + n, REMAIN(n, len), ",\n", 2);
        n += cpy(buf + n, REMAIN(n, len), "  [buffer]: ArrayBuffer {\n", 26);
        n += cpy(buf + n, REMAIN(n, len), "    [Uint8Contents]: <", 22);
        
        if (dv->buffer->data && dv->buffer->length > 0) {
          for (size_t i = 0; i < dv->buffer->length; i++) {
            if (i > 0) n += cpy(buf + n, REMAIN(n, len), " ", 1);
            n += (size_t) snprintf(buf + n, REMAIN(n, len), "%02x", dv->buffer->data[i]);
          }
        }
        
        n += cpy(buf + n, REMAIN(n, len), ">,\n", 3);
        n += cpy(buf + n, REMAIN(n, len), "    [byteLength]: ", 18);
        n += (size_t) snprintf(buf + n, REMAIN(n, len), "%zu", dv->buffer->length);
        n += cpy(buf + n, REMAIN(n, len), "\n  }\n}", 6);
        pop_stringify();
        return n;
      }
    }
  }
  
  if (is_map) {
    jsval_t map_val = js_get_slot(js, obj, SLOT_MAP);
    if (vtype(map_val) == T_UNDEF) goto print_tagged_object;
    
    map_entry_t **map_ptr = (map_entry_t**)(size_t)tod(map_val);
    n += cpy(buf + n, REMAIN(n, len), "Map(", 4);
    
    unsigned int count = 0;
    if (map_ptr && *map_ptr) count = HASH_COUNT(*map_ptr);
    n += (size_t) snprintf(buf + n, REMAIN(n, len), "%u", count);
    n += cpy(buf + n, REMAIN(n, len), ") ", 2);
    
    if (count == 0) {
      n += cpy(buf + n, REMAIN(n, len), "{}", 2);
    } else {
      n += cpy(buf + n, REMAIN(n, len), "{\n", 2);
      stringify_indent++;
      bool first = true;
      if (map_ptr && *map_ptr) {
        map_entry_t *entry, *tmp;
        HASH_ITER(hh, *map_ptr, entry, tmp) {
          if (!first) n += cpy(buf + n, REMAIN(n, len), ",\n", 2);
          first = false;
          n += add_indent(buf + n, REMAIN(n, len), stringify_indent);
          
          size_t key_len = strlen(entry->key);
          n += cpy(buf + n, REMAIN(n, len), "'", 1);
          n += cpy(buf + n, REMAIN(n, len), entry->key, key_len);
          n += cpy(buf + n, REMAIN(n, len), "'", 1);
          n += cpy(buf + n, REMAIN(n, len), " => ", 4);
          n += tostr(js, entry->value, buf + n, REMAIN(n, len));
        }
      }
      stringify_indent--;
      n += cpy(buf + n, REMAIN(n, len), "\n", 1);
      n += add_indent(buf + n, REMAIN(n, len), stringify_indent);
      n += cpy(buf + n, REMAIN(n, len), "}", 1);
    }
    pop_stringify();
    return n;
  }
  
  if (is_set) {
    jsval_t set_val = js_get_slot(js, obj, SLOT_SET);
    if (vtype(set_val) == T_UNDEF) goto print_tagged_object;
    
    set_entry_t **set_ptr = (set_entry_t**)(size_t)tod(set_val);
    n += cpy(buf + n, REMAIN(n, len), "Set(", 4);
    
    unsigned int count = 0;
    if (set_ptr && *set_ptr) count = HASH_COUNT(*set_ptr);
    n += (size_t) snprintf(buf + n, REMAIN(n, len), "%u", count);
    n += cpy(buf + n, REMAIN(n, len), ") ", 2);
    
    if (count == 0) {
      n += cpy(buf + n, REMAIN(n, len), "{}", 2);
    } else {
      n += cpy(buf + n, REMAIN(n, len), "{\n", 2);
      stringify_indent++;
      bool first = true;
      if (set_ptr && *set_ptr) {
        set_entry_t *entry, *tmp;
        HASH_ITER(hh, *set_ptr, entry, tmp) {
          if (!first) n += cpy(buf + n, REMAIN(n, len), ",\n", 2);
          first = false;
          n += add_indent(buf + n, REMAIN(n, len), stringify_indent);
          n += tostr(js, entry->value, buf + n, REMAIN(n, len));
        }
      }
      stringify_indent--;
      n += cpy(buf + n, REMAIN(n, len), "\n", 1);
      n += add_indent(buf + n, REMAIN(n, len), stringify_indent);
      n += cpy(buf + n, REMAIN(n, len), "}", 1);
    }
    pop_stringify();
    return n;
  }
  
print_tagged_object:
  n += cpy(buf + n, REMAIN(n, len), "Object [", 8);
  n += cpy(buf + n, REMAIN(n, len), (const char *) &js->mem[toff], tlen);
  n += cpy(buf + n, REMAIN(n, len), "] {\n", 4);
  goto continue_object_print;
  
print_plain_object:
  inline_mode = is_small_object(js, obj, &prop_count);
  
  jsval_t proto_val = js_get_proto(js, obj);
  bool is_null_proto = (vtype(proto_val) == T_NULL);
  bool proto_is_null_proto = false;
  const char *class_name = NULL;
  jsoff_t class_name_len = 0;
  
  do {
    if (is_null_proto) break;
    uint8_t pt = vtype(proto_val);
    if (pt != T_OBJ && pt != T_FUNC) break;
    
    jsval_t proto_proto = js_get_proto(js, proto_val);
    jsval_t object_proto = get_ctor_proto(js, "Object", 6);
    proto_is_null_proto = (vtype(proto_proto) == T_NULL) && 
                          (vdata(proto_val) != vdata(object_proto));
    
    class_name = get_class_name(js, obj, &class_name_len, "Object");
  } while (0);
  
  if (prop_count == 0) {
    if (is_null_proto) {
      n += cpy(buf + n, REMAIN(n, len), "[Object: null prototype] {}", 27);
    } else if (class_name && class_name_len > 0) {
      n += cpy(buf + n, REMAIN(n, len), class_name, class_name_len);
      if (proto_is_null_proto) {
        n += cpy(buf + n, REMAIN(n, len), " <[Object: null prototype] {}> {}", 33);
      } else n += cpy(buf + n, REMAIN(n, len), " {}", 3);
    } else if (proto_is_null_proto) {
      n += cpy(buf + n, REMAIN(n, len), "<[Object: null prototype] {}> {}", 32);
    } else n += cpy(buf + n, REMAIN(n, len), "{}", 2);
    pop_stringify();
    return n;
  }
  
  if (is_null_proto) {
    n += cpy(buf + n, REMAIN(n, len), "[Object: null prototype] ", 25);
  } else if (class_name && class_name_len > 0) {
    n += cpy(buf + n, REMAIN(n, len), class_name, class_name_len);
    if (proto_is_null_proto) {
      n += cpy(buf + n, REMAIN(n, len), " <[Object: null prototype] {}> ", 31);
    } else n += cpy(buf + n, REMAIN(n, len), " ", 1);
  } else if (proto_is_null_proto) {
    n += cpy(buf + n, REMAIN(n, len), "<[Object: null prototype] {}> ", 30);
  }
  
  n += cpy(buf + n, REMAIN(n, len), inline_mode ? "{ " : "{\n", 2);
  
continue_object_print:;
  
  if (!inline_mode) stringify_indent++;
  jsoff_t next = loadoff(js, (jsoff_t) vdata(js_as_obj(obj))) & ~(3U | FLAGMASK);
  bool first = true;
  
  jsoff_t obj_off = (jsoff_t)vdata(js_as_obj(obj));
  int prop_capacity = 64;
  
  jsoff_t *prop_offsets = malloc(prop_capacity * sizeof(jsoff_t));
  int num_props = 0;
  
  while (next < js->brk && next != 0) {
    jsoff_t header = loadoff(js, next);
    if (is_slot_prop(header)) { next = next_prop(header); continue; }
    
    if (is_sym_key_prop(js, next)) { next = next_prop(header); continue; }
    
    jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
    jsoff_t klen = offtolen(loadoff(js, koff));
    const char *key = (char *) &js->mem[koff + sizeof(koff)];
    
    descriptor_entry_t *desc = lookup_descriptor(js, obj_off, key, klen);
    if (desc && !desc->enumerable) { next = next_prop(header); continue; }
    
    if (num_props >= prop_capacity) {
      prop_capacity *= 2;
      prop_offsets = realloc(prop_offsets, prop_capacity * sizeof(jsoff_t));
    }
    prop_offsets[num_props++] = next;
    next = next_prop(header);
  }
  
  for (int i = 0; i < num_props; i++) {
    jsoff_t prop = prop_offsets[i];
    jsoff_t koff = loadoff(js, prop + (jsoff_t) sizeof(prop));
    jsoff_t klen = offtolen(loadoff(js, koff));
    const char *key = (char *) &js->mem[koff + sizeof(koff)];
    jsval_t val = loadval(js, prop + (jsoff_t) (sizeof(prop) + sizeof(koff)));
    
    if (!first) n += cpy(buf + n, REMAIN(n, len), inline_mode ? ", " : ",\n", 2);
    first = false;
    if (!inline_mode) n += add_indent(buf + n, REMAIN(n, len), stringify_indent);
    
    bool is_special_global = false;
    if (vtype(val) == T_UNDEF && streq(key, klen, "undefined", 9)) {
      is_special_global = true;
    } else if (vtype(val) == T_NUM) {
      double d = tod(val);
      if (isinf(d) && d > 0 && streq(key, klen, "Infinity", 8)) {
        is_special_global = true;
      } else if (isnan(d) && streq(key, klen, "NaN", 3)) is_special_global = true;
    }
    
    if (is_special_global) {
      n += tostr(js, val, buf + n, REMAIN(n, len));
    } else {
      n += strkey(js, mkval(T_STR, koff), buf + n, REMAIN(n, len));
      n += cpy(buf + n, REMAIN(n, len), ": ", 2);
      n += tostr(js, val, buf + n, REMAIN(n, len));
    }
  }
  free(prop_offsets);
  
  next = loadoff(js, obj_off) & ~(3U | FLAGMASK);
  while (next < js->brk && next != 0) {
    jsoff_t header = loadoff(js, next);
    if (is_slot_prop(header) || !is_sym_key_prop(js, next)) { 
      next = next_prop(header); continue; 
    }
    
    jsoff_t sym_off = get_prop_koff(js, next);
    if (vtype(tag_sym) == T_SYMBOL && sym_off == (jsoff_t)vdata(tag_sym)) { 
      next = next_prop(header); continue; 
    }
    
    jsval_t sym = mkval(T_SYMBOL, sym_off);
    jsval_t val = loadval(js, next + (jsoff_t)(sizeof(jsoff_t) * 2));
    
    if (!first) n += cpy(buf + n, REMAIN(n, len), inline_mode ? ", " : ",\n", 2);
    first = false;
    if (!inline_mode) n += add_indent(buf + n, REMAIN(n, len), stringify_indent);
    n += cpy(buf + n, REMAIN(n, len), "[", 1);
    n += tostr(js, sym, buf + n, REMAIN(n, len));
    n += cpy(buf + n, REMAIN(n, len), "]: ", 3);
    n += tostr(js, val, buf + n, REMAIN(n, len));
    
    next = next_prop(header);
  }
  
  descriptor_entry_t *desc, *tmp;
  HASH_ITER(hh, desc_registry, desc, tmp) {
    if (desc->obj_off != obj_off) continue;
    if (!desc->enumerable) continue;
    if (!desc->has_getter && !desc->has_setter) continue;
    
    if (!first) n += cpy(buf + n, REMAIN(n, len), inline_mode ? ", " : ",\n", 2);
    first = false;
    if (!inline_mode) n += add_indent(buf + n, REMAIN(n, len), stringify_indent);
    n += cpy(buf + n, REMAIN(n, len), desc->prop_name, desc->prop_len);
    n += cpy(buf + n, REMAIN(n, len), ": ", 2);
    
    if (desc->has_getter && desc->has_setter) {
      n += cpy(buf + n, REMAIN(n, len), "[Getter/Setter]", 15);
    } else if (desc->has_getter) {
      n += cpy(buf + n, REMAIN(n, len), "[Getter]", 8);
    } else n += cpy(buf + n, REMAIN(n, len), "[Setter]", 8);
  }
  
  if (!inline_mode) stringify_indent--;
  if (inline_mode) {
    n += cpy(buf + n, REMAIN(n, len), " }", 2);
  } else {
    if (!first) n += cpy(buf + n, REMAIN(n, len), "\n", 1);
    n += add_indent(buf + n, REMAIN(n, len), stringify_indent);
    n += cpy(buf + n, REMAIN(n, len), "}", 1);
  }
  pop_stringify();
  return n;
}

static size_t fix_exponent(char *buf, size_t n) {
  char *e = strchr(buf, 'e');
  if (!e) return n;
  
  char *src = e + 1;
  char *dst = src;
  
  if (*src == '+' || *src == '-') {
    dst++;
    src++;
  }
  
  while (*src == '0' && src[1] != '\0') src++;
  
  if (src != dst) {
    memmove(dst, src, strlen(src) + 1);
    return strlen(buf);
  }
  return n;
}

static size_t strnum(jsval_t value, char *buf, size_t len) {
  double dv = tod(value);
  
  if (isnan(dv)) return cpy(buf, len, "NaN", 3);
  if (isinf(dv)) return cpy(buf, len, dv > 0 ? "Infinity" : "-Infinity", dv > 0 ? 8 : 9);
  if (dv == 0.0) return cpy(buf, len, "0", 1);
  
  char temp[64];
  int sign = dv < 0 ? 1 : 0;
  double adv = sign ? -dv : dv;
  
  double iv;
  double frac = modf(adv, &iv);
  if (frac == 0.0 && adv < 9007199254740992.0) {
    int result = snprintf(temp, sizeof(temp), "%.0f", dv);
    fix_exponent(temp, (size_t)result);
    return cpy(buf, len, temp, strlen(temp));
  }
  
  for (int prec = 1; prec <= 17; prec++) {
    int n = snprintf(temp, sizeof(temp), "%.*g", prec, dv);
    double parsed = strtod(temp, NULL);
    if (parsed == dv) {
      fix_exponent(temp, (size_t)n);
      return cpy(buf, len, temp, strlen(temp));
    }
    (void)n;
  }
  
  int result = snprintf(temp, sizeof(temp), "%.17g", dv);
  fix_exponent(temp, (size_t)result);
  return cpy(buf, len, temp, strlen(temp));
}

static inline jsoff_t assert_string_header(ant_t *js, jsval_t value, jsoff_t *out_off) {
  assert(vtype(value) == T_STR);
  jsoff_t off = (jsoff_t) vdata(value);
  assert(off + sizeof(jsoff_t) <= js->brk);
  jsoff_t header = loadoff(js, off);
  assert((header & 3) == T_STR);
  if (out_off) *out_off = off;
  return header;
}

static inline jsoff_t assert_flat_string_len(ant_t *js, jsval_t value, jsoff_t *out_off) {
  jsoff_t off;
  jsoff_t header = assert_string_header(js, value, &off);
  assert((header & ROPE_FLAG) == 0);
  jsoff_t len = offtolen(header);
  assert(off + sizeof(jsoff_t) + len <= js->brk);
  if (out_off) *out_off = off;
  return len;
}

static inline jsoff_t assert_rope_header(ant_t *js, jsval_t value, jsoff_t *out_off) {
  jsoff_t off;
  jsoff_t header = assert_string_header(js, value, &off);
  assert((header & ROPE_FLAG) != 0);
  assert(off + sizeof(rope_node_t) <= js->brk);
  jsoff_t payload_header = header & ~(ROPE_FLAG | (ROPE_DEPTH_MASK << ROPE_DEPTH_SHIFT));
  assert((payload_header >> 3) > 0);
  if (out_off) *out_off = off;
  return header;
}

bool is_rope(ant_t *js, jsval_t value) {
  jsoff_t header = assert_string_header(js, value, NULL);
  if ((header & ROPE_FLAG) != 0) assert_rope_header(js, value, NULL);
  return (header & ROPE_FLAG) != 0;
}

static inline jsoff_t rope_len(ant_t *js, jsval_t value) {
  jsoff_t header = assert_rope_header(js, value, NULL);
  return offtolen(header & ~(ROPE_FLAG | (ROPE_DEPTH_MASK << ROPE_DEPTH_SHIFT)));
}

static inline uint8_t rope_depth(ant_t *js, jsval_t value) {
  jsoff_t off = (jsoff_t) vdata(value);
  jsoff_t header = loadoff(js, off);
  return (uint8_t)((header >> ROPE_DEPTH_SHIFT) & ROPE_DEPTH_MASK);
}

static inline jsval_t rope_left(ant_t *js, jsval_t value) {
  jsoff_t off = (jsoff_t) vdata(value);
  return loadval(js, off + offsetof(rope_node_t, left));
}

static inline jsval_t rope_right(ant_t *js, jsval_t value) {
  jsoff_t off = (jsoff_t) vdata(value);
  return loadval(js, off + offsetof(rope_node_t, right));
}

static inline jsval_t rope_cached_flat(ant_t *js, jsval_t value) {
  jsoff_t off = (jsoff_t) vdata(value);
  return loadval(js, off + offsetof(rope_node_t, cached));
}

static inline void rope_set_cached_flat(ant_t *js, jsval_t rope, jsval_t flat) {
  jsoff_t off = (jsoff_t) vdata(rope);
  saveval(js, off + offsetof(rope_node_t, cached), flat);
}

static void rope_flatten_into(ant_t *js, jsval_t str, char *dest, jsoff_t *pos) {
  assert(vtype(str) == T_STR);
  
  if (!is_rope(js, str)) {
    jsoff_t soff;
    jsoff_t slen = assert_flat_string_len(js, str, &soff);
    memcpy(dest + *pos, &js->mem[soff + sizeof(jsoff_t)], slen);
    *pos += slen; return;
  }
  
  jsval_t cached = rope_cached_flat(js, str);
  if (vtype(cached) == T_STR && !is_rope(js, cached)) {
    jsoff_t coff;
    jsoff_t clen = assert_flat_string_len(js, cached, &coff);
    memcpy(dest + *pos, &js->mem[coff + sizeof(jsoff_t)], clen);
    *pos += clen; return;
  }
  
  jsval_t stack[ROPE_MAX_DEPTH + 8];
  int sp = 0; stack[sp++] = str;
  
  while (sp > 0) {
    jsval_t node = stack[--sp];
    assert(vtype(node) == T_STR);
    
    if (!is_rope(js, node)) {
      jsoff_t soff;
      jsoff_t slen = assert_flat_string_len(js, node, &soff);
      memcpy(dest + *pos, &js->mem[soff + sizeof(jsoff_t)], slen);
      *pos += slen; continue;
    }
    
    jsval_t c = rope_cached_flat(js, node);
    if (vtype(c) == T_STR && !is_rope(js, c)) {
      jsoff_t coff;
      jsoff_t clen = assert_flat_string_len(js, c, &coff);
      memcpy(dest + *pos, &js->mem[coff + sizeof(jsoff_t)], clen);
      *pos += clen; continue;
    }
    
    if (sp + 2 <= ROPE_MAX_DEPTH + 8) {
      stack[sp++] = rope_right(js, node);
      stack[sp++] = rope_left(js, node);
    }
  }
}

jsval_t rope_flatten(ant_t *js, jsval_t rope) {
  assert(vtype(rope) == T_STR);
  if (!is_rope(js, rope)) return rope;
  
  jsval_t cached = rope_cached_flat(js, rope);
  if (vtype(cached) == T_STR && !is_rope(js, cached)) return cached;
  
  jsoff_t total_len = rope_len(js, rope);
  
  char *buf = (char *)ant_calloc(total_len + 1);
  if (!buf) return js_mkerr(js, "oom");
  
  jsoff_t pos = 0;
  rope_flatten_into(js, rope, buf, &pos);
  buf[pos] = '\0';
  
  jsval_t flat = js_mkstr(js, buf, pos);
  free(buf);
  
  if (!is_err(flat)) {
    rope_set_cached_flat(js, rope, flat);
  }
  
  return flat;
}

jsoff_t vstr(ant_t *js, jsval_t value, jsoff_t *len) {
  jsoff_t header = assert_string_header(js, value, NULL);
  
  if (header & ROPE_FLAG) {
    jsval_t flat = rope_flatten(js, value);
    assert(!is_err(flat));
    value = flat;
  }
  
  jsoff_t off;
  jsoff_t slen = assert_flat_string_len(js, value, &off);
  if (len) *len = slen;
  return (jsoff_t) (off + sizeof(off));
}

static size_t strstring(ant_t *js, jsval_t value, char *buf, size_t len) {
  jsoff_t slen, off = vstr(js, value, &slen);
  const char *str = (const char *) &js->mem[off];
  size_t n = 0;
  n += cpy(buf + n, REMAIN(n, len), "'", 1);
  for (jsoff_t i = 0; i < slen && n < len - 1; i++) {
    char c = str[i];
    if (c == '\n') { n += cpy(buf + n, REMAIN(n, len), "\\n", 2); }
    else if (c == '\r') { n += cpy(buf + n, REMAIN(n, len), "\\r", 2); }
    else if (c == '\t') { n += cpy(buf + n, REMAIN(n, len), "\\t", 2); }
    else if (c == '\\') { n += cpy(buf + n, REMAIN(n, len), "\\\\", 2); }
    else if (c == '\'') { n += cpy(buf + n, REMAIN(n, len), "\\'", 2); }
    else { if (n < len) buf[n++] = c; }
  }
  n += cpy(buf + n, REMAIN(n, len), "'", 1);
  
  return n;
}

static const char *intern_string(const char *str, size_t len) {
  uint64_t h = hash_key(str, len);
  uint32_t bucket = (uint32_t)(h & (ANT_LIMIT_SIZE_CACHE - 1));
  
  for (interned_string_t *e = intern_buckets[bucket]; e; e = e->next) {
    if (e->hash == h && e->len == len && memcmp(e->str, str, len) == 0) return e->str;
  }
  
  size_t alloc_size = sizeof(interned_string_t) + len + 1;
  interned_string_t *entry = (interned_string_t *)ant_calloc(alloc_size);
  if (!entry) return NULL;
  
  entry->str = (char *)(entry + 1);
  memcpy(entry->str, str, len);
  entry->str[len] = '\0';
  entry->len = len;
  entry->hash = h;
  entry->next = intern_buckets[bucket];
  intern_buckets[bucket] = entry;
  
  intern_count++;
  intern_bytes += alloc_size;
  
  return entry->str;
}

js_intern_stats_t js_intern_stats(void) {
  return (js_intern_stats_t){ 
    .count = intern_count, 
    .bytes = intern_bytes 
  };
}

bool is_internal_prop(const char *key, jsoff_t klen) {
  if (klen < 2) return false;
  if (key[0] != '_' || key[1] != '_') return false;
  if (klen == STR_PROTO_LEN && memcmp(key, STR_PROTO, STR_PROTO_LEN) == 0) return false;
  if (klen >= 9 && key[2] == 's' && key[3] == 'y' && key[4] == 'm' && key[5] == '_' && key[klen-1] == '_' && key[klen-2] == '_') return true;
  return true;
}

struct func_format {
  const char *prefix;
  size_t prefix_len;
  const char *anon;
  size_t anon_len;
};

static const struct func_format formats[] = {
  [0] = { "[Function: ",      11, "[Function (anonymous)]",      22 },
  [1] = { "[AsyncFunction: ", 16, "[AsyncFunction (anonymous)]", 27 },
};

// todo: make it work with bytecode NAME
static size_t strfunc(ant_t *js, jsval_t value, char *buf, size_t len) {
  jsoff_t name_len = 0;
  const char *name = get_func_name(js, value, &name_len);
  
  jsval_t func_obj = js_func_obj(value);
  jsval_t code_slot = get_slot(js, func_obj, SLOT_CODE);
  jsval_t builtin_slot = get_slot(js, func_obj, SLOT_BUILTIN);
  jsval_t async_slot = get_slot(js, func_obj, SLOT_ASYNC);
  
  bool is_async = (async_slot == js_true);
  bool has_code = (vtype(code_slot) == T_CFUNC);
  
  const struct func_format *fmt = &formats[is_async];
  
  if (vtype(builtin_slot) == T_NUM) {
    if (name && name_len > 0) {
      size_t n = cpy(buf, len, fmt->prefix, fmt->prefix_len);
      n += cpy(buf + n, REMAIN(n, len), name, name_len);
      n += cpy(buf + n, REMAIN(n, len), "]", 1);
      return n;
    }
    return cpy(buf, len, fmt->anon, fmt->anon_len);
  }
  
  if (!has_code) {
    jsval_t cfunc_slot = get_slot(js, func_obj, SLOT_CFUNC);
    bool is_native = (vtype(cfunc_slot) == T_CFUNC);
    size_t n;
    
    if (name && name_len > 0) {
      n = cpy(buf, len, fmt->prefix, fmt->prefix_len);
      n += cpy(buf + n, REMAIN(n, len), name, name_len);
      n += cpy(buf + n, REMAIN(n, len), "]", 1);
    } else {
      n = cpy(buf, len, fmt->anon, fmt->anon_len);
    }
    
    if (!is_native) return n;
    
    jsval_t proto = get_slot(js, func_obj, SLOT_PROTO);
    uint8_t pt = vtype(proto);
    if (pt != T_OBJ && pt != T_FUNC) return n;
    
    jsoff_t ctor_off = lkp(js, proto, "constructor", 11);
    if (ctor_off == 0) return n;
    
    jsval_t ctor = resolveprop(js, mkval(T_PROP, ctor_off));
    uint8_t ct = vtype(ctor);
    if (ct != T_FUNC && ct != T_CFUNC) return n;
    
    jsoff_t ctor_name_len = 0;
    const char *ctor_name = get_func_name(js, ctor, &ctor_name_len);
    if (ctor_name && ctor_name_len > 0) {
      n += cpy(buf + n, REMAIN(n, len), " ", 1);
      n += cpy(buf + n, REMAIN(n, len), ctor_name, ctor_name_len);
    }
    return n;
  }
  
  if (name && name_len > 0) {
    size_t n = cpy(buf, len, fmt->prefix, fmt->prefix_len);
    n += cpy(buf + n, REMAIN(n, len), name, name_len);
    n += cpy(buf + n, REMAIN(n, len), "]", 1);
    return n;
  }
  
  return cpy(buf, len, fmt->anon, fmt->anon_len);
}

static size_t tostr(ant_t *js, jsval_t value, char *buf, size_t len) {
  switch (vtype(value)) {
    case T_UNDEF:   return ANT_COPY(buf, len, "undefined");
    case T_NULL:    return ANT_COPY(buf, len, "null");
    
    case T_BOOL: {
      bool b = vdata(value) & 1;
      return b ? ANT_COPY(buf, len, "true") : ANT_COPY(buf, len, "false");
    }
    
    case T_ARR:     return strarr(js, value, buf, len);
    case T_OBJ:     return strobj(js, value, buf, len);
    case T_STR:     return strstring(js, value, buf, len);
    case T_NUM:     return strnum(value, buf, len);
    case T_BIGINT:  return strbigint(js, value, buf, len);
    case T_PROMISE: return strpromise(js, value, buf, len);
    case T_FUNC:    return strfunc(js, value, buf, len);
    
    case T_CFUNC:   return ANT_COPY(buf, len, "[native code]");
    case T_FFI:     return ANT_COPY(buf, len, "[native code (ffi)]");
    
    case T_ERR: {
      uint64_t data = vdata(value);
      if (data != 0) {
        jsval_t obj = mkval(T_OBJ, data);
        jsval_t stack = js_get(js, obj, "stack");
        if (vtype(stack) == T_STR) {
          jsoff_t slen;
          jsoff_t off = vstr(js, stack, &slen);
          return cpy(buf, len, (const char *)&js->mem[off], slen);
        }
      }
      return ANT_COPY(buf, len, "Error");
    }
    
    case T_SYMBOL: {
      const char *desc = js_sym_desc(js, value);
      if (desc) return (size_t) snprintf(buf, len, "Symbol(%s)", desc);
      return ANT_COPY(buf, len, "Symbol()");
    }
    
    case T_PROP:    return (size_t) snprintf(buf, len, "PROP@%lu", (unsigned long) vdata(value)); 
    default:        return (size_t) snprintf(buf, len, "VTYPE%d", vtype(value));
  }
}

static char *tostr_alloc(ant_t *js, jsval_t value) {
  size_t cap = 64;
  char *buf = ant_calloc(cap);
  size_t n = tostr(js, value, buf, cap);
  if (n >= cap) {
    free(buf);
    buf = ant_calloc(n + 1);
    tostr(js, value, buf, n + 1);
  }
  return buf;
}

js_cstr_t js_to_cstr(ant_t *js, jsval_t value, char *stack_buf, size_t stack_size) {
  js_cstr_t out = { .ptr = "", .len = 0, .needs_free = false };

  if (is_err(value)) {
    uint64_t data = vdata(value);
    if (data != 0) {
      jsval_t obj = mkval(T_OBJ, data);
      jsval_t stack = js_get(js, obj, "stack");
      if (vtype(stack) == T_STR) {
        jsoff_t slen;
        jsoff_t off = vstr(js, stack, &slen);
        out.ptr = (const char *)&js->mem[off];
        out.len = slen;
        return out;
      }
    }
    out.ptr = "Error";
    out.len = 5;
    return out;
  }

  if (vtype(value) == T_STR) {
    size_t len = 0;
    char *str = js_getstr(js, value, &len);
    out.ptr = str ? str : ""; out.len = len;
    return out;
  }

  multiref_count = 0;
  multiref_next_id = 0;
  stringify_depth = 0;
  scan_refs(js, value);

  size_t capacity = stack_size;
  char *buf = stack_buf;
  out.needs_free = false;

  if (!buf || capacity == 0) {
    capacity = 64;
    buf = ant_calloc(capacity);
    if (!buf) return out;
    out.needs_free = true;
  }

  for (;;) {
    stringify_depth = 0;
    stringify_indent = 0;
    size_t len = tostr(js, value, buf, capacity);

    if (len < capacity - 1) {
      out.ptr = buf;
      out.len = len;
      return out;
    }

    size_t new_capacity = capacity * 2;
    char *next = out.needs_free 
      ? ant_realloc(buf, new_capacity) 
      : ant_calloc(new_capacity);
    
    if (!next) {
      if (out.needs_free) free(buf);
      out.ptr = ""; out.len = 0;
      out.needs_free = false;
      return out;
    }

    if (!out.needs_free) {
      memcpy(next, buf, capacity);
      out.needs_free = true;
    }

    buf = next;
    capacity = new_capacity;
  }
}

jsval_t js_tostring_val(ant_t *js, jsval_t value) {
  uint8_t t = vtype(value);
  char *buf; size_t len, buflen;
  
  static const void *jump_table[] = {
    [T_OBJ] = &&L_OBJ, [T_PROP] = &&L_DEFAULT, [T_STR] = &&L_STR,
    [T_UNDEF] = &&L_UNDEF, [T_NULL] = &&L_NULL, [T_NUM] = &&L_NUM,
    [T_BOOL] = &&L_BOOL, [T_FUNC] = &&L_OBJ, [T_CFUNC] = &&L_DEFAULT, 
    [T_ERR] = &&L_DEFAULT, [T_ARR] = &&L_OBJ,
    [T_PROMISE] = &&L_DEFAULT, [T_TYPEDARRAY] = &&L_DEFAULT,
    [T_BIGINT] = &&L_BIGINT, [T_SYMBOL] = &&L_DEFAULT, 
    [T_GENERATOR] = &&L_DEFAULT, [T_FFI] = &&L_DEFAULT
  };
  
  if (t < sizeof(jump_table) / sizeof(jump_table[0])) goto *jump_table[t];
  goto L_DEFAULT;

  L_STR:   return value;
  L_UNDEF: return js_mkstr(js, "undefined", 9);
  L_NULL:  return js_mkstr(js, "null", 4);
  L_BOOL:  return vdata(value) ? js_mkstr(js, "true", 4) : js_mkstr(js, "false", 5);
  L_OBJ:   return js_call_toString(js, value);
  
  L_NUM: {
    buf = (char *)ant_calloc(32);
    len = strnum(value, buf, 32);
    jsval_t result = js_mkstr(js, buf, len);
    free(buf); return result;
  }
    
  L_BIGINT: {
    bigint_digits(js, value, &buflen);
    buf = (char *)ant_calloc(buflen + 2);
    len = strbigint(js, value, buf, buflen + 2);
    jsval_t result = js_mkstr(js, buf, len);
    free(buf); return result;
  }
    
  L_DEFAULT: {
    buf = (char *)ant_calloc(64);
    len = tostr(js, value, buf, 64);
    jsval_t result = js_mkstr(js, buf, len);
    free(buf); return result;
  }
}

const char *js_str(ant_t *js, jsval_t value) {
  if (is_err(value)) {
    uint64_t data = vdata(value);
    if (data != 0) {
      jsval_t obj = mkval(T_OBJ, data);
      jsval_t stack = js_get(js, obj, "stack");
      if (vtype(stack) == T_STR) {
        jsoff_t slen, off = vstr(js, stack, &slen);
        return (const char *)&js->mem[off];
      }
    }
    return "Error";
  }
  
  multiref_count = 0;
  multiref_next_id = 0;
  stringify_depth = 0;
  scan_refs(js, value);
  
  size_t capacity = 4096;
  char *buf = (char *)ant_calloc(capacity);
  if (!buf) return "";
  
  size_t len;
  for (;;) {
    stringify_depth = 0;
    stringify_indent = 0;
    len = tostr(js, value, buf, capacity);
    
    if (len < capacity - 1) break;
    
    capacity *= 2;
    buf = (char *)ant_realloc(buf, capacity);
    if (!buf) return "";
  }
  
  jsval_t str = js_mkstr(js, buf, len);
  free(buf);
  
  if (is_err(str)) return "";
  return (const char *)&js->mem[vdata(str) + sizeof(jsoff_t)];
}

static bool js_try_grow_memory(ant_t *js, size_t needed) {
  if (!js->owns_mem) return false;
  if (js->max_size == 0) return false;
  
  size_t current = (size_t)js->size;
  size_t required = current + needed;
  size_t new_mem_size = ((required + ARENA_GROW_INCREMENT - 1) / ARENA_GROW_INCREMENT) * ARENA_GROW_INCREMENT;
  
  if (new_mem_size > (size_t)js->max_size) new_mem_size = (size_t)js->max_size;
  if (new_mem_size <= current) return false;
  
  if (ant_arena_commit(js->mem, js->size, new_mem_size) != 0) return false;
  js->size = (jsoff_t)(new_mem_size / 8U * 8U);
  
  return true;
}

static inline bool js_has_space(ant_t *js, size_t size) {
  return js->brk + size <= js->size;
}

static bool js_ensure_space(ant_t *js, size_t size) {
  if (js_has_space(js, size)) return true;
  if (js_try_grow_memory(js, size) && js_has_space(js, size)) return true;

  js->needs_gc = true;

  if (js_has_space(js, size)) return true;
  if (js_try_grow_memory(js, size) && js_has_space(js, size)) return true;

  return false;
}

static void js_track_allocation(ant_t *js, size_t size) {
  js->brk += (jsoff_t) size;
  js->gc_alloc_since += (jsoff_t) size;
  
  jsoff_t threshold = js->brk / 2;
  if (threshold < 4 * 1024 * 1024) threshold = 4 * 1024 * 1024;
  if (js->gc_alloc_since > threshold) js->needs_gc = true;
}

static inline jsoff_t js_alloc(ant_t *js, size_t size) {
  size = align64((jsoff_t) size);
  if (!js_ensure_space(js, size)) return ~(jsoff_t) 0;

  jsoff_t ofs = js->brk;
  js_track_allocation(js, size);
  
  return ofs;
}

static jsoff_t dense_alloc(ant_t *js, jsoff_t capacity) {
  jsoff_t size = sizeof(jsoff_t) * 2 + sizeof(jsval_t) * capacity;
  jsoff_t off = js_alloc(js, size);
  if (off == (jsoff_t)~0) return 0;
  
  saveoff(js, off, capacity);
  saveoff(js, off + sizeof(jsoff_t), 0);
  
  for (jsoff_t i = 0; i < capacity; i++) saveval(
    js, off + sizeof(jsoff_t) * 2 + sizeof(jsval_t) * i, T_EMPTY
  );
  
  return off;
}

static inline jsoff_t get_dense_buf(ant_t *js, jsval_t arr) {
  jsoff_t off = (jsoff_t) vdata(js_as_obj(arr));
  if (__builtin_expect(off >= js->brk, 0)) return 0;
  jsoff_t next = loadoff(js, off) & ~(3U | FLAGMASK);
  if (__builtin_expect(next == 0 || next >= js->brk, 0)) return 0;
  jsoff_t header = loadoff(js, next);
  if ((header & SLOTMASK) &&
      loadoff(js, next + sizeof(jsoff_t)) == (jsoff_t)SLOT_DENSE_BUF) {
    jsval_t slot = loadval(js, next + sizeof(jsoff_t) * 2);
    return (jsoff_t) tod(slot);
  }
  jsval_t slot = get_slot(js, arr, SLOT_DENSE_BUF);
  if (vtype(slot) == T_UNDEF) return 0;
  return (jsoff_t) tod(slot);
}

static inline jsoff_t get_dense_buf_off(ant_t *js, jsoff_t obj_off) {
  return get_dense_buf(js, mkval(T_ARR, (uint64_t)obj_off));
}

static inline jsoff_t dense_capacity(ant_t *js, jsoff_t doff) {
  return loadoff(js, doff);
}

static inline jsoff_t dense_length(ant_t *js, jsoff_t doff) {
  return loadoff(js, doff + sizeof(jsoff_t));
}

static inline void dense_set_length(ant_t *js, jsoff_t doff, jsoff_t len) {
  saveoff(js, doff + sizeof(jsoff_t), len);
}

static inline jsval_t dense_get(ant_t *js, jsoff_t doff, jsoff_t idx) {
  return loadval(js, doff + sizeof(jsoff_t) * 2 + sizeof(jsval_t) * idx);
}

static inline void dense_set(ant_t *js, jsoff_t doff, jsoff_t idx, jsval_t val) {
  saveval(js, doff + sizeof(jsoff_t) * 2 + sizeof(jsval_t) * idx, val);
}

static jsoff_t dense_grow(ant_t *js, jsval_t arr, jsoff_t needed) {
  jsoff_t doff = get_dense_buf(js, arr);
  jsoff_t old_cap = doff ? dense_capacity(js, doff) : 0;
  jsoff_t old_len = doff ? dense_length(js, doff) : 0;
  jsoff_t new_cap = old_cap ? old_cap : MAX_DENSE_INITIAL_CAP;
  
  while (new_cap < needed) new_cap *= 2;
  jsoff_t new_doff = dense_alloc(js, new_cap);
  
  if (new_doff == 0) return 0;
  if (doff && old_len > 0) memcpy(
    &js->mem[new_doff + sizeof(jsoff_t) * 2],
    &js->mem[doff + sizeof(jsoff_t) * 2],
    sizeof(jsval_t) * old_len
  );
  
  dense_set_length(js, new_doff, old_len);
  set_slot(js, arr, SLOT_DENSE_BUF, tov((double)new_doff));
  
  return new_doff;
}

static inline jsval_t arr_get(ant_t *js, jsval_t arr, jsoff_t idx) {
  jsoff_t doff = get_dense_buf(js, arr);
  
  if (doff) {
    jsoff_t len = dense_length(js, doff);
    if (idx < len) {
      jsval_t v = dense_get(js, doff, idx);
      if (!is_empty_slot(v)) return v;
      return js_mkundef();
    }
  }
  
  char idxstr[16];
  size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
  jsoff_t prop = lkp(js, arr, idxstr, idxlen);
  
  return prop ? resolveprop(js, mkval(T_PROP, prop)) : js_mkundef();
}

static inline void arr_set(ant_t *js, jsval_t arr, jsoff_t idx, jsval_t val) {
  jsoff_t doff = get_dense_buf(js, arr);
  
  if (doff) {
    jsoff_t len = dense_length(js, doff);
    
    if (idx < len) {
      dense_set(js, doff, idx, val);
      return;
    }
    
    jsoff_t density_limit = len > 0 ? len * 4 : 64;
    if (idx >= density_limit) goto sparse;
    
    jsoff_t cap = dense_capacity(js, doff);
    if (idx >= cap) {
      doff = dense_grow(js, arr, idx + 1);
      if (doff == 0) goto sparse;
    }
    
    for (jsoff_t i = len; i < idx; i++) {
      jsval_t v = dense_get(js, doff, i);
      if (!is_empty_slot(v) && vtype(v) == T_UNDEF) dense_set(js, doff, i, T_EMPTY);
    }
    dense_set(js, doff, idx, val);
    dense_set_length(js, doff, idx + 1);
    return;
  }
  
  sparse:;
  char idxstr[24];
  size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (uint64_t)idx);
  jsval_t key = js_mkstr(js, idxstr, idxlen);
  
  js_setprop(js, arr, key, val);
}

static inline bool arr_has(ant_t *js, jsval_t arr, jsoff_t idx) {
  jsoff_t doff = get_dense_buf(js, arr);
  
  if (doff) {
    jsoff_t len = dense_length(js, doff);
    if (idx < len) return !is_empty_slot(dense_get(js, doff, idx));
  }
  
  char idxstr[16];
  size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
  return lkp(js, arr, idxstr, idxlen) != 0;
}

static inline void arr_del(ant_t *js, jsval_t arr, jsoff_t idx) {
  jsoff_t doff = get_dense_buf(js, arr);
  
  if (doff) {
    jsoff_t len = dense_length(js, doff);
    if (idx < len) dense_set(js, doff, idx, T_EMPTY);
    return;
  }
  
  char idxstr[16];
  uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
  js_del(js, arr, idxstr);
}

static jsval_t mkentity(ant_t *js, jsoff_t b, const void *buf, size_t len) {
  jsoff_t ofs = js_alloc(js, len + sizeof(b));
  if (ofs == (jsoff_t) ~0) return js_mkerr(js, "oom");
  
  memcpy(&js->mem[ofs], &b, sizeof(b));
  if (buf != NULL) {
    size_t copy_len = ((b & 3) == T_STR && len > 0) ? len - 1 : len;
    memmove(&js->mem[ofs + sizeof(b)], buf, copy_len);
  }
  
  if ((b & 3) == T_STR) js->mem[ofs + sizeof(b) + len - 1] = 0;
  return mkval(b & 3, ofs);
}

jsval_t js_mkstr(ant_t *js, const void *ptr, size_t len) {
  jsoff_t n = (jsoff_t) (len + 1);
  return mkentity(js, (jsoff_t) ((n << 3) | T_STR), ptr, n);
}

static jsval_t js_mkrope(ant_t *js, jsval_t left, jsval_t right, jsoff_t total_len, uint8_t depth) {
  jsoff_t ofs = js_alloc(js, sizeof(rope_node_t));
  if (ofs == (jsoff_t) ~0) return js_mkerr(js, "oom");
  
  jsoff_t header = ((total_len + 1) << 3) | T_STR | ROPE_FLAG | ((jsoff_t)depth << ROPE_DEPTH_SHIFT);
  jsval_t undef = js_mkundef();
  
  memcpy(&js->mem[ofs + offsetof(rope_node_t, header)], &header, sizeof(header));
  memcpy(&js->mem[ofs + offsetof(rope_node_t, left)], &left, sizeof(left));
  memcpy(&js->mem[ofs + offsetof(rope_node_t, right)], &right, sizeof(right));
  memcpy(&js->mem[ofs + offsetof(rope_node_t, cached)], &undef, sizeof(undef));
  
  return mkval(T_STR, ofs);
}

static bool bigint_parse_abs_u64(ant_t *js, jsval_t value, uint64_t *out) {
  size_t len = 0; const char *digits = bigint_digits(js, value, &len);
  uint64_t acc = 0;

  for (size_t i = 0; i < len; i++) {
    char c = digits[i];
    if (!is_digit(c)) return false;
    uint64_t digit = (uint64_t)(c - '0');
    if (acc > UINT64_MAX / 10 || (acc == UINT64_MAX / 10 && digit > (UINT64_MAX % 10))) {
      return false;
    } acc = acc * 10 + digit;
  }

  *out = acc;
  return true;
}

static bool bigint_IsNegative(ant_t *js, jsval_t v) {
  jsoff_t ofs = (jsoff_t) vdata(v);
  return js->mem[ofs + sizeof(jsoff_t)] == 1;
}

static bool bigint_parse_u64(ant_t *js, jsval_t value, uint64_t *out) {
  if (bigint_IsNegative(js, value)) return false;
  return bigint_parse_abs_u64(js, value, out);
}

jsval_t js_mkbigint(ant_t *js, const char *digits, size_t len, bool negative) {
  size_t total = len + 2;
  jsoff_t ofs = js_alloc(js, total + sizeof(jsoff_t));
  if (ofs == (jsoff_t) ~0) return js_mkerr(js, "oom");
  jsoff_t header = (jsoff_t) (total << 4);
  memcpy(&js->mem[ofs], &header, sizeof(header));
  js->mem[ofs + sizeof(header)] = negative ? 1 : 0;
  if (digits) memcpy(&js->mem[ofs + sizeof(header) + 1], digits, len);
  js->mem[ofs + sizeof(header) + 1 + len] = 0;
  return mkval(T_BIGINT, ofs);
}

static const char *bigint_digits(ant_t *js, jsval_t v, size_t *len) {
  jsoff_t ofs = (jsoff_t) vdata(v);
  jsoff_t header = loadoff(js, ofs);
  size_t total = (header >> 4) - 2;
  if (len) *len = total;
  return (const char *)&js->mem[ofs + sizeof(jsoff_t) + 1];
}

static int bigint_cmp_abs(const char *a, size_t alen, const char *b, size_t blen) {
  while (alen > 1 && a[0] == '0') { a++; alen--; }
  while (blen > 1 && b[0] == '0') { b++; blen--; }
  if (alen != blen) return alen > blen ? 1 : -1;
  for (size_t i = 0; i < alen; i++) {
    if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1;
  }
  return 0;
}

static char *bigint_add_abs(const char *a, size_t alen, const char *b, size_t blen, size_t *rlen) {
  size_t maxlen = (alen > blen ? alen : blen) + 1;
  char *result = (char *)malloc(maxlen + 1);
  if (!result) return NULL;
  int carry = 0;
  size_t ri = 0;
  for (size_t i = 0; i < maxlen; i++) {
    int da = (i < alen) ? (a[alen - 1 - i] - '0') : 0;
    int db = (i < blen) ? (b[blen - 1 - i] - '0') : 0;
    int sum = da + db + carry;
    carry = sum / 10;
    result[ri++] = (char)('0' + (sum % 10));
  }
  while (ri > 1 && result[ri - 1] == '0') ri--;
  for (size_t i = 0; i < ri / 2; i++) {
    char tmp = result[i]; result[i] = result[ri - 1 - i]; result[ri - 1 - i] = tmp;
  }
  result[ri] = 0;
  *rlen = ri;
  return result;
}

static char *bigint_sub_abs(const char *a, size_t alen, const char *b, size_t blen, size_t *rlen) {
  char *result = (char *)malloc(alen + 1);
  if (!result) return NULL;
  int borrow = 0;
  size_t ri = 0;
  for (size_t i = 0; i < alen; i++) {
    int da = a[alen - 1 - i] - '0';
    int db = (i < blen) ? (b[blen - 1 - i] - '0') : 0;
    int diff = da - db - borrow;
    if (diff < 0) { diff += 10; borrow = 1; } else { borrow = 0; }
    result[ri++] = (char)('0' + diff);
  }
  while (ri > 1 && result[ri - 1] == '0') ri--;
  for (size_t i = 0; i < ri / 2; i++) {
    char tmp = result[i]; result[i] = result[ri - 1 - i]; result[ri - 1 - i] = tmp;
  }
  result[ri] = 0;
  *rlen = ri;
  return result;
}

static char *bigint_mul_abs(const char *a, size_t alen, const char *b, size_t blen, size_t *rlen) {
  size_t reslen = alen + blen;
  int *temp = (int *)calloc(reslen, sizeof(int));
  if (!temp) return NULL;
  for (size_t i = 0; i < alen; i++) {
    for (size_t j = 0; j < blen; j++) {
      temp[i + j] += (a[alen - 1 - i] - '0') * (b[blen - 1 - j] - '0');
    }
  }
  for (size_t i = 0; i < reslen - 1; i++) {
    temp[i + 1] += temp[i] / 10;
    temp[i] %= 10;
  }
  size_t start = reslen - 1;
  while (start > 0 && temp[start] == 0) start--;
  char *result = (char *)malloc(start + 2);
  if (!result) { free(temp); return NULL; }
  for (size_t i = 0; i <= start; i++) result[i] = (char)('0' + temp[start - i]);
  result[start + 1] = 0;
  *rlen = start + 1;
  free(temp);
  return result;
}

static char *bigint_div_abs(const char *a, size_t alen, const char *b, size_t blen, size_t *rlen, char **rem, size_t *remlen) {
  if (blen == 1 && b[0] == '0') return NULL;
  if (bigint_cmp_abs(a, alen, b, blen) < 0) {
    char *result = (char *)malloc(2); result[0] = '0'; result[1] = 0; *rlen = 1;
    if (rem) { *rem = (char *)malloc(alen + 1); memcpy(*rem, a, alen); (*rem)[alen] = 0; *remlen = alen; }
    return result;
  }
  char *current = (char *)calloc(alen + 1, 1);
  char *result = (char *)calloc(alen + 1, 1);
  if (!current || !result) { free(current); free(result); return NULL; }
  size_t curlen = 0, reslen = 0;
  for (size_t i = 0; i < alen; i++) {
    if (curlen == 1 && current[0] == '0') curlen = 0;
    current[curlen++] = a[i]; current[curlen] = 0;
    int count = 0;
    while (bigint_cmp_abs(current, curlen, b, blen) >= 0) {
      size_t sublen;
      char *sub = bigint_sub_abs(current, curlen, b, blen, &sublen);
      if (!sub) break;
      memcpy(current, sub, sublen + 1); curlen = sublen;
      free(sub); count++;
    }
    result[reslen++] = (char)('0' + count);
  }
  size_t start = 0;
  while (start < reslen - 1 && result[start] == '0') start++;
  memmove(result, result + start, reslen - start + 1);
  *rlen = reslen - start;
  if (rem) { *rem = current; *remlen = curlen; } else free(current);
  return result;
}

jsval_t bigint_add(ant_t *js, jsval_t a, jsval_t b) {
  bool aneg = bigint_IsNegative(js, a), bneg = bigint_IsNegative(js, b);
  size_t alen, blen;
  const char *ad = bigint_digits(js, a, &alen), *bd = bigint_digits(js, b, &blen);
  char *result; size_t rlen; bool rneg;
  if (aneg == bneg) {
    result = bigint_add_abs(ad, alen, bd, blen, &rlen); rneg = aneg;
  } else {
    int cmp = bigint_cmp_abs(ad, alen, bd, blen);
    if (cmp >= 0) { result = bigint_sub_abs(ad, alen, bd, blen, &rlen); rneg = aneg; }
    else { result = bigint_sub_abs(bd, blen, ad, alen, &rlen); rneg = bneg; }
  }
  if (!result) return js_mkerr(js, "oom");
  if (rlen == 1 && result[0] == '0') rneg = false;
  jsval_t r = js_mkbigint(js, result, rlen, rneg);
  free(result);
  return r;
}

jsval_t bigint_sub(ant_t *js, jsval_t a, jsval_t b) {
  bool aneg = bigint_IsNegative(js, a), bneg = bigint_IsNegative(js, b);
  size_t alen, blen;
  const char *ad = bigint_digits(js, a, &alen), *bd = bigint_digits(js, b, &blen);
  char *result; size_t rlen; bool rneg;
  if (aneg != bneg) {
    result = bigint_add_abs(ad, alen, bd, blen, &rlen); rneg = aneg;
  } else {
    int cmp = bigint_cmp_abs(ad, alen, bd, blen);
    if (cmp >= 0) { result = bigint_sub_abs(ad, alen, bd, blen, &rlen); rneg = aneg; }
    else { result = bigint_sub_abs(bd, blen, ad, alen, &rlen); rneg = !aneg; }
  }
  if (!result) return js_mkerr(js, "oom");
  if (rlen == 1 && result[0] == '0') rneg = false;
  jsval_t r = js_mkbigint(js, result, rlen, rneg);
  free(result);
  return r;
}

jsval_t bigint_mul(ant_t *js, jsval_t a, jsval_t b) {
  bool aneg = bigint_IsNegative(js, a), bneg = bigint_IsNegative(js, b);
  size_t alen, blen;
  const char *ad = bigint_digits(js, a, &alen), *bd = bigint_digits(js, b, &blen);
  size_t rlen;
  char *result = bigint_mul_abs(ad, alen, bd, blen, &rlen);
  if (!result) return js_mkerr(js, "oom");
  bool rneg = (aneg != bneg) && !(rlen == 1 && result[0] == '0');
  jsval_t r = js_mkbigint(js, result, rlen, rneg);
  free(result);
  return r;
}

jsval_t bigint_div(ant_t *js, jsval_t a, jsval_t b) {
  bool aneg = bigint_IsNegative(js, a), bneg = bigint_IsNegative(js, b);
  size_t alen, blen;
  const char *ad = bigint_digits(js, a, &alen), *bd = bigint_digits(js, b, &blen);
  if (blen == 1 && bd[0] == '0') return js_mkerr(js, "Division by zero");
  size_t rlen;
  char *result = bigint_div_abs(ad, alen, bd, blen, &rlen, NULL, NULL);
  if (!result) return js_mkerr(js, "oom");
  bool rneg = (aneg != bneg) && !(rlen == 1 && result[0] == '0');
  jsval_t r = js_mkbigint(js, result, rlen, rneg);
  free(result);
  return r;
}

jsval_t bigint_mod(ant_t *js, jsval_t a, jsval_t b) {
  bool aneg = bigint_IsNegative(js, a);
  size_t alen, blen;
  const char *ad = bigint_digits(js, a, &alen), *bd = bigint_digits(js, b, &blen);
  if (blen == 1 && bd[0] == '0') return js_mkerr(js, "Division by zero");
  size_t rlen, remlen; char *rem;
  char *result = bigint_div_abs(ad, alen, bd, blen, &rlen, &rem, &remlen);
  if (!result) return js_mkerr(js, "oom");
  free(result);
  bool rneg = aneg && !(remlen == 1 && rem[0] == '0');
  jsval_t r = js_mkbigint(js, rem, remlen, rneg);
  free(rem);
  return r;
}

jsval_t bigint_neg(ant_t *js, jsval_t a) {
  size_t len;
  const char *digits = bigint_digits(js, a, &len);
  bool neg = bigint_IsNegative(js, a);
  if (len == 1 && digits[0] == '0') return js_mkbigint(js, digits, len, false);
  return js_mkbigint(js, digits, len, !neg);
}

jsval_t bigint_exp(ant_t *js, jsval_t base, jsval_t exp) {
  if (bigint_IsNegative(js, exp)) return js_mkerr(js, "Exponent must be positive");
  size_t explen;
  const char *expd = bigint_digits(js, exp, &explen);
  if (explen == 1 && expd[0] == '0') return js_mkbigint(js, "1", 1, false);
  jsval_t result = js_mkbigint(js, "1", 1, false);
  jsval_t b = base;
  jsval_t e = exp;
  jsval_t two = js_mkbigint(js, "2", 1, false);
  while (true) {
    size_t elen;
    const char *ed = bigint_digits(js, e, &elen);
    if (elen == 1 && ed[0] == '0') break;
    int last_digit = ed[elen - 1] - '0';
    if (last_digit % 2 == 1) {
      result = bigint_mul(js, result, b);
      if (is_err(result)) return result;
    }
    b = bigint_mul(js, b, b);
    if (is_err(b)) return b;
    e = bigint_div(js, e, two);
    if (is_err(e)) return e;
  }
  return result;
}

static inline jsval_t bigint_pow2(ant_t *js, uint64_t bits) {
  jsval_t two = js_mkbigint(js, "2", 1, false);
  if (is_err(two)) return two;
  jsval_t exp = bigint_from_u64(js, bits);
  if (is_err(exp)) return exp;
  return bigint_exp(js, two, exp);
}

jsval_t bigint_shift_left(ant_t *js, jsval_t value, uint64_t shift) {
  if (shift == 0) return value;
  if (shift > 18446744073709551615ULL) return js_mkerr(js, "Shift count too large");

  size_t digits_len; const char *digits = bigint_digits(js, value, &digits_len);
  if (digits_len == 1 && digits[0] == '0') return js_mkbigint(js, "0", 1, false);
  uint64_t u64 = 0;
  if (!bigint_IsNegative(js, value) && shift < 64 && bigint_parse_u64(js, value, &u64)) {
    if (u64 <= (UINT64_MAX >> shift)) return bigint_from_u64(js, u64 << shift);
  }

  jsval_t pow = bigint_pow2(js, shift);
  if (is_err(pow)) return pow;
  return bigint_mul(js, value, pow);
}

jsval_t bigint_shift_right(ant_t *js, jsval_t value, uint64_t shift) {
  if (shift == 0) return value;
  if (shift > 18446744073709551615ULL) return js_mkerr(js, "Shift count too large");

  size_t digits_len; const char *digits = bigint_digits(js, value, &digits_len);
  if (digits_len == 1 && digits[0] == '0') return js_mkbigint(js, "0", 1, false);
  uint64_t u64 = 0;
  if (!bigint_IsNegative(js, value) && bigint_parse_u64(js, value, &u64)) {
    if (shift >= 64) return js_mkbigint(js, "0", 1, false);
    return bigint_from_u64(js, u64 >> shift);
  }

  if (bigint_parse_abs_u64(js, value, &u64)) {
    if (shift >= 64) return js_mkbigint(
      js, bigint_IsNegative(js, value) ? "1" : "0", 1, 
      bigint_IsNegative(js, value)
    );
    uint64_t shifted = u64 >> shift;
    if (bigint_IsNegative(js, value)) {
      if ((u64 & ((1ULL << shift) - 1)) != 0) shifted += 1;
      jsval_t pos = bigint_from_u64(js, shifted);
      if (is_err(pos)) return pos;
      return bigint_neg(js, pos);
    }
    return bigint_from_u64(js, shifted);
  }

  jsval_t pow = bigint_pow2(js, shift);
  if (is_err(pow)) return pow;
  return bigint_div(js, value, pow);
}

jsval_t bigint_shift_right_logical(ant_t *js, jsval_t value, uint64_t shift) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "BigInts have no unsigned right shift, use >> instead");
}

int bigint_compare(ant_t *js, jsval_t a, jsval_t b) {
  bool aneg = bigint_IsNegative(js, a), bneg = bigint_IsNegative(js, b);
  size_t alen, blen;
  const char *ad = bigint_digits(js, a, &alen), *bd = bigint_digits(js, b, &blen);
  if (aneg && !bneg) return -1;
  if (!aneg && bneg) return 1;
  int cmp = bigint_cmp_abs(ad, alen, bd, blen);
  return aneg ? -cmp : cmp;
}

static bool bigint_is_zero(ant_t *js, jsval_t v) {
  size_t len;
  const char *digits = bigint_digits(js, v, &len);
  return len == 1 && digits[0] == '0';
}

static size_t strbigint(ant_t *js, jsval_t value, char *buf, size_t len) {
  bool neg = bigint_IsNegative(js, value);
  size_t dlen;
  const char *digits = bigint_digits(js, value, &dlen);
  size_t n = 0;
  if (neg) n += cpy(buf + n, REMAIN(n, len), "-", 1);
  n += cpy(buf + n, REMAIN(n, len), digits, dlen);
  return n;
}

static jsval_t builtin_BigInt(ant_t *js, jsval_t *args, int nargs) {
  if (vtype(js->new_target) != T_UNDEF) return js_mkerr_typed(js, JS_ERR_TYPE, "BigInt is not a constructor");
  if (nargs < 1) return js_mkbigint(js, "0", 1, false);
  
  jsval_t arg = args[0];
  if (vtype(arg) == T_BIGINT) return arg;
  if (vtype(arg) == T_NUM) {
    double d = tod(arg);
    if (!isfinite(d)) return js_mkerr(js, "Cannot convert Infinity or NaN to BigInt");
    if (d != trunc(d)) return js_mkerr(js, "Cannot convert non-integer to BigInt");
    bool neg = d < 0;
    if (neg) d = -d;
    char buf[64];
    snprintf(buf, sizeof(buf), "%.0f", d);
    return js_mkbigint(js, buf, strlen(buf), neg);
  }
  if (vtype(arg) == T_STR) {
    jsoff_t slen, off = vstr(js, arg, &slen);
    const char *str = (const char *)&js->mem[off];
    bool neg = false;
    size_t i = 0;
    if (slen > 0 && str[0] == '-') { neg = true; i++; }
    else if (slen > 0 && str[0] == '+') { i++; }
    while (i < slen && str[i] == '0') i++;
    if (i >= slen) return js_mkbigint(js, "0", 1, false);
    for (size_t j = i; j < slen; j++) {
      if (!is_digit(str[j])) return js_mkerr(js, "Cannot convert string to BigInt");
    }
    return js_mkbigint(js, str + i, slen - i, neg);
  }
  if (vtype(arg) == T_BOOL) {
    return js_mkbigint(js, vdata(arg) ? "1" : "0", 1, false);
  }
  return js_mkerr(js, "Cannot convert to BigInt");
}

static jsval_t bigint_to_u64(ant_t *js, jsval_t value, uint64_t *out) {
  if (!bigint_parse_u64(js, value, out)) {
    return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid bits");
  }
  return js_mkundef();
}

jsval_t bigint_asint_bits(ant_t *js, jsval_t arg, uint64_t *bits_out) {
  if (vtype(arg) == T_BIGINT) {
    return bigint_to_u64(js, arg, bits_out);
  }
  double bits = js_to_number(js, arg);
  if (!isfinite(bits) || bits < 0 || bits != floor(bits)) {
    return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid bits");
  }
  if (bits > 18446744073709551615.0) {
    return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid bits");
  }
  *bits_out = (uint64_t)bits;
  return js_mkundef();
}

static jsval_t builtin_BigInt_asIntN(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "BigInt.asIntN requires 2 arguments");
  uint64_t bits = 0;
  jsval_t err = bigint_asint_bits(js, args[0], &bits);
  if (is_err(err)) return err;
  if (vtype(args[1]) != T_BIGINT) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert to BigInt");
  }
  if (bits == 0) return js_mkbigint(js, "0", 1, false);

  jsval_t mod = bigint_pow2(js, bits);
  if (is_err(mod)) return mod;
  jsval_t res = bigint_mod(js, args[1], mod);
  if (is_err(res)) return res;
  if (bigint_IsNegative(js, res)) {
    jsval_t adj = bigint_add(js, res, mod);
    if (is_err(adj)) return adj;
    res = adj;
  }

  jsval_t threshold = bigint_pow2(js, bits - 1);
  if (is_err(threshold)) return threshold;
  if (bigint_compare(js, res, threshold) >= 0) {
    jsval_t adj = bigint_sub(js, res, mod);
    if (is_err(adj)) return adj;
    res = adj;
  }
  return res;
}

static jsval_t builtin_BigInt_asUintN(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "BigInt.asUintN requires 2 arguments");
  uint64_t bits = 0;
  jsval_t err = bigint_asint_bits(js, args[0], &bits);
  if (is_err(err)) return err;
  if (vtype(args[1]) != T_BIGINT) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert to BigInt");
  }
  if (bits == 0) return js_mkbigint(js, "0", 1, false);

  jsval_t mod = bigint_pow2(js, bits);
  if (is_err(mod)) return mod;
  jsval_t res = bigint_mod(js, args[1], mod);
  if (is_err(res)) return res;
  if (bigint_IsNegative(js, res)) {
    jsval_t adj = bigint_add(js, res, mod);
    if (is_err(adj)) return adj;
    res = adj;
  }
  return res;
}

static jsval_t builtin_bigint_toString(ant_t *js, jsval_t *args, int nargs) {
  jsval_t val = js->this_val;
  if (vtype(val) != T_BIGINT) return js_mkerr(js, "toString called on non-BigInt");
  
  int radix = 10;
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    radix = (int)tod(args[0]);
    if (radix < 2 || radix > 36) return js_mkerr(js, "radix must be between 2 and 36");
  }
  
  bool neg = bigint_IsNegative(js, val);
  size_t dlen; const char *digits = bigint_digits(js, val, &dlen);
  
  if (radix == 10) {
    size_t buflen = dlen + 2;
    char *buf = (char *)ant_calloc(buflen);
    if (!buf) return js_mkerr(js, "oom");
    size_t n = 0; if (neg) buf[n++] = '-';
    memcpy(buf + n, digits, dlen); n += dlen;
    jsval_t ret = js_mkstr(js, buf, n); free(buf);
    return ret;
  }
  
  const uint32_t base = 1000000000U;
  size_t result_cap = dlen * 4 + 16;
  
  char *result = (char *)ant_calloc(result_cap);
  if (!result) return js_mkerr(js, "oom");
  
  size_t rpos = result_cap - 1;
  result[rpos] = '\0';

  size_t limb_cap = (dlen + 8) / 9 + 1;
  uint32_t *limbs = (uint32_t *)ant_calloc(limb_cap * sizeof(uint32_t));
  if (!limbs) { free(result); return js_mkerr(js, "oom"); }
  size_t limb_len = 1;

  for (size_t i = 0; i < dlen; i++) {
    uint64_t carry = (uint64_t)(digits[i] - '0');
    for (size_t j = 0; j < limb_len; j++) {
      uint64_t cur = (uint64_t)limbs[j] * 10 + carry;
      limbs[j] = (uint32_t)(cur % base);
      carry = cur / base;
    }
    if (carry != 0) {
      if (limb_len == limb_cap) {
        size_t new_cap = limb_cap * 2;
        uint32_t *new_limbs = (uint32_t *)ant_realloc(limbs, new_cap * sizeof(uint32_t));
        if (!new_limbs) { free(limbs); free(result); return js_mkerr(js, "oom"); }
        limbs = new_limbs;
        limb_cap = new_cap;
      }
      limbs[limb_len++] = (uint32_t)carry;
    }
  }

  static const char digit_map[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  while (limb_len > 0 && !(limb_len == 1 && limbs[0] == 0)) {
    uint64_t remainder = 0;
    for (size_t i = limb_len; i-- > 0;) {
      uint64_t cur = (uint64_t)limbs[i] + remainder * base;
      limbs[i] = (uint32_t)(cur / (uint64_t)radix);
      remainder = cur % (uint64_t)radix;
    }
    
    while (limb_len > 0 && limbs[limb_len - 1] == 0) limb_len--;
    if (rpos == 0) {
      size_t new_cap = result_cap * 2;
      char *new_result = (char *)ant_calloc(new_cap);
      if (!new_result) { free(limbs); free(result); return js_mkerr(js, "oom"); }
      
      size_t used = result_cap - rpos;
      memcpy(new_result + new_cap - used, result + rpos, used);
      free(result);
      
      result = new_result;
      rpos = new_cap - used;
      result_cap = new_cap;
    }
    result[--rpos] = digit_map[remainder];
  }

  free(limbs);
  
  if (rpos == result_cap - 1) result[--rpos] = '0';  
  if (neg) result[--rpos] = '-';
  
  jsval_t ret = js_mkstr(js, result + rpos, result_cap - 1 - rpos);
  free(result); return ret;
}

jsval_t mkobj(ant_t *js, jsoff_t parent) {
  jsoff_t buf[2] = { parent, 0 };
  return mkentity(js, 0 | T_OBJ, buf, sizeof(buf));
}

jsval_t mkarr(ant_t *js) {
  jsval_t arr = mkobj(js, 0);
  jsoff_t off = (jsoff_t) vdata(arr);
  jsoff_t header = loadoff(js, off);
  
  saveoff(js, off, header | ARRMASK);
  jsval_t array_proto = get_ctor_proto(js, "Array", 5);
  if (vtype(array_proto) == T_OBJ) set_proto(js, arr, array_proto);
  
  jsval_t arr_val = mkval(T_ARR, vdata(arr));
  jsoff_t doff = dense_alloc(js, MAX_DENSE_INITIAL_CAP);
  if (doff) set_slot(js, arr_val, SLOT_DENSE_BUF, tov((double)doff));
  
  return arr_val;
}

jsval_t js_mkarr(ant_t *js) { 
  return mkarr(js); 
}

jsval_t js_newobj(ant_t *js) {
  jsval_t obj = mkobj(js, 0);
  jsval_t proto = get_ctor_proto(js, "Object", 6);
  if (vtype(proto) == T_OBJ) set_proto(js, obj, proto);
  return obj;
}

jsoff_t js_arr_len(ant_t *js, jsval_t arr) {
  if (vtype(arr) != T_ARR) return 0;
  return get_array_length(js, arr);
}

jsval_t js_arr_get(ant_t *js, jsval_t arr, jsoff_t idx) {
  if (vtype(arr) != T_ARR) return js_mkundef();
  return arr_get(js, arr, idx);
}

static inline bool is_const_prop(ant_t *js, jsoff_t propoff) {
  jsoff_t v = loadoff(js, propoff);
  return (v & CONSTMASK) != 0;
}

static inline bool is_nonconfig_prop(ant_t *js, jsoff_t propoff) {
  jsoff_t v = loadoff(js, propoff);
  return (v & NONCONFIGMASK) != 0;
}

static void intern_init(void) {
  if (INTERN_LENGTH) return;
  INTERN_LENGTH = intern_string("length", 6);
  INTERN_BUFFER = intern_string("buffer", 6);
  INTERN_PROTOTYPE = intern_string("prototype", 9);
  INTERN_CONSTRUCTOR = intern_string("constructor", 11);
  INTERN_NAME = intern_string("name", 4);
  INTERN_MESSAGE = intern_string("message", 7);
  INTERN_VALUE = intern_string("value", 5);
  INTERN_GET = intern_string("get", 3);
  INTERN_SET = intern_string("set", 3);
  INTERN_ARGUMENTS = intern_string("arguments", 9);
  INTERN_CALLEE = intern_string("callee", 6);
  INTERN_IDX[0] = intern_string("0", 1);
  INTERN_IDX[1] = intern_string("1", 1);
  INTERN_IDX[2] = intern_string("2", 1);
  INTERN_IDX[3] = intern_string("3", 1);
  INTERN_IDX[4] = intern_string("4", 1);
  INTERN_IDX[5] = intern_string("5", 1);
  INTERN_IDX[6] = intern_string("6", 1);
  INTERN_IDX[7] = intern_string("7", 1);
  INTERN_IDX[8] = intern_string("8", 1);
  INTERN_IDX[9] = intern_string("9", 1);
}

static void invalidate_prop_cache(ant_t *js, jsoff_t obj_off, jsoff_t prop_off) {
  jsoff_t koff = loadoff(js, prop_off + sizeof(jsoff_t));
  jsoff_t klen = (loadoff(js, koff) >> 3) - 1;
  
  const char *key = (char *)&js->mem[koff + sizeof(jsoff_t)];
  const char *interned = intern_string(key, klen);
  if (!interned) return;
  
  uint32_t cache_slot = (((uintptr_t)interned >> 3) ^ obj_off) & (ANT_LIMIT_SIZE_CACHE - 1);
  intern_prop_cache_entry_t *ce = &intern_prop_cache[cache_slot];
  if (ce->obj_off == obj_off && ce->intern_ptr == interned) ce->generation = 0;
}

jsval_t mkprop(ant_t *js, jsval_t obj, jsval_t k, jsval_t v, jsoff_t flags) {
  obj = js_as_obj(obj);
  jsoff_t koff_entity;
  
  if (vtype(k) == T_SYMBOL) {
    koff_entity = (jsoff_t)vdata(k);
  } else {
    jsoff_t klen; jsoff_t koff = vstr(js, k, &klen);
    koff_entity = koff - sizeof(jsoff_t);
    const char *p = (char *) &js->mem[koff];
    (void)intern_string(p, klen);
  }
  
  jsoff_t head = (jsoff_t) vdata(obj);
  char buf[sizeof(koff_entity) + sizeof(v)];
  
  jsoff_t header = loadoff(js, head);
  jsoff_t first_prop = header & ~(3U | FLAGMASK);
  jsoff_t tail = loadoff(js, head + sizeof(jsoff_t) + sizeof(jsoff_t));
  
  memcpy(buf, &koff_entity, sizeof(koff_entity));
  memcpy(buf + sizeof(koff_entity), &v, sizeof(v));
  
  jsoff_t new_prop_off = js->brk;
  jsval_t prop = mkentity(js, 0 | T_PROP | flags, buf, sizeof(buf));
  if (is_err(prop)) return prop;
  
  if (first_prop == 0) {
    jsoff_t new_header = new_prop_off | (header & (3U | FLAGMASK));
    saveoff(js, head, new_header);
  } else {
    jsoff_t tail_header = loadoff(js, tail);
    jsoff_t new_tail_header = new_prop_off | (tail_header & (3U | FLAGMASK));
    saveoff(js, tail, new_tail_header);
  }
  saveoff(js, head + sizeof(jsoff_t) + sizeof(jsoff_t), new_prop_off);
  
  return prop;
}

static inline jsval_t mkprop_fast(ant_t *js, jsval_t obj, jsval_t k, jsval_t v, jsoff_t flags) {
  obj = js_as_obj(obj);
  jsoff_t koff_entity;
  if (vtype(k) == T_SYMBOL) {
    koff_entity = (jsoff_t)vdata(k);
  } else {
    jsoff_t klen; jsoff_t koff = vstr(js, k, &klen);
    koff_entity = koff - sizeof(jsoff_t);
  }
  jsoff_t head = (jsoff_t) vdata(obj);
  char buf[sizeof(koff_entity) + sizeof(v)];
  
  jsoff_t header = loadoff(js, head);
  jsoff_t first_prop = header & ~(3U | FLAGMASK);
  jsoff_t tail = loadoff(js, head + sizeof(jsoff_t) + sizeof(jsoff_t));
  
  memcpy(buf, &koff_entity, sizeof(koff_entity));
  memcpy(buf + sizeof(koff_entity), &v, sizeof(v));
  
  jsoff_t new_prop_off = js->brk;
  jsval_t prop = mkentity(js, 0 | T_PROP | flags, buf, sizeof(buf));
  if (is_err(prop)) return prop;
  
  if (first_prop == 0) {
    jsoff_t new_header = new_prop_off | (header & (3U | FLAGMASK));
    saveoff(js, head, new_header);
  } else {
    jsoff_t tail_header = loadoff(js, tail);
    jsoff_t new_tail_header = new_prop_off | (tail_header & (3U | FLAGMASK));
    saveoff(js, tail, new_tail_header);
  }
  saveoff(js, head + sizeof(jsoff_t) + sizeof(jsoff_t), new_prop_off);
  
  return prop;
}

jsval_t js_mkprop_fast(ant_t *js, jsval_t obj, const char *key, size_t len, jsval_t v) {
  jsval_t k = js_mkstr(js, key, len);
  if (is_err(k)) return k;
  return mkprop_fast(js, obj, k, v, 0);
}

jsoff_t js_mkprop_fast_off(ant_t *js, jsval_t obj, const char *key, size_t len, jsval_t v) {
  jsval_t k = js_mkstr(js, key, len);
  if (is_err(k)) return 0;
  jsoff_t prop_off = js->brk;
  mkprop_fast(js, obj, k, v, 0);
  return prop_off + sizeof(jsoff_t) * 2;
}

void js_saveval(ant_t *js, jsoff_t off, jsval_t v) { saveval(js, off, v); }

static jsval_t mkslot(ant_t *js, jsval_t obj, internal_slot_t slot, jsval_t v) {
  obj = js_as_obj(obj);
  jsoff_t head = (jsoff_t) vdata(obj);
  char buf[sizeof(jsoff_t) + sizeof(v)];
  
  jsoff_t header = loadoff(js, head);
  jsoff_t first_prop = header & ~(3U | FLAGMASK);
  
  jsoff_t slot_key = (jsoff_t)slot;
  memcpy(buf, &slot_key, sizeof(slot_key));
  memcpy(buf + sizeof(slot_key), &v, sizeof(v));
  
  jsoff_t new_prop_off = js->brk;
  jsval_t prop = mkentity(js, first_prop | T_PROP | SLOTMASK, buf, sizeof(buf));
  if (is_err(prop)) return prop;
  
  jsoff_t new_header = new_prop_off | (header & (3U | FLAGMASK));
  saveoff(js, head, new_header);
  
  if (first_prop == 0) {
    saveoff(js, head + sizeof(jsoff_t) + sizeof(jsoff_t), new_prop_off);
  }
  
  return prop;
}

static inline jsoff_t search_slot(ant_t *js, jsval_t obj, internal_slot_t slot) {
  obj = js_as_obj(obj);
  jsoff_t off = (jsoff_t) vdata(obj);
  if (__builtin_expect(off >= js->brk, 0)) return 0;
  jsoff_t next = loadoff(js, off) & ~(3U | FLAGMASK);
  while (next != 0 && next < js->brk) {
    jsoff_t header = loadoff(js, next);
    if ((header & SLOTMASK) == 0) return 0;
    jsoff_t koff = loadoff(js, next + sizeof(jsoff_t));
    if (koff == (jsoff_t)slot) return next;
    next = header & ~(3U | FLAGMASK);
  }
  return 0;
}

static void set_slot(ant_t *js, jsval_t obj, internal_slot_t slot, jsval_t val) {
  jsoff_t existing = search_slot(js, obj, slot);
  if (existing > 0) {
    saveval(js, existing + sizeof(jsoff_t) * 2, val);
  } else mkslot(js, obj, slot, val);
}

static jsval_t get_slot(ant_t *js, jsval_t obj, internal_slot_t slot) {
  jsoff_t off = search_slot(js, obj, slot);
  if (off == 0) return js_mkundef();
  return loadval(js, off + sizeof(jsoff_t) * 2);
}

static void set_func_code_ptr(ant_t *js, jsval_t func_obj, const char *code, size_t len) {
  set_slot(js, func_obj, SLOT_CODE, mkval(T_CFUNC, (size_t)code));
  set_slot(js, func_obj, SLOT_CODE_LEN, tov((double)len));
}

static void set_func_code(ant_t *js, jsval_t func_obj, const char *code, size_t len) {
  const char *arena_code = code_arena_alloc(code, len);
  if (!arena_code) return;
  set_func_code_ptr(js, func_obj, arena_code, len);
  
  if (!memmem(code, len, "var", 3)) return;
  
  size_t vars_buf_len;
  char *vars = OXC_get_func_hoisted_vars(code, len, &vars_buf_len);
  
  if (vars) {
    set_slot(js, func_obj, SLOT_HOISTED_VARS, mkval(T_CFUNC, (size_t)vars));
    set_slot(js, func_obj, SLOT_HOISTED_VARS_LEN, tov((double)vars_buf_len));
  }
}

static const char *get_func_code(ant_t *js, jsval_t func_obj, jsoff_t *len) {
  jsval_t code_val = get_slot(js, func_obj, SLOT_CODE);
  jsval_t len_val = get_slot(js, func_obj, SLOT_CODE_LEN);
  
  if (vtype(code_val) != T_CFUNC) {
    if (len) *len = 0;
    return NULL;
  }
  
  if (len) *len = (jsoff_t)tod(len_val);
  return (const char *)vdata(code_val);
}

static inline bool is_slot_prop(jsoff_t header) {
  return (header & SLOTMASK) != 0;
}

static inline jsoff_t next_prop(jsoff_t header) {
  return header & ~(3U | FLAGMASK);
}

double js_to_number(ant_t *js, jsval_t arg) {
  if (vtype(arg) == T_NUM) return tod(arg);
  if (vtype(arg) == T_BOOL) return vdata(arg) ? 1.0 : 0.0;
  if (vtype(arg) == T_NULL) return 0.0;
  if (vtype(arg) == T_UNDEF) return JS_NAN;
  
  if (vtype(arg) == T_STR) {
    jsoff_t len, off = vstr(js, arg, &len);
    const char *s = (char *)&js->mem[off], *end;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (!*s) return 0.0;
    double val = strtod(s, (char **)&end);
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') end++;
    return (end == s || *end) ? JS_NAN : val;
  }
  
  if (vtype(arg) == T_OBJ || vtype(arg) == T_ARR) {
    if (vtype(arg) == T_OBJ) {
      jsval_t prim = js_call_valueOf(js, arg);
      uint8_t pt = vtype(prim);
      if (pt != T_OBJ && pt != T_ARR && pt != T_FUNC) return js_to_number(js, prim);
    }
    
    jsval_t str_val = js_tostring_val(js, arg);
    if (is_err(str_val) || vtype(str_val) != T_STR) return JS_NAN;
    return js_to_number(js, str_val);
  }
  
  return JS_NAN;
}

static jsval_t setup_func_prototype(ant_t *js, jsval_t func) {
  jsval_t proto_obj = mkobj(js, 0);
  if (is_err(proto_obj)) return proto_obj;
  
  jsval_t object_proto = get_ctor_proto(js, "Object", 6);
  if (vtype(object_proto) == T_OBJ) {
    set_proto(js, proto_obj, object_proto);
  }
  
  jsval_t constructor_key = js_mkstr(js, "constructor", 11);
  if (is_err(constructor_key)) return constructor_key;
  
  jsval_t res = mkprop(js, proto_obj, constructor_key, func, 0);
  if (is_err(res)) return res;
  js_set_descriptor(js, proto_obj, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  jsval_t prototype_key = js_mkstr(js, "prototype", 9);
  if (is_err(prototype_key)) return prototype_key;
  
  res = js_setprop(js, func, prototype_key, proto_obj);
  if (is_err(res)) return res;
  js_set_descriptor(js, func, "prototype", 9, JS_DESC_W);
  
  return js_mkundef();
}

static inline bool proto_walk_next(ant_t *js, jsval_t *cur, uint8_t *t, uint8_t flags) {
  uint8_t ct = *t;

  if (flags & PROTO_WALK_F_OBJECT_ONLY) {
    if (!is_object_type(*cur)) return false;
    jsval_t next = get_proto(js, *cur);
    uint8_t nt = vtype(next);
    if (nt == T_NULL || nt == T_UNDEF || !is_object_type(next)) return false;
    *cur = next; *t = nt;
    return true;
  }

  if (ct == T_OBJ || ct == T_ARR || ct == T_FUNC || ct == T_PROMISE) {
    jsval_t as_obj = js_as_obj(*cur);
    jsval_t proto = get_slot(js, as_obj, SLOT_PROTO);
    
    uint8_t pt = vtype(proto);
    if (pt == T_OBJ || pt == T_ARR || pt == T_FUNC) {
      *cur = proto;
      *t = pt;
      return true;
    }
    
    if (JS_TYPE_FLAG(ct) & T_NEEDS_PROTO_FALLBACK) {
      jsval_t fallback = get_prototype_for_type(js, ct);
      uint8_t ft = vtype(fallback);
      if (ft == T_NULL || ft == T_UNDEF) return false;
      *cur = fallback;
      *t = ft;
      return true;
    }
    
    return false;
  }

  if (ct == T_STR || ct == T_NUM || ct == T_BOOL || ct == T_BIGINT || ct == T_SYMBOL) {
    jsval_t proto = get_prototype_for_type(js, ct);
    uint8_t pt = vtype(proto);
    if (pt == T_NULL || pt == T_UNDEF) return false;
    *cur = proto; *t = pt;
    return true;
  }

  return false;
}

jsval_t js_instance_proto_from_new_target(ant_t *js, jsval_t fallback_proto) {
  jsval_t instance_proto = js_mkundef();
  
  if (vtype(js->new_target) == T_FUNC || vtype(js->new_target) == T_CFUNC) {
    jsval_t nt_obj = js_as_obj(js->new_target);
    jsoff_t nt_proto_off = lkp_interned(js, nt_obj, INTERN_PROTOTYPE, 9);
    if (nt_proto_off != 0) {
      jsval_t nt_proto = resolveprop(js, mkval(T_PROP, nt_proto_off));
      if (is_object_type(nt_proto)) instance_proto = nt_proto;
    }
  }
  
  if (!is_object_type(instance_proto) && is_object_type(fallback_proto)) {
    instance_proto = fallback_proto;
  } return instance_proto;
}

bool proto_chain_contains(ant_t *js, jsval_t obj, jsval_t proto_target) {
  if (!is_object_type(obj) || !is_object_type(proto_target)) return false;
  jsval_t cur = obj; uint8_t t = vtype(cur);
  for (int depth = 0; depth < MAX_PROTO_CHAIN_DEPTH; depth++) {
    if (!proto_walk_next(js, &cur, &t, PROTO_WALK_F_OBJECT_ONLY)) break;
    if (vdata(cur) == vdata(proto_target)) return true;
  }
  return false;
}

static inline bool is_wrapper_ctor_target(ant_t *js, jsval_t this_val, jsval_t expected_proto) {
  if (vtype(js->new_target) == T_UNDEF) return false;
  if (vtype(this_val) != T_OBJ) return false;
  if (vtype(get_slot(js, this_val, SLOT_PRIMITIVE)) != T_UNDEF) return false;
  return proto_chain_contains(js, this_val, expected_proto);
}

jsval_t get_ctor_species_value(ant_t *js, jsval_t ctor) {
  if (!is_object_type(ctor) && vtype(ctor) != T_CFUNC) return js_mkundef();
  return js_get_sym(js, ctor, get_species_sym());
}

bool same_ctor_identity(ant_t *js, jsval_t a, jsval_t b) {
  if (vtype(a) == vtype(b) && vdata(a) == vdata(b)) return true;
  
  if (vtype(a) == T_FUNC && vtype(b) == T_CFUNC) {
    jsval_t c = get_slot(js, a, SLOT_CFUNC);
    return vtype(c) == T_CFUNC && vdata(c) == vdata(b);
  }
  
  if (vtype(a) == T_CFUNC && vtype(b) == T_FUNC) {
    jsval_t c = get_slot(js, b, SLOT_CFUNC);
    return vtype(c) == T_CFUNC && vdata(c) == vdata(a);
  }
  
  if (vtype(a) == T_FUNC && vtype(b) == T_FUNC) {
    jsval_t ca = get_slot(js, a, SLOT_CFUNC);
    jsval_t cb = get_slot(js, b, SLOT_CFUNC);
    if (vtype(ca) == T_CFUNC && vtype(cb) == T_CFUNC && vdata(ca) == vdata(cb)) return true;
  }
  
  return false;
}

static jsval_t array_constructor_from_receiver(ant_t *js, jsval_t receiver) {
  if (!is_object_type(receiver)) return js_mkundef();
  
  jsval_t species_source = receiver;
  if (is_proxy(js, species_source)) {
    species_source = proxy_read_target(js, species_source);
  }
  
  bool receiver_is_array = (vtype(species_source) == T_ARR);
  if (!receiver_is_array) {
    jsval_t array_proto = get_ctor_proto(js, "Array", 5);
    if (is_object_type(array_proto) && is_object_type(species_source)) {
      receiver_is_array = proto_chain_contains(js, species_source, array_proto);
    }
  }
  if (!receiver_is_array) return js_mkundef();

  jsval_t ctor = js_getprop_fallback(js, receiver, "constructor");
  if (is_err(ctor)) return ctor;

  jsval_t species = get_ctor_species_value(js, ctor);
  if (is_err(species)) return species;
  
  if (vtype(species) == T_NULL) return js_mkundef();
  if (vtype(species) == T_FUNC || vtype(species) == T_CFUNC) return species;
  if (vtype(ctor) != T_FUNC && vtype(ctor) != T_CFUNC) return js_mkundef();
  
  return ctor;
}

static jsval_t array_alloc_from_ctor_with_length(ant_t *js, jsval_t ctor, jsoff_t length_hint) {
  if (vtype(ctor) != T_FUNC && vtype(ctor) != T_CFUNC) {
    return mkarr(js);
  }

  jsval_t seed = js_mkobj(js);
  if (is_err(seed)) return seed;

  jsval_t proto = js_get(js, ctor, "prototype");
  if (is_err(proto)) return proto;
  if (is_object_type(proto)) set_proto(js, seed, proto);

  jsval_t ctor_args[1] = { tov((double)length_hint) };
  jsval_t saved_new_target = js->new_target;
  js->new_target = ctor;
  jsval_t constructed = sv_vm_call(js->vm, js, ctor, seed, ctor_args, 1, NULL, true);
  js->new_target = saved_new_target;
  if (is_err(constructed)) return constructed;

  jsval_t result = is_object_type(constructed) ? constructed : seed;
  set_slot(js, js_as_obj(result), SLOT_CTOR, ctor);
  return result;
}

static inline jsval_t array_alloc_from_ctor(ant_t *js, jsval_t ctor) {
  return array_alloc_from_ctor_with_length(js, ctor, 0);
}

static inline jsval_t array_alloc_like(ant_t *js, jsval_t receiver) {
  jsval_t ctor = array_constructor_from_receiver(js, receiver);
  if (is_err(ctor)) return ctor;
  return array_alloc_from_ctor(js, ctor);
}

static jsval_t validate_array_length(ant_t *js, jsval_t v) {
  if (vtype(v) != T_NUM) {
    return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid array length");
  }
  double d = tod(v);
  if (d < 0 || d != (uint32_t)d || d >= 4294967296.0) {
    return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid array length");
  }
  return js_mkundef();
}

static inline jsval_t check_object_extensibility(ant_t *js, jsval_t obj) {
  obj = js_as_obj(obj);
  jsoff_t obj_off_fp = (jsoff_t)vdata(obj);
  jsoff_t next = loadoff(js, obj_off_fp) & ~(3U | FLAGMASK);
  
  while (next != 0 && next < js->brk) {
    jsoff_t hdr = loadoff(js, next);
    if ((hdr & SLOTMASK) == 0) break;
    
    jsoff_t sk = loadoff(js, next + sizeof(jsoff_t));
    jsval_t sv = loadval(js, next + sizeof(jsoff_t) * 2);
    
    if (sk == (jsoff_t)SLOT_FROZEN && js_truthy(js, sv)) {
      return sv_vm_is_strict(js->vm)
        ? js_mkerr(js, "cannot add property to frozen object")
        : js_mkundef();
    }
    
    if (sk == (jsoff_t)SLOT_SEALED && js_truthy(js, sv)) {
      return sv_vm_is_strict(js->vm)
        ? js_mkerr(js, "cannot add property to sealed object")
        : js_mkundef();
    }
    
    if (sk == (jsoff_t)SLOT_EXTENSIBLE && !js_truthy(js, sv)) {
      return sv_vm_is_strict(js->vm)
        ? js_mkerr(js, "cannot add property to non-extensible object")
        : js_mkundef();
    }
    
    next = hdr & ~(3U | FLAGMASK);
  }
  
  return js_mkundef();
}

static inline void update_array_length(ant_t *js, jsval_t obj, jsoff_t new_len) {
  jsoff_t len_off = lkp_interned(js, obj, INTERN_LENGTH, 6);
  jsval_t new_len_val = tov((double)new_len);
  
  if (len_off != 0) saveval(js, len_off + sizeof(jsoff_t) * 2, new_len_val); else {
    js_mkprop_fast(js, obj, "length", 6, new_len_val);
  }
}

static jsval_t js_setprop_array_fast(ant_t *js, jsval_t obj, jsval_t k, jsval_t v, jsoff_t klen, const char *key) {
  unsigned long idx;
  if (!parse_array_index(key, klen, (jsoff_t)-1, &idx)) return js_mkundef();
  
  jsoff_t doff = get_dense_buf(js, obj);
  if (doff) {
    jsoff_t cur_len = dense_length(js, doff);
    if (idx < cur_len) { dense_set(js, doff, (jsoff_t)idx, v); return v; }

    jsoff_t density_limit = cur_len > 0 ? cur_len * 4 : 64;
    if (idx >= density_limit) goto sparse;
    
    jsval_t extensibility_error = check_object_extensibility(js, obj);
    if (!is_undefined(extensibility_error)) return extensibility_error;
    
    arr_set(js, obj, (jsoff_t)idx, v);
    return v;
  }
  
  sparse:;
  jsoff_t cur_len = get_array_length(js, obj);
  if (idx < cur_len) return js_mkundef();
  
  jsval_t extensibility_error = check_object_extensibility(js, obj);
  if (!is_undefined(extensibility_error)) return extensibility_error;
  
  jsval_t result = mkprop_fast(js, obj, k, v, 0);
  update_array_length(js, obj, idx + 1);
  
  return result;
}

jsval_t js_setprop(ant_t *js, jsval_t obj, jsval_t k, jsval_t v) {
  uint8_t ot = vtype(obj);

  if (ot == T_STR || ot == T_NUM || ot == T_BOOL) {
    jsoff_t klen; jsoff_t koff = vstr(js, k, &klen);
    const char *key = (char *)&js->mem[koff];
    jsval_t proto = get_prototype_for_type(js, ot);
    if (is_object_type(proto)) {
      jsval_t setter = js_mkundef();
      bool has_setter = false;
      lkp_with_setter(js, proto, key, klen, &setter, &has_setter);
      if (has_setter && (vtype(setter) == T_FUNC || vtype(setter) == T_CFUNC)) {
        call_proto_accessor(js, obj, setter, true, &v, 1, true);
        return v;
      }
    }
    if (sv_vm_is_strict(js->vm))
      return js_mkerr_typed(js, JS_ERR_TYPE,
        "Cannot create property '%.*s' on %s",
        (int)klen, key, typestr(ot));
    return v;
  }

  if (vtype(obj) == T_FUNC) obj = js_func_obj(obj);
  if (vtype(k) == T_SYMBOL) {
    jsoff_t sym_off = (jsoff_t)vdata(k);
    jsval_t cur = obj;
    
    for (int i = 0; i < MAX_PROTO_CHAIN_DEPTH; i++) {
      jsoff_t cur_off = (jsoff_t)vdata(js_as_obj(cur));
      descriptor_entry_t *sd = lookup_sym_descriptor(cur_off, sym_off);
      if (sd && sd->has_setter) {
        jsval_t setter = sd->setter;
        if (vtype(setter) == T_FUNC || vtype(setter) == T_CFUNC) {
          jsval_t result = sv_vm_call(js->vm, js, setter, obj, &v, 1, NULL, false);
          if (is_err(result)) return result;
          return v;
        }
      }
      
      if (sd && sd->has_getter && !sd->has_setter) {
        if (sv_vm_is_strict(js->vm)) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot set property which has only a getter");
        return v;
      }
      
      jsval_t proto = get_proto(js, js_as_obj(cur));
      if (!is_object_type(proto)) break;
      cur = proto;
    }

    jsoff_t existing = lkp_sym(js, obj, sym_off);
    
    if (existing > 0) {
      if (is_const_prop(js, existing)) return js_mkerr(js, "assignment to constant");
      saveval(js, existing + sizeof(jsoff_t) * 2, v);
      return mkval(T_PROP, existing);
    }
    
    if (js_truthy(js, get_slot(js, obj, SLOT_FROZEN))) {
      if (sv_vm_is_strict(js->vm)) return js_mkerr(js, "cannot add property to frozen object");
      return js_mkundef();
    }
    
    return mkprop(js, obj, k, v, 0);
  }

  jsoff_t klen; jsoff_t koff = vstr(js, k, &klen);
  const char *key = (char *) &js->mem[koff];
  
  if (vtype(obj) == T_ARR && !is_proxy(js, obj) && klen > 0 && key[0] >= '0' && key[0] <= '9') {
    jsval_t result = js_setprop_array_fast(js, obj, k, v, klen, key);
    if (vtype(result) != T_UNDEF) return result;
  }

  if (vtype(obj) == T_ARR && streq(key, klen, "length", 6)) {
    jsval_t err = validate_array_length(js, v);
    if (is_err(err)) return err;
    jsoff_t doff = get_dense_buf(js, obj);
    if (doff) {
      jsoff_t new_len_val = (jsoff_t) tod(v);
      jsoff_t cur_len = dense_length(js, doff);
      if (new_len_val < cur_len) {
        for (jsoff_t i = new_len_val; i < cur_len; i++)
          dense_set(js, doff, i, T_EMPTY);
        dense_set_length(js, doff, new_len_val);
        return v;
      } else if (new_len_val <= dense_capacity(js, doff)) {
        dense_set_length(js, doff, new_len_val);
        return v;
      }
    }
  }
  
  if (is_proxy(js, obj)) {
    jsval_t result = proxy_set(js, obj, key, klen, v);
    if (is_err(result)) return result;
    return v;
  }
  
  if (try_dynamic_setter(js, obj, key, klen, v)) return v;
  jsoff_t existing = lkp(js, obj, key, klen);
  
  {
    jsoff_t obj_off = (jsoff_t)vdata(js_as_obj(obj));
    descriptor_entry_t *desc = lookup_descriptor(js, obj_off, key, klen);

    if (!desc) {
      jsval_t cur = obj;
      for (int i = 0; i < MAX_PROTO_CHAIN_DEPTH && !desc; i++) {
        cur = get_proto(js, cur);
        if (vtype(cur) != T_OBJ && vtype(cur) != T_FUNC) break;
        desc = lookup_descriptor(js, (jsoff_t)vdata(js_as_obj(cur)), key, klen);
      }
      if (desc && !desc->has_setter && !desc->has_getter && desc->writable) desc = NULL;
    }

    if (!desc) goto no_descriptor;
    
    if (desc->has_setter) {
      jsval_t setter = desc->setter;
      uint8_t setter_type = vtype(setter);
      if (setter_type == T_FUNC || setter_type == T_CFUNC) {
        js_error_site_t saved_errsite = js->errsite;
        jsval_t result = sv_vm_call(js->vm, js, setter, obj, &v, 1, NULL, false);
        js->errsite = saved_errsite;
        if (is_err(result)) return result;
        return v;
      }
    }
    
    if (desc->has_getter && !desc->has_setter) {
      if (sv_vm_is_strict(js->vm)) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot set property which has only a getter");
      return v;
    }
    
    if (!desc->writable) {
      if (sv_vm_is_strict(js->vm)) return js_mkerr(js, "assignment to read-only property");
      return existing > 0 ? mkval(T_PROP, existing) : v;
    }
    
    if (existing <= 0) goto no_descriptor;
  }
  
no_descriptor:
  if (existing <= 0) goto create_new;
  if (is_const_prop(js, existing)) return js_mkerr(js, "assignment to constant");

  saveval(js, existing + sizeof(jsoff_t) * 2, v);
  if (vtype(obj) != T_ARR || klen == 0 || key[0] < '0' || key[0] > '9') goto done_update;
  { jsoff_t doff = get_dense_buf(js, obj); if (doff) goto done_update; }
  
  char *endptr;
  unsigned long update_idx = strtoul(key, &endptr, 10);
  if (endptr != key + klen) goto done_update;
  
  jsoff_t len_off = lkp_interned(js, obj, INTERN_LENGTH, 6);
  jsoff_t cur_len = 0;
  if (len_off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, len_off));
    if (vtype(len_val) == T_NUM) cur_len = (jsoff_t) tod(len_val);
  }
  if (update_idx < cur_len) goto done_update;
  
  jsval_t new_len = tov((double)(update_idx + 1));
  if (len_off != 0) saveval(js, len_off + sizeof(jsoff_t) * 2, new_len); else {
    mkprop(js, obj, js->length_str, new_len, 0);
  }

done_update:
  return mkval(T_PROP, existing);

create_new:
  if (js_truthy(js, get_slot(js, obj, SLOT_FROZEN))) {
    if (sv_vm_is_strict(js->vm)) return js_mkerr(js, "cannot add property to frozen object");
    return js_mkundef();
  }
  
  if (js_truthy(js, get_slot(js, obj, SLOT_SEALED))) {
    if (sv_vm_is_strict(js->vm)) return js_mkerr(js, "cannot add property to sealed object");
    return js_mkundef();
  }
  
  jsval_t ext_slot = get_slot(js, obj, SLOT_EXTENSIBLE);
  if (vtype(ext_slot) != T_UNDEF && !js_truthy(js, ext_slot)) {
    if (sv_vm_is_strict(js->vm)) return js_mkerr(js, "cannot add property to non-extensible object");
    return js_mkundef();
  }
  
  int need_length_update = 0;
  unsigned long idx = 0;
  
  if (vtype(obj) == T_ARR && klen > 0 && key[0] >= '0' && key[0] <= '9') {
    char *inner_endptr;
    idx = strtoul(key, &inner_endptr, 10);
    if (inner_endptr == key + klen) {
      jsoff_t inner_len_off = lkp_interned(js, obj, INTERN_LENGTH, 6);
      jsoff_t inner_cur_len = 0;
      if (inner_len_off != 0) {
        jsval_t len_val = resolveprop(js, mkval(T_PROP, inner_len_off));
        if (vtype(len_val) == T_NUM) inner_cur_len = (jsoff_t) tod(len_val);
      }
      if (idx >= inner_cur_len) need_length_update = 1;
    }
  }
  
  jsval_t result = mkprop(js, obj, k, v, 0);
  if (need_length_update) {
    jsoff_t inner_len_off = lkp_interned(js, obj, INTERN_LENGTH, 6);
    jsval_t inner_new_len = tov((double)(idx + 1));
    if (inner_len_off != 0) saveval(js, inner_len_off + sizeof(jsoff_t) * 2, inner_new_len); else {
      mkprop(js, obj, js->length_str, inner_new_len, 0);
    }
  }
  
  return result;
}

jsval_t setprop_cstr(ant_t *js, jsval_t obj, const char *key, size_t len, jsval_t v) {
  obj = js_as_obj(obj);
  jsval_t k = js_mkstr(js, key, len);
  if (is_err(k)) return k;
  return mkprop(js, obj, k, v, 0);
}

jsval_t js_define_own_prop(ant_t *js, jsval_t obj, const char *key, size_t klen, jsval_t v) {
  obj = js_as_obj(obj);
  if (is_proxy(js, obj)) {
    jsval_t result = proxy_set(js, obj, key, klen, v);
    if (is_err(result)) return result;
    return v;
  }

  if (try_dynamic_setter(js, obj, key, klen, v)) return v;
  jsoff_t existing = lkp(js, obj, key, klen);

  {
    jsoff_t obj_off = (jsoff_t)vdata(js_as_obj(obj));
    descriptor_entry_t *desc = lookup_descriptor(js, obj_off, key, klen);
    if (desc) {
      if (!desc->writable && !desc->has_setter) {
        if (sv_vm_is_strict(js->vm)) return js_mkerr(js, "assignment to read-only property");
        return v;
      }
      if (desc->has_setter) {
        jsval_t setter = desc->setter;
        uint8_t setter_type = vtype(setter);
        if (setter_type == T_FUNC || setter_type == T_CFUNC) {
          jsval_t result = sv_vm_call(js->vm, js, setter, obj, &v, 1, NULL, false);
          if (is_err(result)) return result;
          return v;
        }
      }
    }
  }

  if (existing > 0) {
    if (is_const_prop(js, existing)) return js_mkerr(js, "assignment to constant");
    saveval(js, existing + sizeof(jsoff_t) * 2, v);
    return mkval(T_PROP, existing);
  }

  if (js_truthy(js, get_slot(js, obj, SLOT_FROZEN))) {
    if (sv_vm_is_strict(js->vm)) return js_mkerr(js, "cannot add property to frozen object");
    return js_mkundef();
  }
  if (js_truthy(js, get_slot(js, obj, SLOT_SEALED))) {
    if (sv_vm_is_strict(js->vm)) return js_mkerr(js, "cannot add property to sealed object");
    return js_mkundef();
  }
  jsval_t ext_slot = get_slot(js, obj, SLOT_EXTENSIBLE);
  if (vtype(ext_slot) != T_UNDEF && !js_truthy(js, ext_slot)) {
    if (sv_vm_is_strict(js->vm)) return js_mkerr(js, "cannot add property to non-extensible object");
    return js_mkundef();
  }

  jsval_t k = js_mkstr(js, key, klen);
  if (is_err(k)) return k;
  return mkprop(js, obj, k, v, 0);
}

jsval_t setprop_interned(ant_t *js, jsval_t obj, const char *key, size_t len, jsval_t v) {
  jsval_t k = js_mkstr(js, key, len);
  if (is_err(k)) return k;
  return js_setprop(js, obj, k, v);
}

jsval_t js_setprop_nonconfigurable(ant_t *js, jsval_t obj, const char *key, size_t keylen, jsval_t v) {
  jsval_t k = js_mkstr(js, key, keylen);
  if (is_err(k)) return k;
  jsval_t result = js_setprop(js, obj, k, v);
  if (is_err(result)) return result;
  
  js_set_descriptor(js, obj, key, keylen, JS_DESC_W);
  return result;
}

#define SYM_HEAP_FIXED   (sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uintptr_t))
#define SYM_FLAG_GLOBAL  1u
#define SYM_REGISTRY_MAX 256

static struct { 
  const char *key;
  uint32_t sym_id;
} g_sym_registry[SYM_REGISTRY_MAX];

static size_t  g_sym_registry_count = 0;
static size_t  g_sym_all_count      = 0;
static size_t  g_sym_all_cap        = 0;
static jsval_t *g_sym_all = NULL;

static void sym_table_add(jsval_t sym) {
  if (g_sym_all_count >= g_sym_all_cap) {
    size_t new_cap = g_sym_all_cap ? g_sym_all_cap * 2 : 64;
    jsval_t *new_buf = realloc(g_sym_all, new_cap * sizeof(jsval_t));
    if (!new_buf) return;
    g_sym_all = new_buf;
    g_sym_all_cap = new_cap;
  }
  g_sym_all[g_sym_all_count++] = sym;
}

jsval_t js_mksym(ant_t *js, const char *desc) {
  uint32_t id = (uint32_t)(++js->sym_counter);
  size_t desc_len = (desc && *desc) ? strlen(desc) : 0;
  size_t payload = SYM_HEAP_FIXED + (desc_len ? desc_len + 1 : 0);

  jsoff_t ofs = js_alloc(js, payload + sizeof(jsoff_t));
  if (ofs == (jsoff_t)~0) return js_mkerr(js, "oom");

  jsoff_t header = (jsoff_t)(payload << 4);
  memcpy(&js->mem[ofs], &header, sizeof(header));

  jsoff_t p = ofs + sizeof(jsoff_t);
  memcpy(&js->mem[p], &id, sizeof(id));
  p += sizeof(uint32_t);
  uint32_t flags = 0;
  memcpy(&js->mem[p], &flags, sizeof(flags));
  p += sizeof(uint32_t);
  uintptr_t key_ptr = 0;
  memcpy(&js->mem[p], &key_ptr, sizeof(key_ptr));
  p += sizeof(uintptr_t);

  if (desc_len) {
    memcpy(&js->mem[p], desc, desc_len);
    js->mem[p + desc_len] = '\0';
  }

  jsval_t sym = mkval(T_SYMBOL, ofs);
  sym_table_add(sym);
  return sym;
}

static inline uint32_t sym_get_id(ant_t *js, jsval_t v) {
  jsoff_t ofs = (jsoff_t)vdata(v);
  uint32_t id;
  memcpy(&id, &js->mem[ofs + sizeof(jsoff_t)], sizeof(id));
  return id;
}

static inline uint32_t sym_get_flags(ant_t *js, jsval_t v) {
  jsoff_t ofs = (jsoff_t)vdata(v);
  uint32_t flags;
  memcpy(&flags, &js->mem[ofs + sizeof(jsoff_t) + sizeof(uint32_t)], sizeof(flags));
  return flags;
}

static inline uintptr_t sym_get_key_ptr(ant_t *js, jsval_t v) {
  jsoff_t ofs = (jsoff_t)vdata(v);
  uintptr_t kp;
  memcpy(&kp, &js->mem[ofs + sizeof(jsoff_t) + sizeof(uint32_t) * 2], sizeof(kp));
  return kp;
}

static const char *sym_get_desc(ant_t *js, jsval_t v) {
  jsoff_t ofs = (jsoff_t)vdata(v);
  jsoff_t header;
  memcpy(&header, &js->mem[ofs], sizeof(header));
  size_t payload = (size_t)(header >> 4);
  if (payload <= SYM_HEAP_FIXED) return NULL;
  return (const char *)&js->mem[ofs + sizeof(jsoff_t) + SYM_HEAP_FIXED];
}

uint64_t inline js_sym_id(jsval_t sym) {
  // TODO: fix to not use the global runtime
  ant_t *js = rt->js;
  return sym_get_id(js, sym);
}

jsval_t js_mksym_for(ant_t *js, const char *key) {
  const char *interned = intern_string(key, strlen(key));

  for (size_t i = 0; i < g_sym_registry_count; i++) {
    if (g_sym_registry[i].key == interned) {
      uint32_t id = g_sym_registry[i].sym_id;
      if (id - 1 < g_sym_all_count) return g_sym_all[id - 1];
    }
  }

  jsval_t sym = js_mksym(js, key);
  if (is_err(sym)) return sym;

  jsoff_t ofs = (jsoff_t)vdata(sym);
  uint32_t flags = SYM_FLAG_GLOBAL;
  memcpy(&js->mem[ofs + sizeof(jsoff_t) + sizeof(uint32_t)], &flags, sizeof(flags));
  uintptr_t kp = (uintptr_t)interned;
  memcpy(&js->mem[ofs + sizeof(jsoff_t) + sizeof(uint32_t) * 2], &kp, sizeof(kp));

  if (g_sym_registry_count < SYM_REGISTRY_MAX) {
    uint32_t id = sym_get_id(js, sym);
    g_sym_registry[g_sym_registry_count].key = interned;
    g_sym_registry[g_sym_registry_count].sym_id = id;
    g_sym_registry_count++;
  }

  return sym;
}

const char *js_sym_key(jsval_t sym) {
  if (vtype(sym) != T_SYMBOL) return NULL;
  ant_t *js = rt->js;
  if (!(sym_get_flags(js, sym) & SYM_FLAG_GLOBAL)) return NULL;
  return (const char *)sym_get_key_ptr(js, sym);
}

const inline char *js_sym_desc(ant_t *js, jsval_t sym) {
  return sym_get_desc(js, sym);
}

jsval_t sym_lookup_by_id(uint32_t id) {
  if (id == 0 || id - 1 >= g_sym_all_count) return (jsval_t)0;
  return g_sym_all[id - 1];
}

void sym_gc_update_all(void (*op_val)(void *, jsval_t *), void *ctx) {
  for (size_t i = 0; i < g_sym_all_count; i++) {
    op_val(ctx, &g_sym_all[i]);
  }
}

jsoff_t esize(jsoff_t w) {
  jsoff_t cleaned = w & ~FLAGMASK;
  switch (cleaned & 3U) {
    case T_OBJ:  return (jsoff_t) (sizeof(jsoff_t) + sizeof(jsoff_t) + sizeof(jsoff_t));
    case T_PROP: return (jsoff_t) (sizeof(jsoff_t) + sizeof(jsoff_t) + sizeof(jsval_t));
    case T_STR:  return (jsoff_t) (sizeof(jsoff_t) + align64(cleaned >> 3U));
    default:     return (jsoff_t) ~0U;
  }
}

static inline bool streq(const char *buf, size_t len, const char *s, size_t n) {
  return len == n && !memcmp(buf, s, n);
}

static inline jsoff_t lkp_interned(ant_t *js, jsval_t obj, const char *search_intern, size_t len) {
  obj = js_as_obj(obj);
  jsoff_t obj_off = (jsoff_t)vdata(obj);
  jsoff_t tail = loadoff(js, obj_off + sizeof(jsoff_t) * 2);
  
  uint32_t slot = (((uintptr_t)search_intern >> 3) ^ obj_off) & (ANT_LIMIT_SIZE_CACHE - 1);
  intern_prop_cache_entry_t *ce = &intern_prop_cache[slot];
  
  if (ce->generation == intern_prop_cache_gen 
    && ce->obj_off == obj_off 
    && ce->intern_ptr == search_intern
    && ce->tail == tail
  ) return ce->prop_off;
  
  jsoff_t first_prop = loadoff(js, obj_off) & ~(3U | FLAGMASK);
  jsoff_t off = first_prop; jsoff_t result = 0;
  
  while (off < js->brk && off != 0) {
    jsoff_t header = loadoff(js, off);
    if (is_slot_prop(header)) { off = next_prop(header); continue; }
    
    jsoff_t koff = loadoff(js, off + sizeof(jsoff_t));
    jsoff_t klen = (loadoff(js, koff) >> 3) - 1;
    
    if (klen == len) {
      const char *p = (char *)&js->mem[koff + sizeof(jsoff_t)];
      if (intern_string(p, klen) == search_intern) { result = off; break; }
    }
    off = next_prop(header);
  }
  
  ce->generation = intern_prop_cache_gen;
  ce->obj_off = obj_off;
  ce->intern_ptr = search_intern;
  ce->prop_off = result;
  ce->tail = tail;
  
  return result;
}

inline jsoff_t lkp(ant_t *js, jsval_t obj, const char *buf, size_t len) {
  const char *search_intern = intern_string(buf, len);
  if (!search_intern) return 0;
  return lkp_interned(js, obj, search_intern, len);
}

jsoff_t lkp_sym(ant_t *js, jsval_t obj, jsoff_t sym_off) {
  obj = js_as_obj(obj);
  jsoff_t obj_off = (jsoff_t)vdata(obj);
  jsoff_t first_prop = loadoff(js, obj_off) & ~(3U | FLAGMASK);
  jsoff_t off = first_prop;
  
  while (off < js->brk && off != 0) {
    jsoff_t header = loadoff(js, off);
    if (is_slot_prop(header)) { off = next_prop(header); continue; }
    jsoff_t koff = loadoff(js, off + sizeof(jsoff_t));
    if (koff == sym_off) return off;
    off = next_prop(header);
  }
  return 0;
}

jsoff_t lkp_sym_proto(ant_t *js, jsval_t obj, jsoff_t sym_off) {
  for (int i = 0; i < MAX_PROTO_CHAIN_DEPTH; i++) {
    jsoff_t off = lkp_sym(js, obj, sym_off);
    if (off != 0) return off;
    jsval_t proto = get_proto(js, js_as_obj(obj));
    if (!is_object_type(proto)) break;
    obj = proto;
  }
  return 0;
}

static jsval_t *resolve_bound_args(ant_t *js, jsval_t func_obj, jsval_t *args, int nargs, int *out_nargs) {
  *out_nargs = nargs;
  
  jsval_t bound_arr = get_slot(js, func_obj, SLOT_BOUND_ARGS);
  int bound_argc = 0;
  
  if (vtype(bound_arr) == T_ARR) {
    bound_argc = (int)get_array_length(js, bound_arr);
  } if (bound_argc <= 0) return NULL;
  
  *out_nargs = bound_argc + nargs;
  jsval_t *combined = (jsval_t *)ant_calloc(sizeof(jsval_t) * (*out_nargs));
  if (!combined) return NULL;
  
  for (int i = 0; i < bound_argc; i++) combined[i] = arr_get(js, bound_arr, (jsoff_t)i);
  for (int i = 0; i < nargs; i++) combined[bound_argc + i] = args[i];
  
  return combined;
}

static jsoff_t lkp_with_getter(ant_t *js, jsval_t obj, const char *buf, size_t len, jsval_t *getter_out, bool *has_getter_out) {
  *has_getter_out = false;
  *getter_out = js_mkundef();
  
  for (jsval_t current = obj; is_object_type(current); ) {
    current = js_as_obj(current);
    jsoff_t current_off = (jsoff_t)vdata(current);
    descriptor_entry_t *desc = lookup_descriptor(js, current_off, buf, len);
    
    if (desc && desc->has_getter) {
      *getter_out = desc->getter;
      *has_getter_out = true;
      return current_off;
    }
    
    jsoff_t prop_off = lkp_interned(js, current, intern_string(buf, len), len);
    if (prop_off != 0) return prop_off;
    
    jsval_t proto = get_proto(js, current);
    if (!is_object_type(proto)) break;
    current = proto;
  }
  
  return 0;
}

static jsoff_t lkp_with_setter(ant_t *js, jsval_t obj, const char *buf, size_t len, jsval_t *setter_out, bool *has_setter_out) {
  *has_setter_out = false;
  *setter_out = js_mkundef();
  
  jsval_t current = obj;
  while (vtype(current) == T_OBJ || vtype(current) == T_FUNC) {
    current = js_as_obj(current);
    jsoff_t current_off = (jsoff_t)vdata(current);
    descriptor_entry_t *desc = lookup_descriptor(js, current_off, buf, len);
    
    if (desc && desc->has_setter) {
      *setter_out = desc->setter;
      *has_setter_out = true;
      return current_off;
    }
    
    jsoff_t prop_off = lkp_interned(js, current, intern_string(buf, len), len);
    if (prop_off != 0) return prop_off;
    
    jsval_t proto = get_proto(js, current);
    if (vtype(proto) != T_OBJ && vtype(proto) != T_FUNC) break;
    current = proto;
  }
  
  return 0;
}

static jsval_t call_proto_accessor(ant_t *js, jsval_t prim, jsval_t accessor, bool has_accessor, jsval_t *arg, int arg_count, bool is_setter) {
  if (!has_accessor || (vtype(accessor) != T_FUNC && vtype(accessor) != T_CFUNC)) return js_mkundef();
  
  js_error_site_t saved_errsite = js->errsite;
  jsval_t result = sv_vm_call(js->vm, js, accessor, prim, arg, arg_count, NULL, false);
  
  bool had_throw = js->thrown_exists;
  jsval_t thrown = js->thrown_value;
  js->errsite = saved_errsite;
  
  if (had_throw) {
    js->thrown_exists = true;
    js->thrown_value = thrown;
  }
  
  if (is_setter) return is_err(result) ? result : (arg ? *arg : js_mkundef());
  return result;
}

jsval_t js_get_proto(ant_t *js, jsval_t obj) {
  uint8_t t = vtype(obj);

  if (!is_object_type(obj)) return js_mknull();
  jsval_t as_obj = js_as_obj(obj);
  
  jsval_t proto = get_slot(js, as_obj, SLOT_PROTO);
  if (is_object_type(proto)) return proto;
  
  if (t != T_OBJ) return get_prototype_for_type(js, t);
  return js_mknull();
}

static jsval_t get_proto(ant_t *js, jsval_t obj) {
  return js_get_proto(js, obj);
}

void js_set_proto(ant_t *js, jsval_t obj, jsval_t proto) {
  if (!is_object_type(obj)) return;
  
  jsval_t as_obj = js_as_obj(obj);
  set_slot(js, as_obj, SLOT_PROTO, proto);
}

static void set_proto(ant_t *js, jsval_t obj, jsval_t proto) {
  js_set_proto(js, obj, proto);
}

jsval_t js_get_ctor_proto(ant_t *js, const char *name, size_t len) {
  jsoff_t ctor_off = lkp_interned(js, js->global, intern_string(name, len), len);
  if (ctor_off == 0) return js_mknull();
  jsval_t ctor = resolveprop(js, mkval(T_PROP, ctor_off));
  if (vtype(ctor) != T_FUNC) return js_mknull();
  jsval_t ctor_obj = js_as_obj(ctor);
  jsoff_t proto_off = lkp_interned(js, ctor_obj, INTERN_PROTOTYPE, 9);
  if (proto_off == 0) return js_mknull();
  return resolveprop(js, mkval(T_PROP, proto_off));
}

static inline jsval_t get_ctor_proto(ant_t *js, const char *name, size_t len) {
  return js_get_ctor_proto(js, name, len);
}

static jsval_t get_prototype_for_type(ant_t *js, uint8_t type) {
switch (type) {
  case T_STR:     return get_ctor_proto(js, "String", 6);
  case T_NUM:     return get_ctor_proto(js, "Number", 6);
  case T_BOOL:    return get_ctor_proto(js, "Boolean", 7);
  case T_ARR:     return get_ctor_proto(js, "Array", 5);
  case T_FUNC:    return get_ctor_proto(js, "Function", 8);
  case T_PROMISE: return get_ctor_proto(js, "Promise", 7);
  case T_OBJ:     return get_ctor_proto(js, "Object", 6);
  case T_BIGINT:  return get_ctor_proto(js, "BigInt", 6);
  case T_SYMBOL:  return get_ctor_proto(js, "Symbol", 6);
  default:        return js_mknull();
}}

jsoff_t lkp_proto(ant_t *js, jsval_t obj, const char *key, size_t len) {
  uint8_t t = vtype(obj);
  const char *key_intern = intern_string(key, len);
  if (!key_intern) return 0;

  jsval_t cur = obj;
  int depth = 0;
  
  while (depth < MAX_PROTO_CHAIN_DEPTH) {
    if (t == T_OBJ || t == T_ARR || t == T_FUNC || t == T_PROMISE) {
      jsval_t as_obj = js_as_obj(cur);
      jsoff_t off = lkp_interned(js, as_obj, key_intern, len);
      if (off != 0) return off;
    } else if (t == T_CFUNC) {
      jsval_t func_proto = get_ctor_proto(js, "Function", 8);
      uint8_t ft = vtype(func_proto);
      if (ft == T_OBJ || ft == T_ARR || ft == T_FUNC) {
        jsoff_t off = lkp(js, js_as_obj(func_proto), key, len);
        if (off != 0) return off;
      }
      break;
    } else if (t != T_STR && t != T_NUM && t != T_BOOL && t != T_BIGINT && t != T_SYMBOL) break;
    if (!proto_walk_next(js, &cur, &t, PROTO_WALK_F_LOOKUP)) { break; } depth++;
  }
  
  return 0;
}

static jsval_t getprop_any(ant_t *js, jsval_t obj, const char *key, size_t key_len) {
  uint8_t t = vtype(obj);
  
  if (t == T_STR && key_len == 6 && memcmp(key, "length", 6) == 0) {
    jsoff_t byte_len;
    jsoff_t str_off = vstr(js, obj, &byte_len);
    return tov(D(utf16_strlen((const char *)&js->mem[str_off], byte_len)));
  }
  
  if (t == T_STR || t == T_NUM || t == T_BOOL || t == T_BIGINT) {
    jsoff_t off = lkp_proto(js, obj, key, key_len);
    if (off != 0) return resolveprop(js, mkval(T_PROP, off));
    return js_mkundef();
  }
  
  if (t == T_OBJ || t == T_ARR || t == T_FUNC) {
    jsval_t as_obj = js_as_obj(obj);
    jsoff_t off = lkp(js, as_obj, key, key_len);
    if (off != 0) return resolveprop(js, mkval(T_PROP, off));
    off = lkp_proto(js, obj, key, key_len);
    if (off != 0) return resolveprop(js, mkval(T_PROP, off));
  }
  
  return js_mkundef();
}

static jsval_t try_dynamic_getter(ant_t *js, jsval_t obj, const char *key, size_t key_len) {
  jsoff_t obj_off = (jsoff_t)vdata(js_as_obj(obj));
  dynamic_accessors_t *entry = NULL;
  HASH_FIND(hh, accessor_registry, &obj_off, sizeof(jsoff_t), entry);
  if (!entry || !entry->getter) return js_mkundef();
  return entry->getter(js, obj, key, key_len);
}

static bool try_dynamic_setter(ant_t *js, jsval_t obj, const char *key, size_t key_len, jsval_t value) {
  jsoff_t obj_off = (jsoff_t)vdata(js_as_obj(obj));
  dynamic_accessors_t *entry = NULL;
  HASH_FIND(hh, accessor_registry, &obj_off, sizeof(jsoff_t), entry);
  if (!entry || !entry->setter) return false;
  return entry->setter(js, obj, key, key_len, value);
}

static bool try_dynamic_deleter(ant_t *js, jsval_t obj, const char *key, size_t key_len) {
  jsoff_t obj_off = (jsoff_t)vdata(js_as_obj(obj));
  dynamic_accessors_t *entry = NULL;
  HASH_FIND(hh, accessor_registry, &obj_off, sizeof(jsoff_t), entry);
  if (!entry || !entry->deleter) return false;
  return entry->deleter(js, obj, key, key_len);
}

static bool try_accessor_getter(ant_t *js, jsval_t obj, const char *key, size_t key_len, jsval_t *out) {
  jsval_t getter = js_mkundef();
  bool has_getter = false;
  lkp_with_getter(js, obj, key, key_len, &getter, &has_getter);

  jsval_t result = call_proto_accessor(js, obj, getter, has_getter, NULL, 0, false);
  if (vtype(result) != T_UNDEF) {
    *out = result;
    return true;
  }
  return false;
}

static bool try_accessor_setter(ant_t *js, jsval_t obj, const char *key, size_t key_len, jsval_t val, jsval_t *out) {
  jsval_t setter = js_mkundef();
  bool has_setter = false;
  
  lkp_with_setter(js, obj, key, key_len, &setter, &has_setter);
  if (!has_setter) return false;

  jsval_t result = call_proto_accessor(js, obj, setter, has_setter, &val, 1, true);
  if (is_err(result)) {
    *out = result;
    return true;
  }
  
  *out = val;
  return true;
}

jsval_t resolveprop(ant_t *js, jsval_t v) {
  if (vtype(v) != T_PROP) return v;
  return resolveprop(js, loadval(js, (jsoff_t) (vdata(v) + sizeof(jsoff_t) * 2)));
}

typedef struct {
  char *buffer;
  size_t capacity;
  size_t size;
  bool is_dynamic;
} string_builder_t;

static void string_builder_init(string_builder_t *sb, char *static_buf, size_t static_cap) {
  sb->buffer = static_buf;
  sb->capacity = static_cap;
  sb->size = 0;
  sb->is_dynamic = false;
}

static bool string_builder_append(string_builder_t *sb, const char *data, size_t len) {
  if (sb->size + len > sb->capacity) {
    size_t new_capacity = sb->capacity ? sb->capacity * 2 : 256;
    while (new_capacity < sb->size + len) new_capacity *= 2;
    
    char *new_buffer = (char *)ant_calloc(new_capacity);
    if (!new_buffer) return false;
    
    if (sb->size > 0) memcpy(new_buffer, sb->buffer, sb->size);
    if (sb->is_dynamic) free(sb->buffer);
    
    sb->buffer = new_buffer;
    sb->capacity = new_capacity;
    sb->is_dynamic = true;
  }
  
  if (len > 0) {
    memcpy(sb->buffer + sb->size, data, len);
    sb->size += len;
  }
  
  return true;
}

static jsval_t string_builder_finalize(ant_t *js, string_builder_t *sb) {
  jsval_t result = js_mkstr(js, sb->buffer, sb->size);
  if (sb->is_dynamic && sb->buffer) free(sb->buffer);
  return result;
}

jsoff_t str_len_fast(ant_t *js, jsval_t str) {
  if (vtype(str) != T_STR) return 0;
  if (is_rope(js, str)) return rope_len(js, str);
  return assert_flat_string_len(js, str, NULL);
}

jsval_t do_string_op(ant_t *js, uint8_t op, jsval_t l, jsval_t r) {
  if (op == TOK_PLUS) {
    jsoff_t n1 = str_len_fast(js, l);
    jsoff_t n2 = str_len_fast(js, r);
    jsoff_t total_len = n1 + n2;
    
    if (n2 == 0) return l;
    if (n1 == 0) return r;
    
    uint8_t left_depth = (vtype(l) == T_STR && is_rope(js, l)) ? rope_depth(js, l) : 0;
    uint8_t right_depth = (vtype(r) == T_STR && is_rope(js, r)) ? rope_depth(js, r) : 0;
    uint8_t new_depth = (left_depth > right_depth ? left_depth : right_depth) + 1;
    
    if (new_depth >= ROPE_MAX_DEPTH || total_len >= ROPE_FLATTEN_THRESHOLD) {
      jsval_t flat_l = l, flat_r = r;
      if (is_rope(js, l)) flat_l = rope_flatten(js, l);
      if (is_err(flat_l)) return flat_l;
      if (is_rope(js, r)) flat_r = rope_flatten(js, r);
      if (is_err(flat_r)) return flat_r;
      
      jsoff_t off1, off2, len1, len2;
      off1 = vstr(js, flat_l, &len1);
      off2 = vstr(js, flat_r, &len2);
      
      string_builder_t sb;
      char static_buffer[512];
      string_builder_init(&sb, static_buffer, sizeof(static_buffer));
      
      if (
        !string_builder_append(&sb, (char *)&js->mem[off1], len1) ||
        !string_builder_append(&sb, (char *)&js->mem[off2], len2)
      ) return js_mkerr(js, "string concatenation failed");
      
      return string_builder_finalize(js, &sb);
    }
    
    return js_mkrope(js, l, r, total_len, new_depth);
  }
  
  jsoff_t n1, off1 = vstr(js, l, &n1);
  jsoff_t n2, off2 = vstr(js, r, &n2);
  
  if (op == TOK_EQ) {
    bool eq = n1 == n2 && memcmp(&js->mem[off1], &js->mem[off2], n1) == 0;
    return mkval(T_BOOL, eq ? 1 : 0);
  } else if (op == TOK_NE) {
    bool eq = n1 == n2 && memcmp(&js->mem[off1], &js->mem[off2], n1) == 0;
    return mkval(T_BOOL, eq ? 0 : 1);
  } else if (op == TOK_LT || op == TOK_LE || op == TOK_GT || op == TOK_GE) {
    jsoff_t min_len = n1 < n2 ? n1 : n2;
    int cmp = memcmp(&js->mem[off1], &js->mem[off2], min_len);
    
    if (cmp == 0) {
      if (n1 == n2) {
        return mkval(T_BOOL, (op == TOK_LE || op == TOK_GE) ? 1 : 0);
      } else cmp = (n1 < n2) ? -1 : 1;
    }
    
    switch (op) {
      case TOK_LT: return mkval(T_BOOL, cmp < 0 ? 1 : 0);
      case TOK_LE: return mkval(T_BOOL, cmp <= 0 ? 1 : 0);
      case TOK_GT: return mkval(T_BOOL, cmp > 0 ? 1 : 0);
      case TOK_GE: return mkval(T_BOOL, cmp >= 0 ? 1 : 0);
      default:     return js_mkerr(js, "bad str op");
    }
  } else return js_mkerr(js, "bad str op");
}


typedef enum { ITER_CONTINUE, ITER_BREAK, ITER_ERROR } iter_action_t;
typedef iter_action_t (*iter_callback_t)(ant_t *js, jsval_t value, void *ctx, jsval_t *out);
static jsval_t iter_foreach(ant_t *js, jsval_t iterable, iter_callback_t cb, void *ctx);


static bool js_try_call_method(ant_t *js, jsval_t obj, const char *method, size_t method_len, jsval_t *args, int nargs, jsval_t *out_result) {
  jsval_t getter = js_mkundef(); bool has_getter = false;
  jsoff_t off = lkp_with_getter(js, obj, method, method_len, &getter, &has_getter);
  
  jsval_t fn;
  if (has_getter) {
    fn = call_proto_accessor(js, obj, getter, true, NULL, 0, false);
    if (is_err(fn)) { *out_result = fn; return true; }
  } else if (off != 0) {
    fn = resolveprop(js, mkval(T_PROP, off));
  } else return false;
  
  uint8_t ft = vtype(fn);
  if (ft != T_FUNC && ft != T_CFUNC) return false;
  
  jsval_t saved_this = js->this_val;
  js->this_val = obj;
  
  jsval_t result;
  if (ft == T_CFUNC) result = ((jsval_t (*)(ant_t *, jsval_t *, int))vdata(fn))(js, args, nargs);
  else result = sv_vm_call(js->vm, js, fn, obj, args, nargs, NULL, false);
  
  bool had_throw = js->thrown_exists;
  jsval_t thrown = js->thrown_value;
  
  js->this_val = saved_this;
  if (had_throw) {
    js->thrown_exists = true;
    js->thrown_value = thrown;
  }
  
  *out_result = result;
  return true;
}

static jsval_t js_call_method(ant_t *js, jsval_t obj, const char *method, size_t method_len, jsval_t *args, int nargs) {
  jsval_t result;
  if (!js_try_call_method(js, obj, method, method_len, args, nargs, &result)) return js_mkundef();
  return result;
}

static jsval_t js_call_toString(ant_t *js, jsval_t value) {
  jsval_t result = js_call_method(js, value, "toString", 8, NULL, 0);
  
  if (is_err(result)) return result;
  if (vtype(result) == T_STR) return result;
  
  uint8_t rtype = vtype(result);
  if (rtype == T_UNDEF) {
    goto fallback;
  }
  
  if (rtype != T_OBJ && rtype != T_ARR && rtype != T_FUNC) {
    char buf[256];
    size_t len = tostr(js, result, buf, sizeof(buf));
    return js_mkstr(js, buf, len);
  }
  
fallback:;
  char buf[4096];
  size_t len = tostr(js, value, buf, sizeof(buf));
  return js_mkstr(js, buf, len);
}

static jsval_t js_call_valueOf(ant_t *js, jsval_t value) {
  jsval_t result = js_call_method(js, value, "valueOf", 7, NULL, 0);
  if (vtype(result) == T_UNDEF) return value;
  return result;
}

static inline bool is_primitive(jsval_t v) {
  uint8_t t = vtype(v);
  return t == T_STR || t == T_NUM || t == T_BOOL || t == T_NULL || t == T_UNDEF || t == T_SYMBOL || t == T_BIGINT;
}

static jsval_t try_exotic_to_primitive(ant_t *js, jsval_t value, int hint) {
  jsval_t tp_sym = get_toPrimitive_sym();
  if (vtype(tp_sym) != T_SYMBOL) return mkval(T_UNDEF, 0);
  jsoff_t tp_off = lkp_sym_proto(js, value, (jsoff_t)vdata(tp_sym));
  if (tp_off == 0) return mkval(T_UNDEF, 0);
  
  jsval_t tp_fn = resolveprop(js, mkval(T_PROP, tp_off));
  uint8_t ft = vtype(tp_fn);
  
  if (ft == T_UNDEF) return mkval(T_UNDEF, 0);
  if (ft != T_FUNC && ft != T_CFUNC) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Symbol.toPrimitive is not a function");
  }
  
  const char *hint_str = hint == 1 ? "string" : (hint == 2 ? "number" : "default");
  jsval_t hint_arg = js_mkstr(js, hint_str, strlen(hint_str));
  jsval_t result = sv_vm_call(js->vm, js, tp_fn, value, &hint_arg, 1, NULL, false);
  
  if (is_err(result) || is_primitive(result)) return result;
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert object to primitive value");
}

static jsval_t try_ordinary_to_primitive(ant_t *js, jsval_t value, int hint) {
  static const char *names[] = {"valueOf", "toString"};
  static const size_t lens[] = {7, 8};
  
  int first = (hint == 1); 
  jsval_t result;
  
  for (int i = 0; i < 2; i++) {
    int idx = first ^ i;
    if (js_try_call_method(js, value, names[idx], lens[idx], NULL, 0, &result))
      if (is_err(result) || is_primitive(result)) return result;
  }
  
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert object to primitive value");
}

jsval_t js_to_primitive(ant_t *js, jsval_t value, int hint) {
  if (is_primitive(value)) return value;
  if (!is_object_type(value)) return value;
  
  jsval_t result = try_exotic_to_primitive(js, value, hint);
  if (vtype(result) != T_UNDEF) return result;
  
  return try_ordinary_to_primitive(js, value, hint);
}

bool strict_eq_values(ant_t *js, jsval_t l, jsval_t r) {
  uint8_t t = vtype(l);
  if (t != vtype(r)) return false;
  if (t == T_STR) {
    jsoff_t n1, n2, off1 = vstr(js, l, &n1), off2 = vstr(js, r, &n2);
    return n1 == n2 && memcmp(&js->mem[off1], &js->mem[off2], n1) == 0;
  }
  if (t == T_NUM) return tod(l) == tod(r);
  if (t == T_BIGINT) return bigint_compare(js, l, r) == 0;
  return vdata(l) == vdata(r);
}

jsval_t coerce_to_str(ant_t *js, jsval_t v) {
  if (vtype(v) == T_STR) return v;
  
  if (is_object_type(v)) {
    jsval_t prim = js_to_primitive(js, v, 1);
    if (is_err(prim)) return prim;
    if (vtype(prim) == T_STR) return prim;
    return js_tostring_val(js, prim);
  }
  
  return js_tostring_val(js, v);
}

jsval_t coerce_to_str_concat(ant_t *js, jsval_t v) {
  if (vtype(v) == T_STR) return v;
  
  if (is_object_type(v)) {
    jsval_t prim = js_to_primitive(js, v, 0);
    if (is_err(prim)) return prim;
    if (vtype(prim) == T_STR) return prim;
    return js_tostring_val(js, prim);
  }
  
  return js_tostring_val(js, v);
}

static void unlink_prop(ant_t *js, jsoff_t obj_off, jsoff_t prop_off, jsoff_t prev_off) {
  jsoff_t deleted_next = loadoff(js, prop_off) & ~FLAGMASK;
  jsoff_t target = prev_off ? prev_off : obj_off;
  jsoff_t current = loadoff(js, target);
  
  saveoff(js, target, (deleted_next & ~3ULL) | (current & (FLAGMASK | 3ULL)));
  jsoff_t tail = loadoff(js, obj_off + sizeof(jsoff_t) + sizeof(jsoff_t));
  
  if (tail == prop_off) {
    saveoff(js, obj_off + sizeof(jsoff_t) + sizeof(jsoff_t), prev_off);
  }
  
  invalidate_prop_cache(js, obj_off, prop_off);
  js->needs_gc = true;
}

static jsval_t check_frozen_sealed(ant_t *js, jsval_t obj, const char *action) {
  if (js_truthy(js, get_slot(js, obj, SLOT_FROZEN))) {
    if (sv_vm_is_strict(js->vm)) return js_mkerr(js, "cannot %s property of frozen object", action);
    return js_false;
  }
  if (js_truthy(js, get_slot(js, obj, SLOT_SEALED))) {
    if (sv_vm_is_strict(js->vm)) return js_mkerr(js, "cannot %s property of sealed object", action);
    return js_false;
  }
  return js_mkundef();
}

jsval_t js_delete_prop(ant_t *js, jsval_t obj, const char *key, size_t len) {
  obj = js_as_obj(obj);
  if (is_proxy(js, obj)) {
    jsval_t result = proxy_delete(js, obj, key, len);
    return is_err(result) ? result : js_bool(js_truthy(js, result));
  }

  jsval_t err = check_frozen_sealed(js, obj, "delete");
  if (vtype(err) != T_UNDEF) return err;

  jsoff_t obj_off = (jsoff_t)vdata(js_as_obj(obj));

  if (is_arr_off(js, obj_off)) {
    jsoff_t doff = get_dense_buf_off(js, obj_off);
    unsigned long del_idx;
    if (doff && parse_array_index(key, len, dense_length(js, doff), &del_idx)) {
      arr_del(js, mkval(T_ARR, (uint64_t)obj_off), (jsoff_t)del_idx);
      return js_true;
    }
  }

  jsoff_t prop_off = lkp(js, obj, key, len);
  if (prop_off == 0) {
    try_dynamic_deleter(js, obj, key, len);
    return js_true;
  }

  if (is_nonconfig_prop(js, prop_off)) {
    if (sv_vm_is_strict(js->vm)) return js_mkerr_typed(js, JS_ERR_TYPE, "cannot delete non-configurable property");
    return js_false;
  }

  descriptor_entry_t *desc = lookup_descriptor(js, obj_off, key, len);
  if (desc && !desc->configurable) {
    if (sv_vm_is_strict(js->vm)) return js_mkerr_typed(js, JS_ERR_TYPE, "cannot delete non-configurable property");
    return js_false;
  }

  jsoff_t first_prop = loadoff(js, obj_off) & ~(3U | FLAGMASK);
  if (first_prop == prop_off) {
    unlink_prop(js, obj_off, prop_off, 0);
    return js_true;
  }
  for (jsoff_t prev = first_prop; prev != 0; ) {
    jsoff_t next_prop = loadoff(js, prev) & ~(3U | FLAGMASK);
    if (next_prop == prop_off) {
      unlink_prop(js, obj_off, prop_off, prev);
      return js_true;
    }
    prev = next_prop;
  }
  return js_true;
}

jsval_t js_delete_sym_prop(ant_t *js, jsval_t obj, jsval_t sym) {
  obj = js_as_obj(obj);
  if (is_proxy(js, obj)) {
    jsval_t result = proxy_delete_val(js, obj, sym);
    return is_err(result) ? result : js_bool(js_truthy(js, result));
  }

  jsval_t err = check_frozen_sealed(js, obj, "delete");
  if (vtype(err) != T_UNDEF) return err;

  jsoff_t obj_off = (jsoff_t)vdata(obj);
  jsoff_t sym_off = (jsoff_t)vdata(sym);
  jsoff_t prop_off = lkp_sym(js, obj, sym_off);
  if (prop_off == 0) return js_true;

  if (is_nonconfig_prop(js, prop_off)) {
    if (sv_vm_is_strict(js->vm)) return js_mkerr_typed(js, JS_ERR_TYPE, "cannot delete non-configurable property");
    return js_false;
  }

  jsoff_t first_prop = loadoff(js, obj_off) & ~(3U | FLAGMASK);
  if (first_prop == prop_off) {
    unlink_prop(js, obj_off, prop_off, 0);
    return js_true;
  }
  for (jsoff_t prev = first_prop; prev != 0; ) {
    jsoff_t np = loadoff(js, prev) & ~(3U | FLAGMASK);
    if (np == prop_off) {
      unlink_prop(js, obj_off, prop_off, prev);
      return js_true;
    }
    prev = np;
  }
  return js_true;
}

static jsval_t iter_call_noargs_with_this(ant_t *js, jsval_t this_val, jsval_t method) {
  jsval_t result = sv_vm_call(js->vm, js, method, this_val, NULL, 0, NULL, false);
  return result;
}

static jsval_t iter_close_iterator(ant_t *js, jsval_t iterator) {
  jsoff_t return_off = lkp_proto(js, iterator, "return", 6);
  if (return_off == 0) return js_mkundef();
  jsval_t return_method = loadval(js, return_off + sizeof(jsoff_t) * 2);
  if (vtype(return_method) != T_FUNC && vtype(return_method) != T_CFUNC) {
    return js_mkerr(js, "iterator.return is not a function");
  }
  return iter_call_noargs_with_this(js, iterator, return_method);
}

static jsval_t iter_foreach(ant_t *js, jsval_t iterable, iter_callback_t cb, void *ctx) {
  jsval_t iter_sym = get_iterator_sym();
  jsoff_t iter_prop = (vtype(iter_sym) == T_SYMBOL) ? lkp_sym_proto(js, iterable, (jsoff_t)vdata(iter_sym)) : 0;
  if (iter_prop == 0) return js_mkerr(js, "not iterable");

  jsval_t iter_method = loadval(js, iter_prop + sizeof(jsoff_t) * 2);
  jsval_t iterator = iter_call_noargs_with_this(js, iterable, iter_method);
  if (is_err(iterator)) return iterator;
  
  jshdl_t h_iterator = js_root(js, iterator);
  jsval_t out = js_mkundef();
  
  while (true) {
    jsval_t cur_iter = js_deref(js, h_iterator);
    jsoff_t next_off = lkp_proto(js, cur_iter, "next", 4);
    if (next_off == 0) { js_unroot(js, h_iterator); return js_mkerr(js, "iterator.next is not a function"); }
    
    jsval_t next_method = loadval(js, next_off + sizeof(jsoff_t) * 2);
    if (vtype(next_method) != T_FUNC && vtype(next_method) != T_CFUNC) {
      js_unroot(js, h_iterator);
      return js_mkerr(js, "iterator.next is not a function");
    }
    
    cur_iter = js_deref(js, h_iterator);
    jsval_t result = iter_call_noargs_with_this(js, cur_iter, next_method);
    if (is_err(result)) { js_unroot(js, h_iterator); return result; }
    
    jsoff_t done_off = lkp(js, result, "done", 4);
    jsval_t done_val = done_off ? loadval(js, done_off + sizeof(jsoff_t) * 2) : js_mkundef();
    if (js_truthy(js, done_val)) break;
    
    jsoff_t value_off = lkp(js, result, "value", 5);
    jsval_t value = value_off ? loadval(js, value_off + sizeof(jsoff_t) * 2) : js_mkundef();
    
    iter_action_t action = cb(js, value, ctx, &out);
    if (action == ITER_BREAK) {
      jsval_t close_result = iter_close_iterator(js, cur_iter);
      if (is_err(close_result)) { js_unroot(js, h_iterator); return close_result; }
      break;
    }
    if (action == ITER_ERROR) {
      jsval_t close_result = iter_close_iterator(js, cur_iter);
      js_unroot(js, h_iterator);
      if (is_err(close_result)) return close_result;
      return out;
    }
  }
  
  js_unroot(js, h_iterator);
  return out;
}

jsval_t js_symbol_to_string(ant_t *js, jsval_t sym) {
  const char *desc = js_sym_desc(js, sym);
  if (!desc) return js_mkstr(js, "Symbol()", 8);
  
  size_t desc_len = strlen(desc);
  size_t total = 7 + desc_len + 1;
  
  char stack_buf[128];
  char *buf = (total + 1 <= sizeof(stack_buf)) ? stack_buf : malloc(total + 1);
  if (!buf) return js_mkerr(js, "out of memory");
  
  memcpy(buf, "Symbol(", 7);
  memcpy(buf + 7, desc, desc_len);
  buf[7 + desc_len] = ')';
  buf[total] = '\0';
  
  jsval_t result = js_mkstr(js, buf, total);
  if (buf != stack_buf) free(buf);
  return result;
}

static jsval_t builtin_String(ant_t *js, jsval_t *args, int nargs) {
  jsval_t sval;
  
  if (nargs == 0) {
    sval = js_mkstr(js, "", 0);
  } else if (vtype(args[0]) == T_STR) {
    sval = args[0];
  } else if (vtype(args[0]) == T_SYMBOL) {
    sval = js_symbol_to_string(js, args[0]);
    if (is_err(sval)) return sval;
  } else {
    sval = coerce_to_str(js, args[0]);
    if (is_err(sval)) return sval;
  }
  
  jsval_t string_proto = js_get_ctor_proto(js, "String", 6);
  if (is_wrapper_ctor_target(js, js->this_val, string_proto)) {
    set_slot(js, js->this_val, SLOT_PRIMITIVE, sval);
    jsoff_t slen;
    vstr(js, sval, &slen);
    js_setprop(js, js->this_val, js->length_str, tov((double)slen));
    js_set_descriptor(js, js->this_val, "length", 6, 0);
  }
  return sval;
}

static jsval_t builtin_Number_isNaN(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkval(T_BOOL, 0);
  jsval_t arg = args[0];
  
  if (vtype(arg) != T_NUM) return mkval(T_BOOL, 0);
  
  double val = tod(arg);
  return mkval(T_BOOL, isnan(val) ? 1 : 0);
}

static jsval_t builtin_Number_isFinite(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkval(T_BOOL, 0);
  jsval_t arg = args[0];
  
  if (vtype(arg) != T_NUM) return mkval(T_BOOL, 0);
  
  double val = tod(arg);
  return mkval(T_BOOL, isfinite(val) ? 1 : 0);
}

static jsval_t builtin_global_isNaN(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkval(T_BOOL, 1);
  double val = js_to_number(js, args[0]);
  return mkval(T_BOOL, isnan(val) ? 1 : 0);
}

static jsval_t builtin_global_isFinite(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkval(T_BOOL, 0);
  double val = js_to_number(js, args[0]);
  return mkval(T_BOOL, isfinite(val) ? 1 : 0);
}

static jsval_t builtin_eval(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_mkundef();
  jsval_t code = args[0];
  if (vtype(code) != T_STR) return code;
  jsoff_t code_len = 0;
  jsoff_t code_off = vstr(js, code, &code_len);
  const char *code_str = (const char *)&js->mem[code_off];
  return js_eval_bytecode_eval_with_strict(js, code_str, (size_t)code_len, false);
}

static jsval_t builtin_Number_isInteger(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkval(T_BOOL, 0);
  jsval_t arg = args[0];
  
  if (vtype(arg) != T_NUM) return mkval(T_BOOL, 0);
  
  double val = tod(arg);
  if (!isfinite(val)) return mkval(T_BOOL, 0);
  return mkval(T_BOOL, (val == floor(val)) ? 1 : 0);
}

static jsval_t builtin_Number_isSafeInteger(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkval(T_BOOL, 0);
  jsval_t arg = args[0];
  
  if (vtype(arg) != T_NUM) return mkval(T_BOOL, 0);
  
  double val = tod(arg);
  if (!isfinite(val)) return mkval(T_BOOL, 0);
  if (val != floor(val)) return mkval(T_BOOL, 0);
  
  return mkval(T_BOOL, (val >= -9007199254740991.0 && val <= 9007199254740991.0) ? 1 : 0);
}

static jsval_t builtin_Number(ant_t *js, jsval_t *args, int nargs) {
  jsval_t nval = tov(nargs > 0 ? js_to_number(js, args[0]) : 0.0);
  jsval_t number_proto = js_get_ctor_proto(js, "Number", 6);
  if (is_wrapper_ctor_target(js, js->this_val, number_proto)) {
    set_slot(js, js->this_val, SLOT_PRIMITIVE, nval);
  }
  return nval;
}

static jsval_t builtin_Boolean(ant_t *js, jsval_t *args, int nargs) {
  jsval_t bval = mkval(T_BOOL, nargs > 0 && js_truthy(js, args[0]) ? 1 : 0);
  jsval_t boolean_proto = js_get_ctor_proto(js, "Boolean", 7);
  if (is_wrapper_ctor_target(js, js->this_val, boolean_proto)) {
    set_slot(js, js->this_val, SLOT_PRIMITIVE, bval);
  }
  return bval;
}

static jsval_t builtin_Object(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0 || vtype(args[0]) == T_NULL || vtype(args[0]) == T_UNDEF) {
    jsval_t obj_proto = js_get_ctor_proto(js, "Object", 6);
    if (is_unboxed_obj(js, js->this_val, obj_proto)) return js->this_val;
    return js_mkobj(js);
  }
  
  jsval_t arg = args[0];
  uint8_t t = vtype(arg);
  
  if (t == T_OBJ || t == T_ARR || t == T_FUNC) return arg;
  if (t == T_STR || t == T_NUM || t == T_BOOL || t == T_BIGINT) {
    jsval_t wrapper = js_mkobj(js);
    if (is_err(wrapper)) return wrapper;
    set_slot(js, wrapper, SLOT_PRIMITIVE, arg);
    jsval_t proto = get_prototype_for_type(js, t);
    if (vtype(proto) == T_OBJ) set_proto(js, wrapper, proto);
    return wrapper;
  }
  
  return arg;
}

static jsval_t builtin_function_empty(ant_t *, jsval_t *, int);

static jsval_t build_dynamic_function(ant_t *js, jsval_t *args, int nargs, bool is_async) {
  if (nargs == 0) {
    jsval_t func_obj = mkobj(js, 0);
    if (is_err(func_obj)) return func_obj;
    
    set_func_code_ptr(js, func_obj, "(){}", 4);
    if (is_async) {
      set_slot(js, func_obj, SLOT_ASYNC, js_true);
      jsval_t async_proto = get_slot(js, js_glob(js), SLOT_ASYNC_PROTO);
      if (vtype(async_proto) == T_FUNC) set_proto(js, func_obj, async_proto);
    } else {
      jsval_t func_proto = get_slot(js, js_glob(js), SLOT_FUNC_PROTO);
      jsval_t instance_proto = js_instance_proto_from_new_target(js, func_proto);
      if (is_object_type(instance_proto)) set_proto(js, func_obj, instance_proto);
    }
    set_slot(js, func_obj, SLOT_CFUNC, js_mkfun(builtin_function_empty));
    
    jsval_t func = js_obj_to_func(func_obj);
    jsval_t proto_setup = setup_func_prototype(js, func);
    if (is_err(proto_setup)) return proto_setup;
    return func;
  }
  
  size_t total_len = 1;
  
  for (int i = 0; i < nargs - 1; i++) {
    args[i] = coerce_to_str(js, args[i]);
    if (is_err(args[i])) return args[i];
    total_len += vstrlen(js, args[i]);
    if (i < nargs - 2) total_len += 1;
  }
  
  total_len += 2;
  
  jsval_t body = coerce_to_str(js, args[nargs - 1]);
  if (is_err(body)) return body;
  total_len += vstrlen(js, body);
  total_len += 1;
  
  char *code_buf = (char *)malloc(total_len + 1);
  if (!code_buf) return js_mkerr(js, "oom");
  size_t pos = 0;

  code_buf[pos++] = '(';
  for (int i = 0; i < nargs - 1; i++) {
    jsoff_t param_len, param_off = vstr(js, args[i], &param_len);
    memcpy(code_buf + pos, &js->mem[param_off], param_len);
    pos += param_len;
    if (i < nargs - 2) code_buf[pos++] = ',';
  }
  code_buf[pos++] = ')';
  code_buf[pos++] = '{';
  jsoff_t body_len, body_off = vstr(js, body, &body_len);
  memcpy(code_buf + pos, &js->mem[body_off], body_len);
  pos += body_len;
  code_buf[pos++] = '}';
  code_buf[pos] = '\0';

  jsval_t func_obj = mkobj(js, 0);
  if (is_err(func_obj)) { free(code_buf); return func_obj; }

  sv_func_t *compiled = sv_compile_function(js, code_buf, pos, is_async);
  if (!compiled) {
    free(code_buf);
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "invalid function body");
  }

  sv_closure_t *closure = calloc(1, sizeof(sv_closure_t));
  closure->func = compiled;
  closure->bound_this = js_mkundef();
  closure->call_flags = 0;
  closure->func_obj = func_obj;

  size_t params_len = (size_t)(pos - 2) - (size_t)body_len - 2;
  const char *async_prefix = is_async ? "async " : "";
  size_t async_len = is_async ? 6 : 0;
  size_t display_len = async_len + 19 + params_len + 5 + (size_t)body_len + 2;
  char *display = (char *)malloc(display_len + 1);
  if (!display) { free(code_buf); return js_mkerr(js, "oom"); }
  size_t n = 0;
  
  memcpy(display + n, async_prefix, async_len);         n += async_len;
  memcpy(display + n, "function anonymous(", 19);       n += 19;
  memcpy(display + n, code_buf + 1, params_len);        n += params_len;
  memcpy(display + n, "\n) {\n", 5);                    n += 5;
  memcpy(display + n, code_buf + 1 + params_len + 2, (size_t)body_len); n += (size_t)body_len;
  memcpy(display + n, "\n}", 2);                        n += 2;
  display[n] = '\0';
  set_func_code(js, func_obj, display, display_len);
  free(display);
  free(code_buf);
  
  if (is_async) {
    set_slot(js, func_obj, SLOT_ASYNC, js_true);
    jsval_t async_proto = get_slot(js, js_glob(js), SLOT_ASYNC_PROTO);
    if (vtype(async_proto) == T_FUNC) set_proto(js, func_obj, async_proto);
  } else {
    jsval_t func_proto = get_slot(js, js_glob(js), SLOT_FUNC_PROTO);
    jsval_t instance_proto = js_instance_proto_from_new_target(js, func_proto);
    if (is_object_type(instance_proto)) set_proto(js, func_obj, instance_proto);
  }
  
  jsval_t func = mkval(T_FUNC, (uintptr_t)closure);
  jsval_t proto_setup = setup_func_prototype(js, func);
  if (is_err(proto_setup)) return proto_setup;
  return func;
}

static jsval_t builtin_Function(ant_t *js, jsval_t *args, int nargs) {
  return build_dynamic_function(js, args, nargs, false);
}

static jsval_t builtin_AsyncFunction(ant_t *js, jsval_t *args, int nargs) {
  return build_dynamic_function(js, args, nargs, true);
}

static jsval_t builtin_function_empty(ant_t *js, jsval_t *args, int nargs) {
  (void)js; (void)args; (void)nargs;
  return js_mkundef();
}

static jsval_t builtin_function_call(ant_t *js, jsval_t *args, int nargs) {
  jsval_t func = js->this_val;
  if (vtype(func) != T_FUNC && vtype(func) != T_CFUNC) {
    return js_mkerr(js, "call requires a function");
  }
  
  jsval_t this_arg = (nargs > 0) ? args[0] : js_mkundef();
  
  jsval_t *call_args = NULL;
  int call_nargs = (nargs > 1) ? nargs - 1 : 0;
  if (call_nargs > 0) {
    call_args = &args[1];
  }
  
  return sv_vm_call(js->vm, js, func, this_arg, call_args, call_nargs, NULL, false);
}

static int extract_array_args(ant_t *js, jsval_t arr, jsval_t **out_args) {
  int len = (int) get_array_length(js, arr);
  if (len <= 0) return 0;
  
  jsval_t *args_out = (jsval_t *)ant_calloc(sizeof(jsval_t) * len);
  if (!args_out) return 0;
  
  for (int i = 0; i < len; i++) {
    args_out[i] = arr_get(js, arr, (jsoff_t)i);
  }
  
  *out_args = args_out;
  return len;
}

static jsval_t builtin_function_toString(ant_t *js, jsval_t *args, int nargs) {
  jsval_t func = js->this_val;
  uint8_t t = vtype(func);
  
  if (t != T_FUNC && t != T_CFUNC) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Function.prototype.toString requires that 'this' be a Function");
  }
  
  if (t == T_CFUNC) return ANT_STRING("function() { [native code] }");
  
  jsval_t func_obj = js_func_obj(func);
  jsval_t cfunc_slot = get_slot(js, func_obj, SLOT_CFUNC);
  
  if (vtype(cfunc_slot) == T_CFUNC) {
    jsoff_t name_len = 0;
    const char *name = get_func_name(js, func, &name_len);
    if (name && name_len > 0) {
      size_t total = 9 + name_len + 21 + 1;
      char *buf = ant_calloc(total);
      size_t n = 0;
      n += cpy(buf + n, total - n, "function ", 9);
      n += cpy(buf + n, total - n, name, name_len);
      n += cpy(buf + n, total - n, "() { [native code] }", 20);
      jsval_t result = js_mkstr(js, buf, n);
      free(buf);
      return result;
    }
    return ANT_STRING("function() { [native code] }");
  }
  
  jsval_t code_val = get_slot(js, func_obj, SLOT_CODE);
  jsval_t len_val = get_slot(js, func_obj, SLOT_CODE_LEN);
  
  if (vtype(code_val) == T_CFUNC && vtype(len_val) == T_NUM) {
    const char *code = (const char *)(uintptr_t)vdata(code_val);
    size_t code_len = (size_t)tod(len_val);
    
    if (code && code_len > 0) {
      jsval_t async_slot = get_slot(js, func_obj, SLOT_ASYNC);
      jsval_t arrow_slot = get_slot(js, func_obj, SLOT_ARROW);
      
      bool is_async = (async_slot == js_true);
      bool is_arrow = (arrow_slot == js_true);
      
      if (is_arrow) {
        const char *paren_end = memchr(code, ')', code_len);
        if (!paren_end) goto fallback_arrow;
        
        size_t params_len = paren_end - code + 1;
        const char *body = paren_end + 1;
        size_t body_len = code_len - params_len;
        
        size_t len = (is_async ? 6 : 0) + params_len + 4 + body_len + 1;
        char *buf = ant_calloc(len);
        size_t n = 0;
        
        if (is_async) n += cpy(buf + n, REMAIN(n, len), "async ", 6);
        n += cpy(buf + n, REMAIN(n, len), code, params_len);
        n += cpy(buf + n, REMAIN(n, len), " => ", 4);
        n += cpy(buf + n, REMAIN(n, len), body, body_len);
        
        jsval_t result = js_mkstr(js, buf, n);
        free(buf);
        return result;
        fallback_arrow:;
      }
      
      return js_mkstr(js, code, code_len);
    }
  }
  
  sv_closure_t *cl = js_func_closure(func);
  if (cl->func != NULL) {
    sv_func_t *fn = cl->func;
    if (fn && fn->source && fn->source_end > fn->source_start) {
      int start = fn->source_start;
      int end   = fn->source_end;
      if (start >= 0 && end <= fn->source_len && end > start)
        return js_mkstr(js, fn->source + start, (size_t)(end - start));
    }
  }

  char buf[256];
  size_t len = strfunc(js, func, buf, sizeof(buf));
  return js_mkstr(js, buf, len);
}

static jsval_t builtin_function_apply(ant_t *js, jsval_t *args, int nargs) {
  jsval_t func = js->this_val;
  if (vtype(func) != T_FUNC && vtype(func) != T_CFUNC) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Function.prototype.apply requires that 'this' be a Function");
  }
  
  jsval_t this_arg = (nargs > 0) ? args[0] : js_mkundef();
  jsval_t *call_args = NULL;
  int call_nargs = 0;
  
  if (nargs > 1) {
    jsval_t arg_array = args[1];
    uint8_t t = vtype(arg_array);
    if (t == T_ARR || t == T_OBJ) {
      call_nargs = extract_array_args(js, arg_array, &call_args);
    } else if (t != T_UNDEF && t != T_NULL) {}
  }
  
  jsval_t result = sv_vm_call(js->vm, js, func, this_arg, call_args, call_nargs, NULL, false);
  if (call_args) free(call_args);
  return result;
}

static jsval_t builtin_function_bind(ant_t *js, jsval_t *args, int nargs) {
  jsval_t func = js->this_val;
  
  if (vtype(func) != T_FUNC && vtype(func) != T_CFUNC) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "bind requires a function");
  }

  jsval_t this_arg = (nargs > 0) ? args[0] : js_mkundef();
  
  int bound_argc = (nargs > 1) ? nargs - 1 : 0;
  jsval_t *bound_args = (bound_argc > 0) ? &args[1] : NULL;
  
  int orig_length = 0;
  jsval_t target_func_obj;
  if (vtype(func) == T_CFUNC) {
    orig_length = 0;
  } else {
    target_func_obj = js_func_obj(func);
    jsoff_t len_off = lkp_interned(js, target_func_obj, INTERN_LENGTH, 6);
    if (len_off != 0) {
      jsval_t len_val = resolveprop(js, mkval(T_PROP, len_off));
      if (vtype(len_val) == T_NUM) {
        orig_length = (int) tod(len_val);
      }
    }
  }
  
  int bound_length = orig_length - bound_argc;
  if (bound_length < 0) bound_length = 0;

  if (vtype(func) == T_CFUNC) {
    jsval_t bound_func = mkobj(js, 0);
    if (is_err(bound_func)) return bound_func;
    
    set_slot(js, bound_func, SLOT_CFUNC, func);
    set_slot(js, bound_func, SLOT_BOUND_THIS, this_arg);
    
    if (bound_argc > 0) {
      jsval_t bound_arr = mkarr(js);
      for (int i = 0; i < bound_argc; i++) arr_set(js, bound_arr, (jsoff_t)i, bound_args[i]);
      set_slot(js, bound_func, SLOT_BOUND_ARGS, bound_arr);
    }
    
    jsval_t func_proto = get_slot(js, js_glob(js), SLOT_FUNC_PROTO);
    if (vtype(func_proto) == T_FUNC) set_proto(js, bound_func, func_proto);
    
    jsval_t bound = js_obj_to_func(bound_func);
    js_setprop(js, bound_func, js->length_str, tov((double) bound_length));
    
    jsval_t proto_setup = setup_func_prototype(js, bound);
    if (is_err(proto_setup)) return proto_setup;
    
    return bound;
  }

  jsval_t func_obj = js_func_obj(func);
  jsval_t bound_func = mkobj(js, 0);
  if (is_err(bound_func)) return bound_func;

  jsval_t code_val = get_slot(js, func_obj, SLOT_CODE);
  if (vtype(code_val) == T_STR || vtype(code_val) == T_CFUNC) {
    set_slot(js, bound_func, SLOT_CODE, code_val);
    set_slot(js, bound_func, SLOT_CODE_LEN, get_slot(js, func_obj, SLOT_CODE_LEN));
  }

  sv_closure_t *orig = js_func_closure(func);
  sv_closure_t *bound_closure = calloc(1, sizeof(sv_closure_t));
  bound_closure->func = orig->func;
  bound_closure->upvalues = orig->upvalues;
  bound_closure->bound_this = this_arg;
  bound_closure->func_obj = bound_func;
  bound_closure->call_flags = orig->call_flags;
  if (bound_argc > 0)
    bound_closure->call_flags |= SV_CALL_HAS_BOUND_ARGS;

  jsval_t async_slot = get_slot(js, func_obj, SLOT_ASYNC);
  if (vtype(async_slot) == T_BOOL && vdata(async_slot) == 1) {
    set_slot(js, bound_func, SLOT_ASYNC, js_true);
  }

  jsval_t target_proto = get_proto(js, func);
  if (is_object_type(target_proto)) {
    set_proto(js, bound_func, target_proto);
  } else if (vtype(async_slot) == T_BOOL && vdata(async_slot) == 1) {
    jsval_t async_proto = get_slot(js, js_glob(js), SLOT_ASYNC_PROTO);
    if (vtype(async_proto) == T_FUNC) set_proto(js, bound_func, async_proto);
  } else {
    jsval_t func_proto = get_slot(js, js_glob(js), SLOT_FUNC_PROTO);
    if (vtype(func_proto) == T_FUNC) set_proto(js, bound_func, func_proto);
  }

  jsval_t data_slot = get_slot(js, func_obj, SLOT_DATA);
  if (vtype(data_slot) != T_UNDEF) {
    set_slot(js, bound_func, SLOT_DATA, data_slot);
  }

  set_slot(js, bound_func, SLOT_TARGET_FUNC, func);
  set_slot(js, bound_func, SLOT_BOUND_THIS, this_arg);
  
  if (bound_argc > 0) {
    jsval_t bound_arr = mkarr(js);
    for (int i = 0; i < bound_argc; i++) arr_set(js, bound_arr, (jsoff_t)i, bound_args[i]);
    set_slot(js, bound_func, SLOT_BOUND_ARGS, bound_arr);
  }

  jsval_t cfunc_slot = get_slot(js, func_obj, SLOT_CFUNC);
  if (vtype(cfunc_slot) == T_CFUNC) {
    set_slot(js, bound_func, SLOT_CFUNC, cfunc_slot);
  }
  
  js_setprop(js, bound_func, js->length_str, tov((double) bound_length));
  
  jsval_t bound = mkval(T_FUNC, (uintptr_t)bound_closure);  
  jsval_t proto_setup = setup_func_prototype(js, bound);
  if (is_err(proto_setup)) return proto_setup;
  
  return bound;
}

static jsval_t builtin_Array(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = mkarr(js);
  if (is_err(arr)) return arr;
  
  if (nargs == 1 && vtype(args[0]) == T_NUM) {
    jsval_t err = validate_array_length(js, args[0]);
    if (is_err(err)) return err;
    jsoff_t new_len = (jsoff_t)tod(args[0]);
    jsoff_t doff = get_dense_buf(js, arr);
    if (doff && new_len <= 1024) {
      if (new_len > dense_capacity(js, doff)) doff = dense_grow(js, arr, new_len);
      if (doff) dense_set_length(js, doff, new_len);
    }
    update_array_length(js, arr, new_len);
  } else if (nargs > 0) {
    for (int i = 0; i < nargs; i++) arr_set(js, arr, (jsoff_t)i, args[i]);
  }

  jsval_t array_proto = get_ctor_proto(js, "Array", 5);
  jsval_t instance_proto = js_instance_proto_from_new_target(js, array_proto);
  
  if (is_object_type(instance_proto)) set_proto(js, arr, instance_proto);
  if (vtype(js->new_target) == T_FUNC || vtype(js->new_target) == T_CFUNC) {
    set_slot(js, arr, SLOT_CTOR, js->new_target);
  }
  
  return arr;
}

static jsval_t builtin_Error(ant_t *js, jsval_t *args, int nargs) {
  bool is_new = (vtype(js->new_target) != T_UNDEF);
  jsval_t this_val = js->this_val;
  
  jsval_t target = is_new ? js->new_target : js->current_func;
  jsval_t name = ANT_STRING("Error");
  
  if (vtype(target) == T_FUNC) {
    jsoff_t off = lkp(js, js_func_obj(target), "name", 4);
    if (off) name = resolveprop(js, mkval(T_PROP, off));
  }

  if (!is_new) {
    this_val = js_mkobj(js);
    jsoff_t proto_off = lkp_interned(js, js_func_obj(js->current_func), INTERN_PROTOTYPE, 9);
    if (proto_off) set_proto(js, this_val, resolveprop(js, mkval(T_PROP, proto_off)));
    else set_proto(js, this_val, get_ctor_proto(js, "Error", 5));
  }
  
  if (nargs > 0) {
    jsval_t msg = args[0];
    if (vtype(msg) != T_STR) {
      const char *str = js_str(js, msg);
      msg = js_mkstr(js, str, strlen(str));
    }
    js_mkprop_fast(js, this_val, "message", 7, msg);
  }
  
  if (nargs > 1 && vtype(args[1]) == T_OBJ) {
    jsoff_t cause_off = lkp(js, args[1], "cause", 5);
    if (cause_off) js_mkprop_fast(js, this_val, "cause", 5, resolveprop(js, mkval(T_PROP, cause_off)));
  }
  
  js_mkprop_fast(js, this_val, "name", 4, name);
  set_slot(js, this_val, SLOT_ERROR_BRAND, js_true);

  return this_val;
}

static jsval_t builtin_Error_toString(ant_t *js, jsval_t *args, int nargs) {
  jsval_t this_val = js_getthis(js);
  
  jsval_t name = js_get(js, this_val, "name");
  if (vtype(name) == T_UNDEF) name = js_mkstr(js, "Error", 5);
  else if (vtype(name) != T_STR) {
    const char *s = js_str(js, name);
    name = js_mkstr(js, s, strlen(s));
  }
  
  jsval_t msg = js_get(js, this_val, "message");
  if (vtype(msg) == T_UNDEF) msg = js_mkstr(js, "", 0);
  else if (vtype(msg) != T_STR) {
    const char *s = js_str(js, msg);
    msg = js_mkstr(js, s, strlen(s));
  }
  
  jsoff_t name_len, msg_len;
  jsoff_t name_off = vstr(js, name, &name_len);
  jsoff_t msg_off = vstr(js, msg, &msg_len);
  
  const char *name_str = (const char *)&js->mem[name_off];
  const char *msg_str = (const char *)&js->mem[msg_off];
  
  if (name_len == 0) return msg;
  if (msg_len == 0) return name;

  size_t total = (size_t)(name_len + 2 + msg_len);
  char *buf = malloc(total + 1);
  if (!buf) return js_mkerr(js, "out of memory");

  memcpy(buf, name_str, (size_t)name_len);
  buf[name_len] = ':';
  buf[name_len + 1] = ' ';
  memcpy(buf + name_len + 2, msg_str, (size_t)msg_len);
  buf[total] = '\0';
  
  jsval_t result = js_mkstr(js, buf, total);
  free(buf);
  return result;
}

static jsval_t builtin_AggregateError(ant_t *js, jsval_t *args, int nargs) {
  bool is_new = (vtype(js->new_target) != T_UNDEF);
  jsval_t this_val = js->this_val;
  
  if (!is_new) {
    this_val = js_mkobj(js);
    jsoff_t proto_off = lkp_interned(js, js_func_obj(js->current_func), INTERN_PROTOTYPE, 9);
    if (proto_off) set_proto(js, this_val, resolveprop(js, mkval(T_PROP, proto_off)));
    else set_proto(js, this_val, get_ctor_proto(js, "AggregateError", 14));
  }
  
  jsval_t errors = nargs > 0 ? args[0] : mkarr(js);
  if (vtype(errors) != T_ARR) errors = mkarr(js);
  js_mkprop_fast(js, this_val, "errors", 6, errors);
  
  if (nargs > 1 && vtype(args[1]) != T_UNDEF) {
    jsval_t msg = args[1];
    if (vtype(msg) != T_STR) {
      const char *str = js_str(js, msg);
      msg = js_mkstr(js, str, strlen(str));
    }
    js_mkprop_fast(js, this_val, "message", 7, msg);
  }
  
  if (nargs > 2 && vtype(args[2]) == T_OBJ) {
    jsoff_t cause_off = lkp(js, args[2], "cause", 5);
    if (cause_off) js_mkprop_fast(js, this_val, "cause", 5, resolveprop(js, mkval(T_PROP, cause_off)));
  }
  
  js_mkprop_fast(js, this_val, "name", 4, ANT_STRING("AggregateError"));
  set_slot(js, this_val, SLOT_ERROR_BRAND, js_true);

  return this_val;
}

static jsval_t builtin_Math_abs(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(fabs(x));
}

static jsval_t builtin_Math_acos(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(acos(x));
}

static jsval_t builtin_Math_acosh(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(acosh(x));
}

static jsval_t builtin_Math_asin(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(asin(x));
}

static jsval_t builtin_Math_asinh(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(asinh(x));
}

static jsval_t builtin_Math_atan(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(atan(x));
}

static jsval_t builtin_Math_atanh(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(atanh(x));
}

static jsval_t builtin_Math_atan2(ant_t *js, jsval_t *args, int nargs) {
  double y = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  double x = (nargs < 2) ? JS_NAN : js_to_number(js, args[1]);
  if (isnan(y) || isnan(x)) return tov(JS_NAN);
  return tov(atan2(y, x));
}

static jsval_t builtin_Math_cbrt(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(cbrt(x));
}

static jsval_t builtin_Math_ceil(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(ceil(x));
}

static jsval_t builtin_Math_clz32(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return tov(32);
  double x = js_to_number(js, args[0]);
  if (isnan(x) || isinf(x)) return tov(32);
  uint32_t n = (uint32_t) x;
  if (n == 0) return tov(32);
  int count = 0;
  while ((n & 0x80000000U) == 0) { count++; n <<= 1; }
  return tov((double) count);
}

static jsval_t builtin_Math_cos(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(cos(x));
}

static jsval_t builtin_Math_cosh(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(cosh(x));
}

static jsval_t builtin_Math_exp(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(exp(x));
}

static jsval_t builtin_Math_expm1(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(expm1(x));
}

static jsval_t builtin_Math_floor(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(floor(x));
}

static jsval_t builtin_Math_fround(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov((double)(float)x);
}

static jsval_t builtin_Math_hypot(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return tov(0.0);
  double sum = 0.0;
  for (int i = 0; i < nargs; i++) {
    double v = js_to_number(js, args[i]);
    if (isnan(v)) return tov(JS_NAN);
    sum += v * v;
  }
  return tov(sqrt(sum));
}

static int32_t toInt32(double d) {
  if (isnan(d) || isinf(d) || d == 0) return 0;
  double int_val = trunc(d);
  double two32 = (double)(1ULL << 32);
  double two31 = (double)(1ULL << 31);
  double mod_val = fmod(int_val, two32);
  if (mod_val < 0) mod_val += two32;
  if (mod_val >= two31) mod_val -= two32;
  return (int32_t)mod_val;
}

static jsval_t builtin_Math_imul(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 2) return tov(0);
  int32_t a = toInt32(js_to_number(js, args[0]));
  int32_t b = toInt32(js_to_number(js, args[1]));
  return tov((double)((int32_t)((uint32_t)a * (uint32_t)b)));
}

static jsval_t builtin_Math_log(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(log(x));
}

static jsval_t builtin_Math_log1p(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(log1p(x));
}

static jsval_t builtin_Math_log10(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(log10(x));
}

static jsval_t builtin_Math_log2(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(log2(x));
}

static jsval_t builtin_Math_max(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return tov(JS_NEG_INF);
  double max_val = JS_NEG_INF;
  for (int i = 0; i < nargs; i++) {
    double v = js_to_number(js, args[i]);
    if (isnan(v)) return tov(JS_NAN);
    if (v > max_val) max_val = v;
  }
  return tov(max_val);
}

static jsval_t builtin_Math_min(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return tov(JS_INF);
  double min_val = JS_INF;
  for (int i = 0; i < nargs; i++) {
    double v = js_to_number(js, args[i]);
    if (isnan(v)) return tov(JS_NAN);
    if (v < min_val) min_val = v;
  }
  return tov(min_val);
}

static jsval_t builtin_Math_pow(ant_t *js, jsval_t *args, int nargs) {
  double base = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  double exp = (nargs < 2) ? JS_NAN : js_to_number(js, args[1]);
  if (isnan(base) || isnan(exp)) return tov(JS_NAN);
  return tov(pow(base, exp));
}

static bool random_seeded = false;

static jsval_t builtin_Math_random(ant_t *js, jsval_t *args, int nargs) {
  if (!random_seeded) {
    srand((unsigned int) time(NULL));
    random_seeded = true;
  }
  return tov((double) rand() / ((double) RAND_MAX + 1.0));
}

static jsval_t builtin_Math_round(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x) || isinf(x)) return tov(x);
  return tov(floor(x + 0.5));
}

static jsval_t builtin_Math_sign(ant_t *js, jsval_t *args, int nargs) {
  double v = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(v)) return tov(JS_NAN);
  if (v > 0) return tov(1.0);
  if (v < 0) return tov(-1.0);
  return tov(v);
}

static jsval_t builtin_Math_sin(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(sin(x));
}

static jsval_t builtin_Math_sinh(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(sinh(x));
}

static jsval_t builtin_Math_sqrt(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(sqrt(x));
}

static jsval_t builtin_Math_tan(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(tan(x));
}

static jsval_t builtin_Math_tanh(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(tanh(x));
}

static jsval_t builtin_Math_trunc(ant_t *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(trunc(x));
}

typedef jsval_t (*dynamic_kv_mapper_fn)(
  ant_t *js,
  jsval_t key,
  jsval_t val
);

static jsval_t iterate_dynamic_keys(ant_t *js, jsval_t obj, dynamic_accessors_t *acc, dynamic_kv_mapper_fn mapper) {
  jsval_t keys_arr = acc->keys(js, obj);
  jsval_t arr = mkarr(js);
  jsoff_t len = get_array_length(js, keys_arr);
  
  for (jsoff_t i = 0; i < len; i++) {
    jsval_t key_val = arr_get(js, keys_arr, i);
    if (vtype(key_val) != T_STR) continue;
    jsoff_t klen; jsoff_t str_off = vstr(js, key_val, &klen);
    const char *key = (const char *)&js->mem[str_off];
    jsval_t val = acc->getter(js, obj, key, klen);
    js_arr_push(js, arr, mapper ? mapper(js, key_val, val) : val);
  }
  
  return mkval(T_ARR, vdata(arr));
}

static jsval_t builtin_object_is(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_false;
  
  jsval_t x = args[0];
  jsval_t y = args[1];
  
  uint8_t tx = vtype(x);
  uint8_t ty = vtype(y);
  if (tx != ty) return js_false;
  
  if (tx == T_UNDEF || tx == T_NULL) return js_true;
  
  if (tx == T_NUM) {
    double dx = tod(x);
    double dy = tod(y);
    if (isnan(dx) && isnan(dy)) return js_true;
    if (dx == 0.0 && dy == 0.0) {
      bool x_neg = (1.0 / dx) < 0;
      bool y_neg = (1.0 / dy) < 0;
      return x_neg == y_neg ? js_true : js_false;
    }
    return dx == dy ? js_true : js_false;
  }
  
  if (tx == T_BOOL) return vdata(x) == vdata(y) ? js_true : js_false;
  
  return x == y ? js_true : js_false;
}

enum obj_enum_mode { 
  OBJ_ENUM_KEYS,
  OBJ_ENUM_VALUES,
  OBJ_ENUM_ENTRIES
};

static jsval_t map_to_entry(ant_t *js, jsval_t key, jsval_t val) {
  jsval_t pair = mkarr(js);
  arr_set(js, pair, 0, key);
  arr_set(js, pair, 1, val);
  return mkval(T_ARR, vdata(pair));
}

static jsval_t object_enum(ant_t *js, jsval_t obj, enum obj_enum_mode mode) {
  bool is_arr = (vtype(obj) == T_ARR);
  if (vtype(obj) == T_FUNC) obj = js_func_obj(obj);
  
  jsoff_t obj_off = (jsoff_t)vdata(obj);
  dynamic_accessors_t *acc = NULL;
  HASH_FIND(hh, accessor_registry, &obj_off, sizeof(jsoff_t), acc);
  if (acc && acc->keys) {
    if (mode == OBJ_ENUM_KEYS && !acc->getter) return acc->keys(js, obj);
    if (acc->getter) {
      dynamic_kv_mapper_fn mapper = (mode == OBJ_ENUM_ENTRIES) ? map_to_entry : NULL;
      return iterate_dynamic_keys(js, obj, acc, mapper);
    }
  }
  
  jsval_t arr = mkarr(js);
  jsoff_t idx = 0;
  
  if (is_arr) {
    jsoff_t doff = get_dense_buf_off(js, obj_off);
    if (doff) {
      jsoff_t dense_len = dense_length(js, doff);
      for (jsoff_t i = 0; i < dense_len; i++) {
        jsval_t v = dense_get(js, doff, i);
        if (is_empty_slot(v)) continue;
        char idxstr[16]; size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
        jsval_t key_val = js_mkstr(js, idxstr, idxlen);
        if (mode == OBJ_ENUM_KEYS) arr_set(js, arr, idx, key_val);
        else if (mode == OBJ_ENUM_VALUES) arr_set(js, arr, idx, v);
        else arr_set(js, arr, idx, map_to_entry(js, key_val, v));
        idx++;
      }
    }
  }
  
  jsoff_t next = loadoff(js, obj_off) & ~(3U | FLAGMASK);
  
  while (next < js->brk && next != 0) {
    jsoff_t header = loadoff(js, next);
    if (is_slot_prop(header)) { next = next_prop(header); continue; }
    if (is_sym_key_prop(js, next)) { next = next_prop(header); continue; }
    
    jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
    jsoff_t klen = offtolen(loadoff(js, koff));
    const char *key = (char *) &js->mem[koff + sizeof(koff)];
    jsval_t val = loadval(js, next + (jsoff_t) (sizeof(next) + sizeof(koff)));
    
    next = next_prop(header);
    if (is_internal_prop(key, klen)) continue;
    if (is_arr && is_array_index(key, klen)) {
      jsoff_t doff = get_dense_buf_off(js, obj_off);
      if (doff) {
        unsigned long pidx = 0;
        for (jsoff_t ci = 0; ci < klen; ci++) pidx = pidx * 10 + (key[ci] - '0');
        if (pidx < dense_length(js, doff)) continue;
      }
    }
    
    bool should_include = true;
    descriptor_entry_t *desc = lookup_descriptor(js, obj_off, key, klen);
    if (desc) should_include = desc->enumerable;
    
    if (should_include) {
      jsval_t key_val = js_mkstr(js, key, klen);
      if (mode == OBJ_ENUM_KEYS) arr_set(js, arr, idx, key_val);
      else if (mode == OBJ_ENUM_VALUES) arr_set(js, arr, idx, val);
      else arr_set(js, arr, idx, map_to_entry(js, key_val, val));
      idx++;
    }
  }
  
  return mkval(T_ARR, vdata(arr));
}

static jsval_t builtin_object_keys(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkarr(js);
  jsval_t obj = args[0];
  if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR && vtype(obj) != T_FUNC) return mkarr(js);
  
  if (is_proxy(js, obj)) {
    proxy_data_t *data = get_proxy_data(obj);
    if (!data) return mkarr(js);
    if (data->revoked)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot perform 'ownKeys' on a proxy that has been revoked");
    
    jsoff_t trap_off = lkp(js, data->handler, "ownKeys", 7);
    if (!trap_off) return object_enum(js, data->target, OBJ_ENUM_KEYS);
    
    jsval_t trap = resolveprop(js, mkval(T_PROP, trap_off));
    uint8_t ft = vtype(trap);
    if (ft != T_FUNC && ft != T_CFUNC) return object_enum(js, data->target, OBJ_ENUM_KEYS);
    
    jsval_t trap_args[1] = { data->target };
    jsval_t result = sv_vm_call(js->vm, js, trap, data->handler, trap_args, 1, NULL, false);
    if (is_err(result)) return result;
    if (vtype(result) != T_ARR)
      return js_mkerr_typed(js, JS_ERR_TYPE, "ownKeys trap must return an array");
    
    jsoff_t len = get_array_length(js, result);
    for (jsoff_t i = 0; i < len; i++) {
      jsval_t ki = arr_get(js, result, i);
      if (vtype(ki) != T_STR && vtype(ki) != T_SYMBOL)
        return js_mkerr_typed(js, JS_ERR_TYPE, "ownKeys trap result must contain only strings or symbols");
      jsoff_t ki_len; jsoff_t ki_off = vstr(js, ki, &ki_len);
      for (jsoff_t j = 0; j < i; j++) {
        jsval_t kj = arr_get(js, result, j);
        jsoff_t kj_len; jsoff_t kj_off = vstr(js, kj, &kj_len);
        if (ki_len == kj_len && memcmp(&js->mem[ki_off], &js->mem[kj_off], ki_len) == 0)
          return js_mkerr_typed(js, JS_ERR_TYPE, "ownKeys trap result must not contain duplicate entries");
      }
    }
    return result;
  }
  
  return object_enum(js, obj, OBJ_ENUM_KEYS);
}

static jsval_t for_in_keys_add(ant_t *js, jsval_t out, jsval_t seen, jsval_t key) {
  if (vtype(key) != T_STR) return js_mkundef();

  jsoff_t key_len = 0;
  jsoff_t key_off = vstr(js, key, &key_len);
  const char *key_ptr = (const char *)&js->mem[key_off];

  if (is_internal_prop(key_ptr, key_len))
    return js_mkundef();
  if (lkp(js, seen, key_ptr, key_len) != 0)
    return js_mkundef();

  jsval_t mark = setprop_cstr(js, seen, key_ptr, key_len, js_true);
  if (is_err(mark)) return mark;

  js_arr_push(js, out, key);
  return js_mkundef();
}

static jsval_t for_in_keys_add_string_indices(ant_t *js, jsval_t out, jsval_t seen, jsval_t str) {
  jsoff_t slen = vstrlen(js, str);
  for (jsoff_t i = 0; i < slen; i++) {
    char idx[16];
    size_t idx_len = uint_to_str(idx, sizeof(idx), (uint64_t)i);
    jsval_t key = js_mkstr(js, idx, idx_len);
    jsval_t r = for_in_keys_add(js, out, seen, key);
    if (is_err(r)) return r;
  }
  return js_mkundef();
}

jsval_t js_for_in_keys(ant_t *js, jsval_t obj) {
  uint8_t t = vtype(obj);
  jsval_t out = mkarr(js);
  if (t == T_NULL || t == T_UNDEF) return out;

  jsval_t seen = mkobj(js, 0);
  jshdl_t h_out = js_root(js, out);
  jshdl_t h_seen = js_root(js, seen);
  jsval_t result = js_mkundef();

  if (t == T_STR) {
    result = for_in_keys_add_string_indices(js, js_deref(js, h_out), js_deref(js, h_seen), obj);
    goto done;
  }

  if (t == T_OBJ) {
    jsval_t prim = get_slot(js, obj, SLOT_PRIMITIVE);
    if (vtype(prim) == T_STR) {
      result = for_in_keys_add_string_indices(js, js_deref(js, h_out), js_deref(js, h_seen), prim);
      if (is_err(result)) goto done;
    }
  }

  if (t == T_OBJ || t == T_ARR || t == T_FUNC) {
    jsval_t own_keys = object_enum(js, obj, OBJ_ENUM_KEYS);
    if (is_err(own_keys)) {
      result = own_keys;
      goto done;
    }
    if (vtype(own_keys) == T_ARR) {
      jshdl_t h_own = js_root(js, own_keys);
      jsoff_t own_len = js_arr_len(js, js_deref(js, h_own));
      for (jsoff_t i = 0; i < own_len; i++) {
        jsval_t key = js_arr_get(js, js_deref(js, h_own), i);
        result = for_in_keys_add(js, js_deref(js, h_out), js_deref(js, h_seen), key);
        if (is_err(result)) {
          js_unroot(js, h_own);
          goto done;
        }
      }
      js_unroot(js, h_own);
    }
  }

done:
  if (is_err(result)) {
    js_unroot(js, h_seen);
    js_unroot(js, h_out);
    return result;
  }
  jsval_t ret = js_deref(js, h_out);
  js_unroot(js, h_seen);
  js_unroot(js, h_out);
  return ret;
}

static jsval_t builtin_object_values(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkarr(js);
  jsval_t obj = args[0];
  if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR && vtype(obj) != T_FUNC) return mkarr(js);
  return object_enum(js, obj, OBJ_ENUM_VALUES);
}

static jsval_t builtin_object_entries(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkarr(js);
  jsval_t obj = args[0];
  if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR && vtype(obj) != T_FUNC) return mkarr(js);
  return object_enum(js, obj, OBJ_ENUM_ENTRIES);
}

static jsval_t builtin_object_getPrototypeOf(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_mkerr(js, "Object.getPrototypeOf requires an argument");
  jsval_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t == T_STR || t == T_NUM || t == T_BOOL || t == T_BIGINT) return get_prototype_for_type(js, t);
  if (is_object_type(obj)) return get_proto(js, obj);
  
  return js_mknull();
}

static jsval_t builtin_object_setPrototypeOf(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "Object.setPrototypeOf requires 2 arguments");
  
  jsval_t obj = args[0];
  jsval_t proto = args[1];
  
  uint8_t t = vtype(obj);
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) {
    return js_mkerr(js, "Object.setPrototypeOf: first argument must be an object");
  }
  
  uint8_t pt = vtype(proto);
  if (pt != T_OBJ && pt != T_ARR && pt != T_FUNC && pt != T_NULL) {
    return js_mkerr(js, "Object.setPrototypeOf: prototype must be an object or null");
  }
  
  for (jsval_t cur = proto; pt != T_NULL && vtype(cur) != T_NULL; cur = get_proto(js, cur)) {
    if (vdata(js_as_obj(cur)) == vdata(js_as_obj(obj))) return js_mkerr(js, "Cyclic __proto__ value");
  }
  
  set_proto(js, obj, proto);
  return obj;
}

static jsval_t builtin_proto_getter(ant_t *js, jsval_t *args, int nargs) {
  jsval_t this_val = js->this_val;
  uint8_t t = vtype(this_val);
  
  if (t == T_UNDEF || t == T_NULL) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot read property '__proto__' of %s", typestr(t));
  }
  
  if (t == T_OBJ || t == T_ARR || t == T_FUNC) {
    return get_proto(js, this_val);
  }
  
  return get_prototype_for_type(js, t);
}

static jsval_t builtin_proto_setter(ant_t *js, jsval_t *args, int nargs) {
  jsval_t this_val = js->this_val;
  uint8_t t = vtype(this_val);
  
  if (t == T_UNDEF || t == T_NULL) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot set property '__proto__' of %s", typestr(t));
  }
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) {
    return js_mkundef();
  }
  
  if (nargs == 0) return js_mkundef();
  
  jsval_t proto = args[0];
  uint8_t pt = vtype(proto);
  
  if (pt != T_OBJ && pt != T_ARR && pt != T_FUNC && pt != T_NULL) {
    return js_mkundef();
  }
  
  for (jsval_t cur = proto; pt != T_NULL && vtype(cur) == T_OBJ; cur = get_proto(js, cur)) {
    if (vdata(cur) == vdata(this_val)) return js_mkundef();
  }
  
  set_proto(js, this_val, proto);
  return js_mkundef();
}

static jsval_t builtin_object_create(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_mkerr(js, "Object.create requires a prototype argument");
  
  jsval_t proto = args[0];
  uint8_t pt = vtype(proto);
  
  if (pt != T_OBJ && pt != T_ARR && pt != T_FUNC && pt != T_NULL) {
    return js_mkerr(js, "Object.create: prototype must be an object or null");
  }
  
  jsval_t obj = js_mkobj(js);
  if (pt == T_NULL) {
    set_proto(js, obj, js_mknull());
  } else set_proto(js, obj, proto);
  
  if (nargs >= 2 && vtype(args[1]) == T_OBJ) {
    jsval_t props = args[1];
    jsoff_t next = loadoff(js, (jsoff_t) vdata(props)) & ~(3U | FLAGMASK);
    
    while (next < js->brk && next != 0) {
      jsoff_t header = loadoff(js, next);
      if (is_slot_prop(header)) { next = next_prop(header); continue; }
      
      jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
      jsoff_t klen = offtolen(loadoff(js, koff));
      const char *key = (char *) &js->mem[koff + sizeof(koff)];
      jsval_t descriptor = resolveprop(js, mkval(T_PROP, next));
      
      if (vtype(descriptor) == T_OBJ) {
        jsoff_t val_off = lkp(js, descriptor, "value", 5);
        if (val_off != 0) {
          jsval_t val = resolveprop(js, mkval(T_PROP, val_off));
          jsval_t key_str = js_mkstr(js, key, klen);
          js_setprop(js, obj, key_str, val);
        }
      }
      
      next = next_prop(header);
    }
  }
  
  return obj;
}

static jsval_t builtin_object_hasOwn(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 2) return mkval(T_BOOL, 0);
  
  jsval_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t == T_NULL || t == T_UNDEF) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert undefined or null to object");
  }
  
  jsval_t key = args[1];
  if (vtype(key) != T_STR) {
    key = js_tostring_val(js, key);
    if (is_err(key)) return key;
  }
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return mkval(T_BOOL, 0);
  
  jsval_t as_obj = js_as_obj(obj);
  jsoff_t key_len, key_off = vstr(js, key, &key_len);
  const char *key_str = (char *) &js->mem[key_off];
  
  jsoff_t off = lkp(js, as_obj, key_str, key_len);
  return mkval(T_BOOL, off != 0 ? 1 : 0);
}

static jsval_t builtin_object_groupBy(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr_typed(js, JS_ERR_TYPE, "Object.groupBy requires 2 arguments");
  
  jsval_t items = args[0];
  jsval_t callback = args[1];
  
  if (vtype(callback) != T_FUNC && vtype(callback) != T_CFUNC)
    return js_mkerr_typed(js, JS_ERR_TYPE, "callback is not a function");
  
  jsval_t result = js_mkobj(js);
  set_proto(js, result, js_mknull());
  
  jsoff_t len = get_array_length(js, items);
  for (jsoff_t i = 0; i < len; i++) {
    jsval_t val = arr_get(js, items, i);
    jsval_t cb_args[2] = { val, tov((double)i) };
    jsval_t key = sv_vm_call(js->vm, js, callback, js_mkundef(), cb_args, 2, NULL, false);
    if (is_err(key)) return key;
    
    jsval_t key_str = js_tostring_val(js, key);
    if (is_err(key_str)) return key_str;
    
    jsoff_t klen;
    jsoff_t koff = vstr(js, key_str, &klen);
    const char *kptr = (char *)&js->mem[koff];
    
    jsoff_t grp_off = lkp(js, result, kptr, klen);
    jsval_t group;
    if (grp_off) {
      group = resolveprop(js, mkval(T_PROP, grp_off));
    } else {
      group = mkarr(js);
      js_setprop(js, result, key_str, group);
    }
    js_arr_push(js, group, val);
  }
  
  return result;
}

static jsval_t builtin_object_defineProperty(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 3) return js_mkerr(js, "Object.defineProperty requires 3 arguments");
  
  jsval_t obj = args[0];
  jsval_t prop = args[1];
  jsval_t descriptor = args[2];
  
  uint8_t t = vtype(obj);
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) {
    return js_mkerr(js, "Object.defineProperty called on non-object");
  }
  
  bool sym_key = (vtype(prop) == T_SYMBOL);
  if (!sym_key && vtype(prop) != T_STR) {
    char buf[64];
    size_t len = tostr(js, prop, buf, sizeof(buf));
    prop = js_mkstr(js, buf, len);
  }
  
  if (vtype(descriptor) != T_OBJ) {
    return js_mkerr(js, "Property descriptor must be an object");
  }
  
  jsval_t as_obj = js_as_obj(obj);
  
  jsoff_t prop_len = 0;
  const char *prop_str = NULL;
  jsoff_t sym_off = 0;
  
  if (sym_key) {
    sym_off = (jsoff_t)vdata(prop);
    const char *desc = js_sym_desc(js, prop);
    prop_str = desc ? desc : "symbol";
    prop_len = (jsoff_t)strlen(prop_str);
  } else {
    jsoff_t prop_off = vstr(js, prop, &prop_len);
    prop_str = (char *) &js->mem[prop_off];
    if (streq(prop_str, prop_len, STR_PROTO, STR_PROTO_LEN)) {
      return js_mkerr(js, "Cannot define " STR_PROTO " property");
    }
  }
  
  bool has_value = false, has_get = false, has_set = false, has_writable = false;
  jsval_t value = js_mkundef();
  bool writable = false, enumerable = false, configurable = false;
  
  jsoff_t value_off = lkp(js, descriptor, "value", 5);
  if (value_off != 0) {
    has_value = true;
    value = resolveprop(js, mkval(T_PROP, value_off));
  }
  
  jsoff_t get_off = lkp_interned(js, descriptor, INTERN_GET, 3);
  if (get_off != 0) {
    has_get = true;
    jsval_t getter = resolveprop(js, mkval(T_PROP, get_off));
    if (vtype(getter) != T_FUNC && vtype(getter) != T_UNDEF) {
      return js_mkerr(js, "Getter must be a function");
    }
  }
  
  jsoff_t set_off = lkp_interned(js, descriptor, INTERN_SET, 3);
  if (set_off != 0) {
    has_set = true;
    jsval_t setter = resolveprop(js, mkval(T_PROP, set_off));
    if (vtype(setter) != T_FUNC && vtype(setter) != T_UNDEF) {
      return js_mkerr(js, "Setter must be a function");
    }
  }
  
  if ((has_value || has_writable) && (has_get || has_set)) {
    return js_mkerr(js, "Invalid property descriptor. Cannot both specify accessors and a value or writable attribute");
  }
  
  jsoff_t writable_off = lkp(js, descriptor, "writable", 8);
  if (writable_off != 0) {
    has_writable = true;
    jsval_t w_val = resolveprop(js, mkval(T_PROP, writable_off));
    writable = js_truthy(js, w_val);
  }
  
  jsoff_t enumerable_off = lkp(js, descriptor, "enumerable", 10);
  if (enumerable_off != 0) {
    jsval_t e_val = resolveprop(js, mkval(T_PROP, enumerable_off));
    enumerable = js_truthy(js, e_val);
  }
  
  jsoff_t configurable_off = lkp(js, descriptor, "configurable", 12);
  if (configurable_off != 0) {
    jsval_t c_val = resolveprop(js, mkval(T_PROP, configurable_off));
    configurable = js_truthy(js, c_val);
  }
  
  jsoff_t existing_off = sym_key ? lkp_sym(js, as_obj, sym_off) : lkp(js, as_obj, prop_str, prop_len);
  
  if (existing_off == 0) {
    if (js_truthy(js, get_slot(js, as_obj, SLOT_FROZEN))) 
      return js_mkerr(js, "Cannot define property %.*s, object is not extensible", (int)prop_len, prop_str);
    if (js_truthy(js, get_slot(js, as_obj, SLOT_SEALED)))
      return js_mkerr(js, "Cannot define property %.*s, object is not extensible", (int)prop_len, prop_str);
    if (get_slot(js, as_obj, SLOT_EXTENSIBLE) == js_false)
      return js_mkerr(js, "Cannot define property %.*s, object is not extensible", (int)prop_len, prop_str);
  }
  
  if (has_get || has_set) {
    int desc_flags = 
      (enumerable ? JS_DESC_E : 0) |
      (configurable ? JS_DESC_C : 0);
    
    if (!sym_key) {
      if (has_get && has_set) {
        jsval_t getter = resolveprop(js, mkval(T_PROP, get_off));
        jsval_t setter = resolveprop(js, mkval(T_PROP, set_off));
        js_set_accessor_desc(js, as_obj, prop_str, prop_len, getter, setter, desc_flags);
      } else if (has_get) {
        jsval_t getter = resolveprop(js, mkval(T_PROP, get_off));
        js_set_getter_desc(js, as_obj, prop_str, prop_len, getter, desc_flags);
      } else {
        jsval_t setter = resolveprop(js, mkval(T_PROP, set_off));
        js_set_setter_desc(js, as_obj, prop_str, prop_len, setter, desc_flags);
      }
    }
  } else {
    int desc_flags = 
      (writable ? JS_DESC_W : 0) | 
      (enumerable ? JS_DESC_E : 0) | 
      (configurable ? JS_DESC_C : 0);
      
    if (!sym_key) js_set_descriptor(js, as_obj, prop_str, prop_len, desc_flags);
    
    if (existing_off > 0) {
      bool is_frozen = js_truthy(js, get_slot(js, as_obj, SLOT_FROZEN));
      bool is_nonconfig = is_nonconfig_prop(js, existing_off) || is_frozen;
      bool is_readonly = is_const_prop(js, existing_off) || is_frozen;
      
      if (is_nonconfig) {
        if (configurable) return js_mkerr(js,
          "Cannot redefine property %.*s: cannot change configurable from false to true", 
          (int)prop_len, prop_str
        );
        
        if (is_readonly && has_writable && writable) return js_mkerr(js, 
          "Cannot redefine property %.*s: cannot change writable from false to true", 
          (int)prop_len, prop_str
        );
      }
      
      if (is_readonly && has_value) return js_mkerr(js, "Cannot assign to read-only property '%.*s'", (int)prop_len, prop_str);
      if (has_value) saveval(js, existing_off + sizeof(jsoff_t) * 2, value);
      
      if (!writable || !configurable) {
        jsoff_t head = (jsoff_t) vdata(as_obj);
        jsoff_t firstprop = loadoff(js, head);
        if ((firstprop & ~(3U | FLAGMASK)) == existing_off) {
          jsoff_t flags = 0;
          if (!writable) flags |= CONSTMASK;
          if (!configurable) flags |= NONCONFIGMASK;
          saveoff(js, head, firstprop | flags);
        } else {
          jsoff_t prop_header = loadoff(js, existing_off);
          jsoff_t flags = 0;
          if (!writable) flags |= CONSTMASK;
          if (!configurable) flags |= NONCONFIGMASK;
          saveoff(js, existing_off, prop_header | flags);
        }
      }
    } else {
      if (!has_value) value = js_mkundef();      
      jsval_t prop_key = sym_key ? prop : js_mkstr(js, prop_str, prop_len);
      jsoff_t flags = (writable ? 0 : CONSTMASK) | (configurable ? 0 : NONCONFIGMASK);
      mkprop(js, as_obj, prop_key, value, flags);
    }
  }
  
  return obj;
}

static jsval_t builtin_object_defineProperties(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "Object.defineProperties requires 2 arguments");
  
  jsval_t obj = args[0];
  jsval_t props = args[1];
  
  uint8_t t = vtype(obj);
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) {
    return js_mkerr(js, "Object.defineProperties called on non-object");
  }
  
  if (vtype(props) != T_OBJ) {
    return js_mkerr(js, "Property descriptors must be an object");
  }
  
  jsval_t props_obj = props;
  jsoff_t next = loadoff(js, (jsoff_t) vdata(props_obj)) & ~(3U | FLAGMASK);
  
  while (next < js->brk && next != 0) {
    jsoff_t header = loadoff(js, next);
    if (is_slot_prop(header)) { next = next_prop(header); continue; }
    
    jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
    jsoff_t klen = offtolen(loadoff(js, koff));
    const char *key = (char *) &js->mem[koff + sizeof(koff)];
    jsval_t descriptor = loadval(js, next + (jsoff_t) (sizeof(next) + sizeof(koff)));
    
    next = next_prop(header);
    if (is_internal_prop(key, klen)) continue;
    
    jsval_t prop_key = js_mkstr(js, key, klen);
    jsval_t define_args[3] = { obj, prop_key, descriptor };
    jsval_t result = builtin_object_defineProperty(js, define_args, 3);
    if (is_err(result)) return result;
  }
  
  return obj;
}

static jsval_t builtin_object_assign(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_mkerr(js, "Object.assign requires at least 1 argument");
  
  jsval_t target = args[0];
  uint8_t t = vtype(target);
  
  if (t == T_NULL || t == T_UNDEF) {
    return js_mkerr(js, "Cannot convert undefined or null to object");
  }
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) {
    target = js_mkobj(js);
  }
  
  jsval_t as_obj = js_as_obj(target);
  
  for (int i = 1; i < nargs; i++) {
    jsval_t source = args[i];
    uint8_t st = vtype(source);
    
    if (st == T_NULL || st == T_UNDEF) continue;
    if (st != T_OBJ && st != T_ARR && st != T_FUNC) continue;
    
    jsval_t src_obj = js_as_obj(source);
    jsoff_t next = loadoff(js, (jsoff_t) vdata(src_obj)) & ~(3U | FLAGMASK);
    
    while (next < js->brk && next != 0) {
      jsoff_t header = loadoff(js, next);
      if (is_slot_prop(header)) { next = next_prop(header); continue; }
      
      jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
      jsoff_t klen = offtolen(loadoff(js, koff));
      const char *key = (char *) &js->mem[koff + sizeof(koff)];
      jsval_t val = loadval(js, next + (jsoff_t) (sizeof(next) + sizeof(koff)));
      
      next = next_prop(header);
      if (is_internal_prop(key, klen)) continue;
      
      bool should_copy = true;
      jsoff_t src_obj_off = (jsoff_t)vdata(src_obj);
      
      descriptor_entry_t *desc = lookup_descriptor(js, src_obj_off, key, klen);
      if (desc) should_copy = desc->enumerable;
      
      if (should_copy) {
        jsval_t key_str = js_mkstr(js, key, klen);
        js_setprop(js, as_obj, key_str, val);
      }
    }
  }
  
  return target;
}

static jsval_t builtin_object_freeze(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_mkundef();
  
  jsval_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return obj;
  jsval_t as_obj = js_as_obj(obj);
  
  jsoff_t next = loadoff(js, (jsoff_t) vdata(as_obj)) & ~(3U | FLAGMASK);
  
  while (next < js->brk && next != 0) {
    jsoff_t header = loadoff(js, next);
    if (is_slot_prop(header)) { next = next_prop(header); continue; }
    
    jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
    jsoff_t klen = offtolen(loadoff(js, koff));
    const char *key = (char *) &js->mem[koff + sizeof(koff)];
    
    jsoff_t cur_prop = next;
    next = next_prop(header);
    if (is_internal_prop(key, klen)) continue;
    
    jsoff_t freeze_flags = CONSTMASK | NONCONFIGMASK;
    jsoff_t head = (jsoff_t) vdata(as_obj);
    jsoff_t firstprop = loadoff(js, head);
    if ((firstprop & ~(3U | FLAGMASK)) == cur_prop) {
      saveoff(js, head, firstprop | freeze_flags);
    } else {
      jsoff_t prop_header = loadoff(js, cur_prop);
      saveoff(js, cur_prop, prop_header | freeze_flags);
    }
    
    js_set_descriptor(js, as_obj, key, klen, JS_DESC_E);
  }
  
  set_slot(js, as_obj, SLOT_FROZEN, js_true);
  return obj;
}

static jsval_t builtin_object_isFrozen(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_true;
  
  jsval_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return js_true;
  jsval_t as_obj = js_as_obj(obj);
  
  return js_bool(js_truthy(js, get_slot(js, as_obj, SLOT_FROZEN)));
}

static jsval_t builtin_object_seal(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_mkundef();
  
  jsval_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return obj;
  jsval_t as_obj = js_as_obj(obj);
  
  set_slot(js, as_obj, SLOT_SEALED, js_true);
  jsoff_t next = loadoff(js, (jsoff_t) vdata(as_obj)) & ~(3U | FLAGMASK);
  
  while (next < js->brk && next != 0) {
    jsoff_t header = loadoff(js, next);
    if (is_slot_prop(header)) { next = next_prop(header); continue; }
    
    jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
    jsoff_t klen = offtolen(loadoff(js, koff));
    const char *key = (char *) &js->mem[koff + sizeof(koff)];
    
    jsoff_t cur_prop = next;
    next = next_prop(header);
    
    if (is_internal_prop(key, klen)) continue;
    
    jsoff_t head = (jsoff_t) vdata(as_obj);
    jsoff_t firstprop = loadoff(js, head);
    if ((firstprop & ~(3U | FLAGMASK)) == cur_prop) {
      saveoff(js, head, firstprop | NONCONFIGMASK);
    } else {
      jsoff_t prop_header = loadoff(js, cur_prop);
      saveoff(js, cur_prop, prop_header | NONCONFIGMASK);
    }
    
    js_set_descriptor(js, as_obj, key, klen, JS_DESC_W | JS_DESC_E);
  }
  
  return obj;
}

static jsval_t builtin_object_isSealed(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_true;
  
  jsval_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return js_true;
  jsval_t as_obj = js_as_obj(obj);
  
  if (js_truthy(js, get_slot(js, as_obj, SLOT_SEALED))) return js_true;
  if (js_truthy(js, get_slot(js, as_obj, SLOT_FROZEN))) return js_true;
  
  return js_false;
}

static jsval_t builtin_object_fromEntries(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_mkerr(js, "Object.fromEntries requires an iterable argument");
  
  jsval_t iterable = args[0];
  uint8_t t = vtype(iterable);
  
  if (t != T_ARR && t != T_OBJ) {
    return js_mkerr(js, "Object.fromEntries requires an iterable");
  }
  
  jsval_t result = js_mkobj(js);
  jsoff_t len = get_array_length(js, iterable);
  if (len == 0) return result;
  
  for (jsoff_t i = 0; i < len; i++) {
    jsval_t entry = arr_get(js, iterable, i);
    if (vtype(entry) != T_ARR && vtype(entry) != T_OBJ) continue;
    
    jsval_t key = arr_get(js, entry, 0);
    if (is_undefined(key)) continue;
    jsval_t val = arr_get(js, entry, 1);
    
    if (vtype(key) != T_STR) {
      char buf[64];
      size_t n = tostr(js, key, buf, sizeof(buf));
      key = js_mkstr(js, buf, n);
    }
    
    js_setprop(js, result, key, val);
  }
  
  return result;
}

static jsval_t builtin_object_getOwnPropertyDescriptor(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  
  jsval_t obj = args[0];
  jsval_t key = args[1];
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return js_mkundef();
  
  const char *key_str;
  jsoff_t key_len;
  
  bool is_sym = (vtype(key) == T_SYMBOL);
  if (is_sym) {
    const char *d = js_sym_desc(js, key);
    key_str = d ? d : "symbol";
    key_len = (jsoff_t)strlen(key_str);
  } else if (vtype(key) == T_STR) {
    jsoff_t key_off = vstr(js, key, &key_len);
    key_str = (char *) &js->mem[key_off];
  } else {
    char buf[64];
    size_t n = tostr(js, key, buf, sizeof(buf));
    key = js_mkstr(js, buf, n);
    jsoff_t key_off = vstr(js, key, &key_len);
    key_str = (char *) &js->mem[key_off];
  }
  
  jsval_t as_obj = js_as_obj(obj);
  jsoff_t obj_off = (jsoff_t)vdata(as_obj);
  
  descriptor_entry_t *desc = is_sym 
    ? lookup_sym_descriptor(obj_off, (jsoff_t)vdata(key)) 
    : lookup_descriptor(js, obj_off, key_str, key_len);
  
  jsoff_t prop_off = is_sym ? lkp_sym(js, as_obj, (jsoff_t)vdata(key)) : lkp(js, as_obj, key_str, key_len);
  if (prop_off == 0 && !desc) {
    return js_mkundef();
  }
  
  jsval_t result = js_mkobj(js);
  
  if (desc && (desc->has_getter || desc->has_setter)) {
    if (desc->has_getter) {
      js_setprop(js, result, js_mkstr(js, "get", 3), desc->getter);
    }
    if (desc->has_setter) {
      js_setprop(js, result, js_mkstr(js, "set", 3), desc->setter);
    }
    js_setprop(js, result, js_mkstr(js, "enumerable", 10), js_bool(desc->enumerable));
    js_setprop(js, result, js_mkstr(js, "configurable", 12), js_bool(desc->configurable));
  } else {
    if (prop_off != 0) {
      jsval_t prop_val = resolveprop(js, mkval(T_PROP, prop_off));
      js_setprop(js, result, js_mkstr(js, "value", 5), prop_val);
    }
    js_setprop(js, result, js_mkstr(js, "writable", 8), desc ? (js_bool(desc->writable)) : js_true);
    js_setprop(js, result, js_mkstr(js, "enumerable", 10), desc ? (js_bool(desc->enumerable)) : js_true);
    js_setprop(js, result, js_mkstr(js, "configurable", 12), desc ? (js_bool(desc->configurable)) : js_true);
  }
  
  return result;
  
  return js_mkundef();
}

static inline bool own_prop_names_is_dense_shadow(
  ant_t *js, jsoff_t obj_off,
  const char *key, jsoff_t key_len
) {
  jsoff_t doff = get_dense_buf_off(js, obj_off);
  if (!doff) return false;
  
  jsoff_t dense_len = dense_length(js, doff);
  if (dense_len <= 0 || !is_array_index(key, key_len)) return false;
  
  unsigned long dense_idx = 0;
  return parse_array_index(key, (size_t)key_len, dense_len, &dense_idx);
}

static jsval_t builtin_object_getOwnPropertyNames(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkarr(js);
  jsval_t obj = args[0];
  
  if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR && vtype(obj) != T_FUNC) return mkarr(js);
  if (vtype(obj) == T_FUNC) obj = js_func_obj(obj);
  
  jsoff_t obj_off = (jsoff_t)vdata(obj);
  bool is_arr_obj = (vtype(obj) == T_ARR);
  
  jsval_t arr = mkarr(js);
  jsoff_t idx = 0;
  
  if (is_arr_obj) {
  for (jsoff_t i = 0;; i++) {
    jsoff_t doff = get_dense_buf_off(js, obj_off);
    if (!doff) break;
    
    jsoff_t dense_len = dense_length(js, doff);
    if (i >= dense_len) break;

    jsval_t v = dense_get(js, doff, i);
    if (is_empty_slot(v)) continue;
    
    char idxstr[16]; size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
    arr_set(js, arr, idx++, js_mkstr(js, idxstr, idxlen));
  }}
  
  jsoff_t next = loadoff(js, obj_off) & ~(3U | FLAGMASK);
  while (next < js->brk && next != 0) {
    jsoff_t prop = next;
    jsoff_t header = loadoff(js, prop);
    next = next_prop(header);
    if (is_slot_prop(header) || is_sym_key_prop(js, prop)) continue;

    jsoff_t koff = loadoff(js, prop + (jsoff_t)sizeof(prop));
    jsoff_t klen = offtolen(loadoff(js, koff));
    const char *key = (char *)&js->mem[koff + sizeof(koff)];

    if (is_internal_prop(key, klen)) continue;
    if (is_arr_obj && own_prop_names_is_dense_shadow(js, obj_off, key, klen)) continue;
    arr_set(js, arr, idx++, js_mkstr(js, key, klen));
  }
  
  if (is_arr_obj) arr_set(js, arr, idx++, js->length_str);
  return mkval(T_ARR, vdata(arr));
}

static jsval_t builtin_object_getOwnPropertySymbols(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkarr(js);
  jsval_t obj = args[0];
  
  uint8_t t = vtype(obj);
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return mkarr(js);
  if (t == T_FUNC) obj = js_func_obj(obj);
  
  jsval_t arr = mkarr(js); jsoff_t idx = 0;
  jsoff_t next = loadoff(js, (jsoff_t)vdata(obj)) & ~(3U | FLAGMASK);
  
  while (next < js->brk && next != 0) {
    jsoff_t header = loadoff(js, next);
    if (!is_slot_prop(header) && is_sym_key_prop(js, next)) {
      arr_set(js, arr, idx++, mkval(T_SYMBOL, get_prop_koff(js, next)));
    }
    next = next_prop(header);
  }
  
  return mkval(T_ARR, vdata(arr));
}

static jsval_t builtin_object_isExtensible(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_true;
  
  jsval_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return js_true;
  jsval_t as_obj = js_as_obj(obj);
  
  if (js_truthy(js, get_slot(js, as_obj, SLOT_FROZEN))) return js_false;
  if (js_truthy(js, get_slot(js, as_obj, SLOT_SEALED))) return js_false;
  
  jsval_t ext_slot = get_slot(js, as_obj, SLOT_EXTENSIBLE);
  if (vtype(ext_slot) != T_UNDEF) return js_bool(js_truthy(js, ext_slot));
  
  return js_true;
}

static jsval_t builtin_object_preventExtensions(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_mkundef();
  
  jsval_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return obj;
  jsval_t as_obj = js_as_obj(obj);
  
  set_slot(js, as_obj, SLOT_EXTENSIBLE, js_false);
  return obj;
}

static jsval_t builtin_object_hasOwnProperty(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return mkval(T_BOOL, 0);
  
  jsval_t obj = js->this_val;
  jsval_t key = args[0];
  
  obj = resolveprop(js, obj);
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return mkval(T_BOOL, 0);
  jsval_t as_obj = js_as_obj(obj);

  if (vtype(key) == T_SYMBOL) {
    jsoff_t off = lkp_sym(js, as_obj, (jsoff_t)vdata(key));
    return mkval(T_BOOL, off != 0 ? 1 : 0);
  }

  const char *key_str = NULL;
  jsoff_t key_len = 0;
  if (vtype(key) != T_STR) {
    char buf[64];
    size_t n = tostr(js, key, buf, sizeof(buf));
    key = js_mkstr(js, buf, n);
  }
  jsoff_t key_off = vstr(js, key, &key_len);
  key_str = (char *) &js->mem[key_off];

  if (t == T_ARR && streq(key_str, key_len, "length", 6)) return mkval(T_BOOL, 1);
  if (t == T_ARR && is_array_index(key_str, key_len)) {
    unsigned long idx;
    if (parse_array_index(key_str, key_len, get_array_length(js, obj), &idx)) {
      return mkval(T_BOOL, arr_has(js, obj, (jsoff_t)idx) ? 1 : 0);
    }
  }

  jsoff_t off = lkp(js, as_obj, key_str, key_len);
  if (off != 0) return mkval(T_BOOL, 1);
  descriptor_entry_t *desc = lookup_descriptor(js, (jsoff_t)vdata(as_obj), key_str, key_len);
  return mkval(T_BOOL, (desc && (desc->has_getter || desc->has_setter)) ? 1 : 0);
}

static jsval_t builtin_object_isPrototypeOf(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return mkval(T_BOOL, 0);
  
  jsval_t proto_obj = resolveprop(js, js->this_val);
  jsval_t obj = args[0];
  
  uint8_t obj_type = vtype(obj);
  if (obj_type != T_OBJ && obj_type != T_ARR && obj_type != T_FUNC) return mkval(T_BOOL, 0);
  
  uint8_t proto_type = vtype(proto_obj);
  if (proto_type != T_OBJ && proto_type != T_ARR && proto_type != T_FUNC) return mkval(T_BOOL, 0);
  jsoff_t proto_data = (jsoff_t)vdata(js_as_obj(proto_obj));
  
  jsval_t current = get_proto(js, obj);
  while (!is_undefined(current) && !is_null(current)) {
    uint8_t cur_type = vtype(current);
    if (cur_type != T_OBJ && cur_type != T_ARR && cur_type != T_FUNC) break;
    if ((jsoff_t)vdata(js_as_obj(current)) == proto_data) return mkval(T_BOOL, 1);
    current = get_proto(js, current);
  }
  
  return mkval(T_BOOL, 0);
}

static jsval_t builtin_object_propertyIsEnumerable(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return mkval(T_BOOL, 0);
  
  jsval_t obj = js->this_val;
  jsval_t key = args[0];
  
  obj = resolveprop(js, obj);
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return mkval(T_BOOL, 0);
  jsval_t as_obj = js_as_obj(obj);

  if (vtype(key) == T_SYMBOL) {
    jsoff_t off = lkp_sym(js, as_obj, (jsoff_t)vdata(key));
    return mkval(T_BOOL, off != 0 ? 1 : 0);
  }

  const char *key_str = NULL;
  jsoff_t key_len = 0;
  if (vtype(key) != T_STR) {
    char buf[64];
    size_t n = tostr(js, key, buf, sizeof(buf));
    key = js_mkstr(js, buf, n);
  }
  jsoff_t key_off = vstr(js, key, &key_len);
  key_str = (char *) &js->mem[key_off];

  if (t == T_ARR && streq(key_str, key_len, "length", 6)) {
    return mkval(T_BOOL, 0);
  }
  
  if (t == T_ARR) {
    jsoff_t doff = get_dense_buf(js, obj);
    if (doff) {
      unsigned long idx;
      if (parse_array_index(key_str, key_len, dense_length(js, doff), &idx)) {
        return mkval(T_BOOL, !is_empty_slot(dense_get(js, doff, (jsoff_t)idx)) ? 1 : 0);
      }
    }
  }
  
  jsoff_t off = lkp(js, as_obj, key_str, key_len);
  if (off == 0) return mkval(T_BOOL, 0);
  
  jsoff_t obj_off = (jsoff_t)vdata(as_obj);
  descriptor_entry_t *desc = lookup_descriptor(js, obj_off, key_str, key_len);
  if (desc) {
    return mkval(T_BOOL, desc->enumerable ? 1 : 0);
  }
  
  return mkval(T_BOOL, 1);
}

static jsval_t builtin_object_toString(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t obj = js->this_val;
  
  obj = resolveprop(js, obj);
  uint8_t t = vtype(obj);
  
  const char *tag = NULL;
  jsoff_t tag_len = 0;

  jsval_t tag_sym = get_toStringTag_sym();
  if (vtype(tag_sym) == T_SYMBOL) {
    jsoff_t sym_off = (jsoff_t)vdata(tag_sym);
    jsoff_t tag_off = 0;
    if (is_object_type(obj)) {
      tag_off = lkp_sym_proto(js, obj, sym_off);
    } else {
      jsval_t proto = get_prototype_for_type(js, t);
      if (is_object_type(proto)) {
        tag_off = lkp_sym_proto(js, proto, sym_off);
      }
    }
    if (tag_off != 0) {
      jsval_t tag_val = resolveprop(js, mkval(T_PROP, tag_off));
      if (vtype(tag_val) == T_STR) {
        jsoff_t str_off = vstr(js, tag_val, &tag_len);
        tag = (const char *)&js->mem[str_off];
      }
    }
  }
  
  if (!tag) {
    if (is_object_type(obj) && get_slot(js, obj, SLOT_ERROR_BRAND) == js_true) {
      tag = "Error"; tag_len = 5;
    } else switch (t) {
      case T_UNDEF:   tag = "Undefined"; tag_len = 9; break;
      case T_NULL:    tag = "Null";      tag_len = 4; break;
      case T_BOOL:    tag = "Boolean";   tag_len = 7; break;
      case T_NUM:     tag = "Number";    tag_len = 6; break;
      case T_STR:     tag = "String";    tag_len = 6; break;
      case T_ARR:     tag = "Array";     tag_len = 5; break;
      case T_FUNC:    tag = "Function";  tag_len = 8; break;
      case T_CFUNC:   tag = "Function";  tag_len = 8; break;
      case T_ERR:     tag = "Error";     tag_len = 5; break;
      case T_BIGINT:  tag = "BigInt";    tag_len = 6; break;
      case T_PROMISE: tag = "Promise";   tag_len = 7; break;
      case T_OBJ:     tag = "Object";    tag_len = 6; break;
      default:        tag = "Unknown";   tag_len = 7; break;
    }
  }

  char static_buf[64];
  string_builder_t sb;
  
  string_builder_init(&sb, static_buf, sizeof(static_buf));
  string_builder_append(&sb, "[object ", 8);
  string_builder_append(&sb, tag, tag_len);
  string_builder_append(&sb, "]", 1);
  
  return string_builder_finalize(js, &sb);
}

static jsval_t builtin_object_valueOf(ant_t *js, jsval_t *args, int nargs) {
  return js->this_val;
}

static jsval_t builtin_object_toLocaleString(ant_t *js, jsval_t *args, int nargs) {
  return js_call_toString(js, js->this_val);
}

static inline bool is_callable(jsval_t v) {
  uint8_t t = vtype(v);
  return t == T_FUNC || t == T_CFUNC;
}

static inline jsval_t require_callback(ant_t *js, jsval_t *args, int nargs, const char *name) {
  if (nargs == 0 || !is_callable(args[0]))
    return js_mkerr(js, "%s requires a function argument", name);
  return args[0];
}

static jsval_t array_shallow_copy(ant_t *js, jsval_t arr, jsoff_t len) {
  jsval_t result = mkarr(js);
  if (is_err(result)) return result;
  
  jsoff_t doff = get_dense_buf(js, arr);
  if (doff) {
    for (jsoff_t i = 0; i < len; i++) {
      jsval_t v = dense_get(js, doff, i);
      arr_set(js, result, i, v);
    }
    return result;
  }
  
  ant_iter_t iter = js_prop_iter_begin(js, arr);
  const char *key;
  size_t key_len;
  jsval_t val;
  
  while (js_prop_iter_next(&iter, &key, &key_len, &val)) {
    if (key_len == 0 || key[0] > '9' || key[0] < '0') continue;
    js_mkprop_fast(js, result, key, key_len, val);
  }
  
  js_prop_iter_end(&iter);
  js_mkprop_fast(js, result, "length", 6, tov((double)len));
  return result;
}

static jsval_t builtin_array_push(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  arr = resolveprop(js, arr);
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "push called on non-array");
  }

  if (is_proxy(js, arr)) {
    jsoff_t off = lkp_interned(js, arr, INTERN_LENGTH, 6);
    jsoff_t len = 0;
    if (off != 0) {
      jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
      if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
    }
    for (int i = 0; i < nargs; i++) {
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)len);
      jsval_t key = js_mkstr(js, idxstr, idxlen);
      js_setprop(js, arr, key, args[i]);
      len++;
    }
    
    jsval_t len_val = tov((double) len);
    js_setprop(js, arr, js->length_str, len_val);
    return len_val;
  }

  jsoff_t len = get_array_length(js, arr);
  
  jsoff_t doff = get_dense_buf(js, arr);
  if (doff) {
    for (int i = 0; i < nargs; i++) {
      jsoff_t cap = dense_capacity(js, doff);
      if (len >= cap) {
        doff = dense_grow(js, arr, len + 1);
        if (doff == 0) return js_mkerr(js, "oom");
      }
      dense_set(js, doff, len, args[i]);
      len++;
      dense_set_length(js, doff, len);
    }
    return tov((double) len);
  }
  
  jsoff_t off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  
  for (int i = 0; i < nargs; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)len);
    js_mkprop_fast(js, arr, idxstr, idxlen, args[i]); len++;
  }

  jsval_t new_len = tov((double) len);
  if (off != 0) saveval(js, off + sizeof(jsoff_t) * 2, new_len);
  else js_mkprop_fast(js, arr, "length", 6, new_len);

  return new_len;
}

void js_arr_push(ant_t *js, jsval_t arr, jsval_t val) {
  arr = resolveprop(js, arr);
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) return;
  
  jsoff_t doff = get_dense_buf(js, arr);
  if (doff) {
    jsoff_t len = dense_length(js, doff);
    jsoff_t cap = dense_capacity(js, doff);
    if (len >= cap) {
      doff = dense_grow(js, arr, len + 1);
      if (doff == 0) return;
    }
    dense_set(js, doff, len, val);
    dense_set_length(js, doff, len + 1);
    return;
  }
  
  jsoff_t len_off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (len_off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, len_off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  char idxstr[16];
  size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)len);
  js_mkprop_fast(js, arr, idxstr, idxlen, val);
  
  if (len_off != 0) saveval(js, len_off + sizeof(jsoff_t) * 2, tov((double)(len + 1)));
  else js_mkprop_fast(js, arr, "length", 6, tov((double)(len + 1)));
}

static jsval_t builtin_array_pop(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;

  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "pop called on non-array");
  }

  if (is_proxy(js, arr)) {
    jsoff_t len = proxy_aware_length(js, arr);
    if (len == 0) {
      js_setprop(js, arr, js->length_str, tov(0.0));
      return js_mkundef();
    }
    len--;
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)len);
    jsval_t result = proxy_aware_get_elem(js, arr, idxstr, idxlen);
    js_setprop(js, arr, js->length_str, tov((double) len));
    js->needs_gc = true;
    return result;
  }

  jsoff_t doff = get_dense_buf(js, arr);
  if (doff) {
    jsoff_t len = dense_length(js, doff);
    if (len == 0) return js_mkundef();
    len--;
    jsval_t result = dense_get(js, doff, len);
    if (is_empty_slot(result)) result = js_mkundef();
    dense_set(js, doff, len, T_EMPTY);
    dense_set_length(js, doff, len);
    return result;
  }

  jsoff_t off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;

  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }

  if (len == 0) return js_mkundef();
  len--; char idxstr[16];
  size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)len);

  jsoff_t arr_off = (jsoff_t)vdata(js_as_obj(arr));
  jsoff_t tail = loadoff(js, arr_off + sizeof(jsoff_t) * 2);
  jsoff_t elem_off = 0;

  if (tail != 0 && tail < js->brk) {
    jsoff_t tail_hdr = loadoff(js, tail);
    if ((tail_hdr & SLOTMASK) == 0) {
      jsoff_t tail_koff = loadoff(js, tail + sizeof(jsoff_t));
      jsoff_t tail_klen = offtolen(loadoff(js, tail_koff));
      const char *tail_key = (char *)&js->mem[tail_koff + sizeof(jsoff_t)];
      if (tail_klen == idxlen && memcmp(tail_key, idxstr, idxlen) == 0) elem_off = tail;
    }
  }

  if (elem_off == 0) elem_off = lkp(js, arr, idxstr, idxlen);
  jsval_t result = js_mkundef();
  if (elem_off != 0) result = resolveprop(js, mkval(T_PROP, elem_off));

  if (off != 0) {
    saveval(js, off + sizeof(jsoff_t) * 2, tov((double) len));
  } else js_setprop(js, arr, js->length_str, tov((double) len));

  js->needs_gc = true;
  return result;
}

static jsval_t builtin_array_slice(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "slice called on non-array");
  }
  
  jsoff_t len = get_array_length(js, arr);
  
  jsoff_t start = 0, end = len;
  double dlen = D(len);
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    double d = tod(args[0]);
    if (d < 0) {
      start = (jsoff_t) (d + dlen < 0 ? 0 : d + dlen);
    } else start = (jsoff_t) (d > dlen ? dlen : d);
  }
  
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    double d = tod(args[1]);
    if (d < 0) {
      end = (jsoff_t) (d + dlen < 0 ? 0 : d + dlen);
    } else {
      end = (jsoff_t) (d > dlen ? dlen : d);
    }
  }
  
  if (start > end) start = end;
  jsval_t result = array_alloc_like(js, arr);
  if (is_err(result)) return result;
  jsoff_t result_idx = 0;
  
  for (jsoff_t i = start; i < end; i++) {
    jsval_t elem = arr_get(js, arr, i);
    arr_set(js, result, result_idx, elem);
    result_idx++;
  }
  
  return result;
}

static jsval_t builtin_array_join(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "join called on non-array");
  }
  const char *sep = ",";
  jsoff_t sep_len = 1;
  
  if (nargs >= 1) {
    if (vtype(args[0]) == T_STR) {
      sep_len = 0;
      jsoff_t sep_off = vstr(js, args[0], &sep_len);
      sep = (const char *) &js->mem[sep_off];
    } else if (vtype(args[0]) != T_UNDEF) {
      const char *sep_str = js_str(js, args[0]);
      sep = sep_str;
      sep_len = (jsoff_t) strlen(sep_str);
    }
  }
  
  jsoff_t len = get_array_length(js, arr);
  
  if (len == 0) return js_mkstr(js, "", 0);
  
  size_t capacity = 1024;
  size_t result_len = 0;
  char *result = (char *)ant_calloc(capacity);
  if (!result) return js_mkerr(js, "oom");
  
  for (jsoff_t i = 0; i < len; i++) {
    if (i > 0) {
      if (result_len + sep_len >= capacity) {
        capacity = (result_len + sep_len + 1) * 2;
        char *new_result = (char *)ant_realloc(result, capacity);
        if (!new_result) return js_mkerr(js, "oom");
        result = new_result;
      }
      memcpy(result + result_len, sep, sep_len);
      result_len += sep_len;
    }
    
    {
      jsval_t elem = arr_get(js, arr, i);
      uint8_t et = vtype(elem);
      if (et == T_NULL || et == T_UNDEF) continue;
      
      const char *elem_str = NULL;
      size_t elem_len = 0;
      char numstr[64];
      jsval_t str_val = js_mkundef();
      
      if (et == T_STR) {
        jsoff_t soff, slen;
        soff = vstr(js, elem, &slen);
        elem_str = (const char *)&js->mem[soff];
        elem_len = slen;
      } else if (et == T_NUM) {
        snprintf(numstr, sizeof(numstr), "%g", tod(elem));
        elem_str = numstr;
        elem_len = strlen(numstr);
      } else if (et == T_BOOL) {
        elem_str = vdata(elem) ? "true" : "false";
        elem_len = strlen(elem_str);
      } else if (et == T_ARR || et == T_OBJ || et == T_FUNC || et == T_BIGINT) {
        str_val = to_string_val(js, elem);
        
        if (is_err(str_val)) {
          free(result);
          return str_val;
        }
        
        if (vtype(str_val) == T_STR) {
          jsoff_t soff, slen;
          soff = vstr(js, str_val, &slen);
          elem_str = (const char *)&js->mem[soff];
          elem_len = slen;
        }
      }

      
      if (elem_str && elem_len > 0) {
        if (result_len + elem_len >= capacity) {
          capacity = (result_len + elem_len + 1) * 2;
          char *new_result = (char *)ant_realloc(result, capacity);
          if (!new_result) { free(result); return js_mkerr(js, "oom"); }
          result = new_result;
        }
        memcpy(result + result_len, elem_str, elem_len);
        result_len += elem_len;
      }
    }
  }
  
  jsval_t ret = js_mkstr(js, result, result_len);
  free(result); return ret;
}

static jsval_t builtin_array_includes(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "includes called on non-array");
  
  if (nargs == 0) return mkval(T_BOOL, 0);
  jsval_t search = args[0];
  
  jsoff_t len = get_array_length(js, arr);
  if (len == 0) return mkval(T_BOOL, 0);
  
  jsoff_t start = 0;
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    int s = (int) tod(args[1]);
    if (s < 0) s = (int)len + s;
    if (s < 0) s = 0;
    start = (jsoff_t) s;
  }
  
  for (jsoff_t i = start; i < len; i++) {
    jsval_t val = arr_get(js, arr, i);
    if (vtype(val) == T_NUM && vtype(search) == T_NUM && isnan(tod(val)) && isnan(tod(search))) return mkval(T_BOOL, 1);
    if (strict_eq_values(js, val, search)) return mkval(T_BOOL, 1);
  }
  
  return mkval(T_BOOL, 0);
}

static jsval_t builtin_array_every(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "every called on non-array");
  
  jsval_t callback = require_callback(js, args, nargs, "every");
  if (is_err(callback)) return callback;
  jsval_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  
  jsoff_t len = get_array_length(js, arr);
  
  for (jsoff_t i = 0; i < len; i++) {
    if (!arr_has(js, arr, i)) continue;
    jsval_t val = arr_get(js, arr, i);
    jsval_t call_args[3] = { val, tov((double)i), arr };
    jsval_t result = sv_vm_call(js->vm, js, callback, this_arg, call_args, 3, NULL, false);
    if (is_err(result)) return result;
    if (!js_truthy(js, result)) return mkval(T_BOOL, 0);
  }
  
  return mkval(T_BOOL, 1);
}

static jsval_t builtin_array_forEach(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "forEach called on non-array");
  
  jsval_t callback = require_callback(js, args, nargs, "forEach");
  if (is_err(callback)) return callback;
  jsval_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  
  jsoff_t len = get_array_length(js, arr);
  
  for (jsoff_t i = 0; i < len; i++) {
    if (!arr_has(js, arr, i)) continue;
    jsval_t val = arr_get(js, arr, i);
    jsval_t call_args[3] = { val, tov((double)i), arr };
    jsval_t result = sv_vm_call(js->vm, js, callback, this_arg, call_args, 3, NULL, false);
    if (is_err(result)) return result;
  }
  
  return js_mkundef();
}

static jsval_t builtin_array_reverse(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t arr = js->this_val;

  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "reverse called on non-array");

  if (is_proxy(js, arr)) {
    jsoff_t len = proxy_aware_length(js, arr);
    if (len <= 1) return arr;
    jsval_t read_from = proxy_read_target(js, arr);
    jsoff_t lower = 0;
    while (lower < len / 2) {
      jsoff_t upper_idx = len - lower - 1;
      bool lower_exists = arr_has(js, read_from, lower);
      bool upper_exists = arr_has(js, read_from, upper_idx);
      jsval_t lower_val = lower_exists ? arr_get(js, read_from, lower) : js_mkundef();
      jsval_t upper_val = upper_exists ? arr_get(js, read_from, upper_idx) : js_mkundef();
      if (lower_exists && upper_exists) {
        char s1[16]; size_t l1 = uint_to_str(s1, sizeof(s1), (unsigned)lower);
        js_setprop(js, arr, js_mkstr(js, s1, l1), upper_val);
        char s2[16]; size_t l2 = uint_to_str(s2, sizeof(s2), (unsigned)upper_idx);
        js_setprop(js, arr, js_mkstr(js, s2, l2), lower_val);
      } else if (upper_exists) {
        char s[16]; size_t l = uint_to_str(s, sizeof(s), (unsigned)lower);
        js_setprop(js, arr, js_mkstr(js, s, l), upper_val);
      } else if (lower_exists) {
        char s[16]; size_t l = uint_to_str(s, sizeof(s), (unsigned)upper_idx);
        js_setprop(js, arr, js_mkstr(js, s, l), lower_val);
      } lower++;
    } return arr;
  }

  jsoff_t len = get_array_length(js, arr);
  if (len <= 1) return arr;

  jsoff_t doff = get_dense_buf(js, arr);
  if (doff) {
    for (jsoff_t i = 0; i < len / 2; i++) {
      jsval_t a = dense_get(js, doff, i);
      jsval_t b = dense_get(js, doff, len - 1 - i);
      dense_set(js, doff, i, b);
      dense_set(js, doff, len - 1 - i, a);
    }
    return arr;
  }

  jsval_t *vals = malloc(len * sizeof(jsval_t));
  jsoff_t *offs = malloc(len * sizeof(jsoff_t));
  if (!vals || !offs) { free(vals); free(offs); return js_mkerr(js, "out of memory"); }

  jsoff_t count = 0;
  ant_iter_t iter = js_prop_iter_begin(js, arr);
  const char *key;
  size_t key_len;
  jsval_t val;

  while (js_prop_iter_next(&iter, &key, &key_len, &val)) {
    unsigned long parsed_idx;
    if (!parse_array_index(key, key_len, len, &parsed_idx) || count >= len) continue;

    vals[count] = val;
    offs[count] = iter.off;
    count++;
  }
  js_prop_iter_end(&iter);

  for (jsoff_t i = 0; i < count / 2; i++) {
    jsval_t tmp = vals[i];
    vals[i] = vals[count - 1 - i];
    vals[count - 1 - i] = tmp;
  }

  for (jsoff_t i = 0; i < count; i++) {
    saveval(js, offs[i] + sizeof(jsoff_t) * 2, vals[i]);
  }

  free(vals);
  free(offs);
  return arr;
}

static jsval_t builtin_array_map(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "map called on non-array");
  
  jsval_t callback = require_callback(js, args, nargs, "map");
  if (is_err(callback)) return callback;
  jsval_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  
  jsoff_t len = get_array_length(js, arr);
  jsval_t result = array_alloc_like(js, arr);
  if (is_err(result)) return result;
  
  for (jsoff_t i = 0; i < len; i++) {
    if (!arr_has(js, arr, i)) continue;
    jsval_t val = arr_get(js, arr, i);
    jsval_t call_args[3] = { val, tov((double)i), arr };
    jsval_t mapped = sv_vm_call(js->vm, js, callback, this_arg, call_args, 3, NULL, false);
    if (is_err(mapped)) return mapped;
    arr_set(js, result, i, mapped);
  }
  
  return result;
}

static jsval_t builtin_array_filter(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "filter called on non-array");
  
  jsval_t callback = require_callback(js, args, nargs, "filter");
  if (is_err(callback)) return callback;
  jsval_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  
  jsoff_t len = get_array_length(js, arr);
  jsval_t result = array_alloc_like(js, arr);
  if (is_err(result)) return result;
  
  jsoff_t result_idx = 0;
  
  for (jsoff_t i = 0; i < len; i++) {
    if (!arr_has(js, arr, i)) continue;
    jsval_t val = arr_get(js, arr, i);
    jsval_t call_args[3] = { val, tov((double)i), arr };
    jsval_t test = sv_vm_call(js->vm, js, callback, this_arg, call_args, 3, NULL, false);
    
    if (is_err(test)) return test;
    if (js_truthy(js, test)) { arr_set(js, result, result_idx, val); result_idx++; }
  }
  
  return result;
}

static jsval_t builtin_array_reduce(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "reduce called on non-array");
  
  jsval_t callback = require_callback(js, args, nargs, "reduce");
  if (is_err(callback)) return callback;
  bool has_initial = (nargs >= 2);
  
  jsoff_t len = get_array_length(js, arr);
  
  jsval_t accumulator = has_initial ? args[1] : js_mkundef();
  bool first = !has_initial;
  
  for (jsoff_t i = 0; i < len; i++) {
    if (!arr_has(js, arr, i)) continue;
    jsval_t val = arr_get(js, arr, i);
    if (first) { accumulator = val; first = false; continue; }
    jsval_t call_args[4] = { accumulator, val, tov((double)i), arr };
    accumulator = sv_vm_call(js->vm, js, callback, js_mkundef(), call_args, 4, NULL, false);
    if (is_err(accumulator)) return accumulator;
  }
  
  if (first) return js_mkerr(js, "reduce of empty array with no initial value");
  return accumulator;
}

static void flat_helper(ant_t *js, jsval_t arr, jsval_t result, jsoff_t *result_idx, int depth) {
  jsoff_t len = get_array_length(js, arr);
  if (len == 0) return;
  
  for (jsoff_t i = 0; i < len; i++) {
    if (!arr_has(js, arr, i)) continue;
    jsval_t val = arr_get(js, arr, i);
    
    if (depth > 0 && (vtype(val) == T_ARR || vtype(val) == T_OBJ)) {
      flat_helper(js, val, result, result_idx, depth - 1);
    } else { arr_set(js, result, *result_idx, val); (*result_idx)++; }
  }
}

static jsval_t builtin_array_flat(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "flat called on non-array");
  }
  
  int depth = 1;
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    depth = (int) tod(args[0]);
    if (depth < 0) depth = 0;
  }
  
  jsval_t result = array_alloc_like(js, arr);
  if (is_err(result)) return result;
  jsoff_t result_idx = 0;
  
  flat_helper(js, arr, result, &result_idx, depth);
  return result;
}

static jsval_t builtin_array_concat(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "concat called on non-array");
  }
  
  jsval_t result = array_alloc_like(js, arr);
  if (is_err(result)) return result;
  
  jsoff_t result_idx = 0;
  for (int a = -1; a < nargs; a++) {
    jsval_t arg = (a < 0) ? arr : args[a];
    bool spreadable = false;
    
    if (vtype(arg) == T_ARR || vtype(arg) == T_OBJ) {
      bool array_default_spreadable = (vtype(arg) == T_ARR);
      if (!array_default_spreadable && is_proxy(js, arg)) {
        jsval_t target = proxy_read_target(js, arg);
        array_default_spreadable = (vtype(target) == T_ARR);
      }
      
      jsval_t spread_val = js_get_sym(js, arg, get_isConcatSpreadable_sym());
      if (is_err(spread_val)) return spread_val;
      if (vtype(spread_val) == T_UNDEF) spreadable = array_default_spreadable;
      else spreadable = js_truthy(js, spread_val);
    }
    
    if (spreadable) {
      jsoff_t arg_len = 0;
      jsval_t len_val = js_get(js, arg, "length");
      if (is_err(len_val)) return len_val;
      if (vtype(len_val) == T_NUM && tod(len_val) > 0) arg_len = (jsoff_t)tod(len_val);
      
      for (jsoff_t i = 0; i < arg_len; i++) {
        char idxstr[32];
        uint_to_str(idxstr, sizeof(idxstr), (uint64_t)i);
        jsval_t elem = js_get(js, arg, idxstr);
        if (is_err(elem)) return elem;
        arr_set(js, result, result_idx, elem);
        result_idx++;
      }
    } else {
      arr_set(js, result, result_idx, arg);
      result_idx++;
    }
  }
  
  return result;
}

static jsval_t builtin_array_at(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "at called on non-array");
  }
  
  if (nargs == 0 || vtype(args[0]) != T_NUM) return js_mkundef();
  
  jsoff_t len = get_array_length(js, arr);
  
  int idx = (int) tod(args[0]);
  if (idx < 0) idx = (int)len + idx;
  if (idx < 0 || (jsoff_t)idx >= len) return js_mkundef();
  
  return arr_get(js, arr, (jsoff_t)idx);
}

static jsval_t builtin_array_fill(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "fill called on non-array");
  }

  jsval_t value = nargs >= 1 ? args[0] : js_mkundef();

  jsoff_t len = proxy_aware_length(js, arr);
  
  jsoff_t start = 0, end = len;
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    int s = (int) tod(args[1]);
    if (s < 0) s = (int)len + s;
    if (s < 0) s = 0;
    start = (jsoff_t) s;
  }
  if (nargs >= 3 && vtype(args[2]) == T_NUM) {
    int e = (int) tod(args[2]);
    if (e < 0) e = (int)len + e;
    if (e < 0) e = 0;
    end = (jsoff_t) e;
  }
  if (start > len) start = len;
  if (end > len) end = len;
  
  for (jsoff_t i = start; i < end; i++) {
    arr_set(js, arr, i, value);
  }
  
  return arr;
}

static jsval_t array_find_impl(ant_t *js, jsval_t *args, int nargs, bool return_index, const char *name) {
  jsval_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "%s called on non-array", name);
  
  jsval_t callback = require_callback(js, args, nargs, name);
  if (is_err(callback)) return callback;
  jsval_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  
  jsoff_t len = get_array_length(js, arr);
  if (len == 0) return return_index ? tov(-1) : js_mkundef();
  
  for (jsoff_t i = 0; i < len; i++) {
    jsval_t val = arr_get(js, arr, i);
    
    jsval_t call_args[3] = { val, tov((double)i), arr };
    jsval_t result = sv_vm_call(js->vm, js, callback, this_arg, call_args, 3, NULL, false);
    
    if (is_err(result)) return result;
    if (js_truthy(js, result)) return return_index ? tov((double)i) : val;
  }
  
  return return_index ? tov(-1) : js_mkundef();
}

static jsval_t builtin_array_find(ant_t *js, jsval_t *args, int nargs) {
  return array_find_impl(js, args, nargs, false, "find");
}

static jsval_t builtin_array_findIndex(ant_t *js, jsval_t *args, int nargs) {
  return array_find_impl(js, args, nargs, true, "findIndex");
}

static jsval_t array_find_last_impl(ant_t *js, jsval_t *args, int nargs, bool return_index, const char *name) {
  jsval_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "%s called on non-array", name);
  
  jsval_t callback = require_callback(js, args, nargs, name);
  if (is_err(callback)) return callback;
  jsval_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  
  jsoff_t len = get_array_length(js, arr);
  if (len == 0) return return_index ? tov(-1) : js_mkundef();
  
  for (jsoff_t i = len; i > 0; i--) {
    jsval_t val = arr_get(js, arr, i - 1);
    
    jsval_t call_args[3] = { val, tov((double)(i - 1)), arr };
    jsval_t result = sv_vm_call(js->vm, js, callback, this_arg, call_args, 3, NULL, false);
    
    if (is_err(result)) return result;
    if (js_truthy(js, result)) return return_index ? tov((double)(i - 1)) : val;
  }
  
  return return_index ? tov(-1) : js_mkundef();
}

static jsval_t builtin_array_findLast(ant_t *js, jsval_t *args, int nargs) {
  return array_find_last_impl(js, args, nargs, false, "findLast");
}

static jsval_t builtin_array_findLastIndex(ant_t *js, jsval_t *args, int nargs) {
  return array_find_last_impl(js, args, nargs, true, "findLastIndex");
}

static jsval_t builtin_array_flatMap(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "flatMap called on non-array");
  }
  if (nargs == 0 || vtype(args[0]) != T_FUNC) {
    return js_mkerr(js, "flatMap requires a function argument");
  }
  
  jsval_t callback = args[0];
  jsval_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  jsoff_t len = get_array_length(js, arr);
  
  jsval_t result = mkarr(js);
  if (is_err(result)) return result;
  jsoff_t result_idx = 0;
  
  for (jsoff_t i = 0; i < len; i++) {
    jsval_t elem = arr_get(js, arr, i);
    jsval_t call_args[3] = { elem, tov((double)i), arr };
    jsval_t mapped = sv_vm_call(js->vm, js, callback, this_arg, call_args, 3, NULL, false);
    if (is_err(mapped)) return mapped;
    
    if (vtype(mapped) == T_ARR || vtype(mapped) == T_OBJ) {
      jsoff_t m_len = get_array_length(js, mapped);
      for (jsoff_t j = 0; j < m_len; j++) {
        jsval_t m_elem = arr_get(js, mapped, j);
        arr_set(js, result, result_idx, m_elem);
        result_idx++;
      }
    } else {
      arr_set(js, result, result_idx, mapped);
      result_idx++;
    }
  }
  
  return mkval(T_ARR, vdata(result));
}

static const char *js_tostring(ant_t *js, jsval_t v) {
  if (vtype(v) == T_STR) {
    jsoff_t slen, off = vstr(js, v, &slen);
    return (const char *)&js->mem[off];
  }
  return js_str(js, v);
}

static int js_compare_values(ant_t *js, jsval_t a, jsval_t b, jsval_t compareFn) {
  uint8_t t = vtype(compareFn);
  if (t == T_FUNC || t == T_CFUNC) {
    jsval_t call_args[2] = { a, b };
    jsval_t result = sv_vm_call(js->vm, js, compareFn, js_mkundef(), call_args, 2, NULL, false);
    if (vtype(result) == T_NUM) return (int)tod(result);
    return 0;
  }
  
  if (vtype(a) == T_STR && vtype(b) == T_STR) {
    jsoff_t len_a, len_b;
    const char *sa = (const char *)&js->mem[vstr(js, a, &len_a)];
    const char *sb = (const char *)&js->mem[vstr(js, b, &len_b)];
    return strcmp(sa, sb);
  }
  
  const char *sa = js_tostring(js, a);
  size_t len = strlen(sa);
  
  char *copy = alloca(len + 1);
  memcpy(copy, sa, len + 1);
  
  return strcmp(copy, js_tostring(js, b));
}

static jsval_t builtin_array_indexOf(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "indexOf called on non-array");
  }
  if (nargs == 0) return tov(-1);
  
  jsval_t search = args[0];
  jsoff_t len = get_array_length(js, arr);
  
  jsoff_t start = 0;
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    int s = (int) tod(args[1]);
    if (s < 0) s = (int)len + s;
    if (s < 0) s = 0;
    start = (jsoff_t) s;
  }
  
  for (jsoff_t i = start; i < len; i++) {
    jsval_t elem = arr_get(js, arr, i);
    if (vtype(elem) == T_UNDEF && !arr_has(js, arr, i)) continue;
    if (strict_eq_values(js, elem, search)) return tov((double)i);
  }
  return tov(-1);
}

static jsval_t builtin_array_lastIndexOf(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "lastIndexOf called on non-array");
  }
  if (nargs == 0) return tov(-1);
  
  jsval_t search = args[0];
  jsoff_t len = get_array_length(js, arr);
  
  int start = (int)len - 1;
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    start = (int) tod(args[1]);
    if (start < 0) start = (int)len + start;
  }
  if (start >= (int)len) start = (int)len - 1;
  
  for (int i = start; i >= 0; i--) {
    jsval_t elem = arr_get(js, arr, (jsoff_t)i);
    if (vtype(elem) == T_UNDEF && !arr_has(js, arr, (jsoff_t)i)) continue;
    if (strict_eq_values(js, elem, search)) return tov((double)i);
  }
  return tov(-1);
}

static jsval_t builtin_array_reduceRight(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "reduceRight called on non-array");
  }
  if (nargs == 0 || vtype(args[0]) != T_FUNC) {
    return js_mkerr(js, "reduceRight requires a function argument");
  }
  
  jsval_t callback = args[0];
  jsoff_t len = get_array_length(js, arr);
  
  int start_idx = (int)len - 1;
  jsval_t accumulator;
  
  if (nargs >= 2) {
    accumulator = args[1];
  } else {
    if (len == 0) return js_mkerr(js, "reduceRight of empty array with no initial value");
    accumulator = arr_get(js, arr, len - 1);
    start_idx = (int)len - 2;
  }
  
  for (int i = start_idx; i >= 0; i--) {
    jsval_t elem = arr_get(js, arr, (jsoff_t)i);
    jsval_t call_args[4] = { accumulator, elem, tov((double)i), arr };
    accumulator = sv_vm_call(js->vm, js, callback, js_mkundef(), call_args, 4, NULL, false);
    if (is_err(accumulator)) return accumulator;
  }
  
  return accumulator;
}

static jsval_t builtin_array_shift(ant_t *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "shift called on non-array");
  }

  jsoff_t len = proxy_aware_length(js, arr);
  if (len == 0) return js_mkundef();

  jsoff_t doff = get_dense_buf(js, arr);
  if (doff && !is_proxy(js, arr)) {
    jsoff_t d_len = dense_length(js, doff);
    if (d_len == 0) return js_mkundef();
    jsval_t first = dense_get(js, doff, 0);
    if (is_empty_slot(first)) first = js_mkundef();
    memmove(&js->mem[doff + sizeof(jsoff_t) * 2],
            &js->mem[doff + sizeof(jsoff_t) * 2 + sizeof(jsval_t)],
            sizeof(jsval_t) * (d_len - 1));
    dense_set(js, doff, d_len - 1, T_EMPTY);
    dense_set_length(js, doff, d_len - 1);
    return first;
  }

  jsval_t read_from = is_proxy(js, arr) ? proxy_read_target(js, arr) : arr;
  jsval_t first = arr_get(js, read_from, 0);

  for (jsoff_t i = 1; i < len; i++) {
    if (arr_has(js, read_from, i)) {
      jsval_t elem = arr_get(js, read_from, i);
      char dst[16];
      size_t dstlen = uint_to_str(dst, sizeof(dst), (unsigned)(i - 1));
      js_setprop(js, arr, js_mkstr(js, dst, dstlen), elem);
    }
  }

  js_setprop(js, arr, js->length_str, tov((double)(len - 1)));
  js->needs_gc = true;
  
  return first;
}

static jsval_t builtin_array_unshift(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "unshift called on non-array");
  }

  jsoff_t len = proxy_aware_length(js, arr);

  jsoff_t doff = get_dense_buf(js, arr);
  if (doff && !is_proxy(js, arr)) {
    jsoff_t d_len = dense_length(js, doff);
    jsoff_t new_len = d_len + nargs;
    jsoff_t cap = dense_capacity(js, doff);
    if (new_len > cap) {
      doff = dense_grow(js, arr, new_len);
      if (doff == 0) return js_mkerr(js, "oom");
    }
    memmove(&js->mem[doff + sizeof(jsoff_t) * 2 + sizeof(jsval_t) * nargs],
            &js->mem[doff + sizeof(jsoff_t) * 2],
            sizeof(jsval_t) * d_len);
    for (int i = 0; i < nargs; i++)
      dense_set(js, doff, (jsoff_t)i, args[i]);
    dense_set_length(js, doff, new_len);
    return tov((double) new_len);
  }

  jsval_t read_from = is_proxy(js, arr) ? proxy_read_target(js, arr) : arr;

  for (int i = (int)len - 1; i >= 0; i--) {
    if (arr_has(js, read_from, (jsoff_t)i)) {
      jsval_t elem = arr_get(js, read_from, (jsoff_t)i);
      char dst[16];
      size_t dstlen = uint_to_str(dst, sizeof(dst), (unsigned)(i + nargs));
      js_setprop(js, arr, js_mkstr(js, dst, dstlen), elem);
    }
  }
  
  for (int i = 0; i < nargs; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
    jsval_t key = js_mkstr(js, idxstr, idxlen);
    js_setprop(js, arr, key, args[i]);
  }
  
  jsoff_t new_len = len + nargs;
  js_setprop(js, arr, js->length_str, tov((double) new_len));
  
  return tov((double) new_len);
}

static jsval_t builtin_array_some(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "some called on non-array");
  
  jsval_t callback = require_callback(js, args, nargs, "some");
  if (is_err(callback)) return callback;
  jsval_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  
  jsoff_t len = get_array_length(js, arr);
  if (len == 0) return mkval(T_BOOL, 0);
  
  for (jsoff_t i = 0; i < len; i++) {
    if (!arr_has(js, arr, i)) continue;
    jsval_t val = arr_get(js, arr, i);
    
    jsval_t call_args[3] = { val, tov((double)i), arr };
    jsval_t result = sv_vm_call(js->vm, js, callback, this_arg, call_args, 3, NULL, false);
    
    if (is_err(result)) return result;
    if (js_truthy(js, result)) return mkval(T_BOOL, 1);
  }
  
  return mkval(T_BOOL, 0);
}

static jsval_t builtin_array_sort(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  jsval_t compareFn = js_mkundef();
  jsval_t *vals = NULL, *keys = NULL, *temp_vals = NULL, *temp_keys = NULL;
  jsoff_t *offs = NULL;
  jsoff_t count = 0, undef_count = 0, len = 0;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "sort called on non-array");
  
  if (nargs >= 1) {
    uint8_t t = vtype(args[0]);
    if (t == T_FUNC || t == T_CFUNC) compareFn = args[0];
    else if (t != T_UNDEF) return js_mkerr_typed(js, JS_ERR_TYPE, "compareFn must be a function or undefined");
  }
  
  len = get_array_length(js, arr);
  if (len == 0) return arr;
  
  jsoff_t doff = get_dense_buf(js, arr);
  if (doff) {
    vals = malloc(len * sizeof(jsval_t));
    if (!vals) goto oom;
    for (jsoff_t i = 0; i < len; i++) {
      jsval_t v = dense_get(js, doff, i);
      if (is_empty_slot(v) || vtype(v) == T_UNDEF) undef_count++;
      else vals[count++] = v;
    }
  } else {
    vals = malloc(len * sizeof(jsval_t));
    offs = malloc(len * sizeof(jsoff_t));
    if (!vals || !offs) goto oom;
    
    ant_iter_t iter = js_prop_iter_begin(js, arr);
    const char *key;
    size_t key_len;
    jsval_t val;
    
    while (js_prop_iter_next(&iter, &key, &key_len, &val)) {
      if (key_len == 0 || key[0] > '9' || key[0] < '0') continue;
      
      unsigned idx = 0;
      bool valid = true;
      for (size_t i = 0; i < key_len && valid; i++) {
        if (key[i] < '0' || key[i] > '9') valid = false;
        else idx = idx * 10 + (key[i] - '0');
      }
      if (!valid || idx >= len || (count + undef_count) >= len) continue;
      
      offs[count + undef_count] = iter.off;
      if (vtype(val) == T_UNDEF) undef_count++;
      else vals[count++] = val;
    }
    
    js_prop_iter_end(&iter);
  }
  if (count <= 1) goto writeback;
  
  bool use_keys = (vtype(compareFn) == T_UNDEF);
  if (use_keys) {
    keys = malloc(count * sizeof(jsval_t));
    if (!keys) goto oom;
    for (jsoff_t i = 0; i < count; i++) {
      const char *s = js_tostring(js, vals[i]);
      keys[i] = js_mkstr(js, s, strlen(s));
    }
  }
  
  temp_vals = malloc(count * sizeof(jsval_t));
  if (use_keys) temp_keys = malloc(count * sizeof(jsval_t));
  if (!temp_vals || (use_keys && !temp_keys)) goto oom;
  
  for (jsoff_t width = 1; width < count; width *= 2) {
    for (jsoff_t left = 0; left < count; left += width * 2) {
      jsoff_t mid = left + width;
      jsoff_t right = (mid + width < count) ? mid + width : count;
      if (mid >= count) break;
      
      jsoff_t i = left, j = mid, k = 0;
      while (i < mid && j < right) {
        int cmp;
        if (use_keys) {
          jsoff_t len_a, len_b;
          const char *sa = (const char *)&js->mem[vstr(js, keys[i], &len_a)];
          const char *sb = (const char *)&js->mem[vstr(js, keys[j], &len_b)];
          cmp = strcmp(sa, sb);
        } else {
          cmp = js_compare_values(js, vals[i], vals[j], compareFn);
        }
        if (cmp <= 0) {
          temp_vals[k] = vals[i];
          if (use_keys) temp_keys[k] = keys[i];
          k++; i++;
        } else {
          temp_vals[k] = vals[j];
          if (use_keys) temp_keys[k] = keys[j];
          k++; j++;
        }
      }
      while (i < mid) {
        temp_vals[k] = vals[i];
        if (use_keys) temp_keys[k] = keys[i];
        k++; i++;
      }
      while (j < right) {
        temp_vals[k] = vals[j];
        if (use_keys) temp_keys[k] = keys[j];
        k++; j++;
      }
      
      memcpy(&vals[left], temp_vals, k * sizeof(jsval_t));
      if (use_keys) memcpy(&keys[left], temp_keys, k * sizeof(jsval_t));
    }
  }
  
writeback:
  if (doff) {
    for (jsoff_t i = 0; i < count; i++) dense_set(js, doff, i, vals[i]);
    for (jsoff_t i = count; i < count + undef_count; i++) dense_set(js, doff, i, js_mkundef());
    for (jsoff_t i = count + undef_count; i < len; i++) dense_set(js, doff, i, T_EMPTY);
  } else {
    for (jsoff_t i = 0; i < count; i++)
      saveval(js, offs[i] + sizeof(jsoff_t) * 2, vals[i]);
    for (jsoff_t i = 0; i < undef_count; i++)
      saveval(js, offs[count + i] + sizeof(jsoff_t) * 2, js_mkundef());
  }
  
  free(temp_keys);
  free(temp_vals);
  free(keys);
  free(vals);
  free(offs);
  return arr;
  
oom:
  free(temp_keys);
  free(temp_vals);
  free(keys);
  free(vals);
  free(offs);
  return js_mkerr(js, "out of memory");
}

static jsval_t builtin_array_splice(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "splice called on non-array");
  }

  jsoff_t len = proxy_aware_length(js, arr);
  jsval_t read_from = is_proxy(js, arr) ? proxy_read_target(js, arr) : arr;

  int start = 0;
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    start = (int) tod(args[0]);
    if (start < 0) start = (int)len + start;
    if (start < 0) start = 0;
    if (start > (int)len) start = (int)len;
  }

  int deleteCount = (int)len - start;
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    deleteCount = (int) tod(args[1]);
    if (deleteCount < 0) deleteCount = 0;
    if (deleteCount > (int)len - start) deleteCount = (int)len - start;
  }

  int insertCount = nargs > 2 ? nargs - 2 : 0;

  jsval_t removed = array_alloc_like(js, arr);
  if (is_err(removed)) return removed;

  jsoff_t doff = get_dense_buf(js, arr);
  if (doff && !is_proxy(js, arr)) {
    for (int i = 0; i < deleteCount; i++) {
      jsval_t elem = arr_get(js, arr, (jsoff_t)(start + i));
      arr_set(js, removed, (jsoff_t)i, elem);
    }

    jsoff_t d_len = dense_length(js, doff);
    int shift = insertCount - deleteCount;
    jsoff_t new_len = (jsoff_t)((int)d_len + shift);

    if (shift != 0) {
      if (new_len > dense_capacity(js, doff)) {
        doff = dense_grow(js, arr, new_len);
        if (doff == 0) return js_mkerr(js, "oom");
      }
      jsoff_t move_start = (jsoff_t)(start + deleteCount);
      jsoff_t move_dest = (jsoff_t)(start + insertCount);
      jsoff_t move_count = d_len - move_start;
      if (move_count > 0) memmove(
        &js->mem[doff + sizeof(jsoff_t) * 2 + sizeof(jsval_t) * move_dest],
        &js->mem[doff + sizeof(jsoff_t) * 2 + sizeof(jsval_t) * move_start],
        sizeof(jsval_t) * move_count
      );
    }

    for (int i = 0; i < insertCount; i++)
      dense_set(js, doff, (jsoff_t)(start + i), args[2 + i]);

    if (shift < 0) {
      for (jsoff_t i = new_len; i < d_len; i++)
        dense_set(js, doff, i, T_EMPTY);
    }

    dense_set_length(js, doff, new_len);
    if (deleteCount > 0) js->needs_gc = true;
    return removed;
  }

  for (int i = 0; i < deleteCount; i++) {
    char src[16], dst[16];
    snprintf(src, sizeof(src), "%u", (unsigned)(start + i));
    snprintf(dst, sizeof(dst), "%u", (unsigned) i);
    jsoff_t elem_off = lkp(js, read_from, src, strlen(src));
    if (elem_off != 0) {
      jsval_t elem = resolveprop(js, mkval(T_PROP, elem_off));
      jsval_t key = js_mkstr(js, dst, strlen(dst));
      js_setprop(js, removed, key, elem);
    }
  }

  js_setprop(js, removed, js->length_str, tov((double) deleteCount));
  int shift = insertCount - deleteCount;
  
  if (shift > 0) {
    for (int i = (int)len - 1; i >= start + deleteCount; i--) {
      char src[16], dst[16];
      snprintf(src, sizeof(src), "%u", (unsigned) i);
      snprintf(dst, sizeof(dst), "%u", (unsigned)(i + shift));
      jsoff_t elem_off = lkp(js, read_from, src, strlen(src));
      jsval_t elem = elem_off ? resolveprop(js, mkval(T_PROP, elem_off)) : js_mkundef();
      jsval_t key = js_mkstr(js, dst, strlen(dst));
      js_setprop(js, arr, key, elem);
    }
  } else if (shift < 0) {
    for (int i = start + deleteCount; i < (int)len; i++) {
      char src[16], dst[16];
      snprintf(src, sizeof(src), "%u", (unsigned) i);
      snprintf(dst, sizeof(dst), "%u", (unsigned)(i + shift));
      jsoff_t elem_off = lkp(js, read_from, src, strlen(src));
      jsval_t elem = elem_off ? resolveprop(js, mkval(T_PROP, elem_off)) : js_mkundef();
      jsval_t key = js_mkstr(js, dst, strlen(dst));
      js_setprop(js, arr, key, elem);
    }
  }
  
  for (int i = 0; i < insertCount; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)(start + i));
    jsval_t key = js_mkstr(js, idxstr, idxlen);
    js_setprop(js, arr, key, args[2 + i]);
  }
  
  js_setprop(js, arr, js->length_str, tov((double)((int)len + shift)));
  if (deleteCount > 0) js->needs_gc = true;
  
  return removed;
}

static jsval_t builtin_array_copyWithin(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "copyWithin called on non-array");
  }

  jsoff_t len = proxy_aware_length(js, arr);
  jsval_t read_from = is_proxy(js, arr) ? proxy_read_target(js, arr) : arr;

  int target = 0, start = 0, end = (int)len;
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    target = (int) tod(args[0]);
    if (target < 0) target = (int)len + target;
    if (target < 0) target = 0;
  }
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    start = (int) tod(args[1]);
    if (start < 0) start = (int)len + start;
    if (start < 0) start = 0;
  }
  if (nargs >= 3 && vtype(args[2]) == T_NUM) {
    end = (int) tod(args[2]);
    if (end < 0) end = (int)len + end;
    if (end < 0) end = 0;
  }
  
  if (end > (int)len) end = (int)len;
  int count = end - start;
  if (count > (int)len - target) count = (int)len - target;
  if (count <= 0) return arr;

  jsoff_t doff = get_dense_buf(js, arr);
  if (doff && !is_proxy(js, arr)) {
    if (start < target) {
      for (int i = count - 1; i >= 0; i--) {
        jsval_t v = dense_get(js, doff, (jsoff_t)(start + i));
        dense_set(js, doff, (jsoff_t)(target + i), is_empty_slot(v) ? js_mkundef() : v);
      }
    } else {
      for (int i = 0; i < count; i++) {
        jsval_t v = dense_get(js, doff, (jsoff_t)(start + i));
        dense_set(js, doff, (jsoff_t)(target + i), is_empty_slot(v) ? js_mkundef() : v);
      }
    }
    return arr;
  }

  jsval_t *temp = (jsval_t *)malloc(count * sizeof(jsval_t));
  for (int i = 0; i < count; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)(start + i));
    jsoff_t elem_off = lkp(js, read_from, idxstr, idxlen);
    temp[i] = elem_off ? resolveprop(js, mkval(T_PROP, elem_off)) : js_mkundef();
  }
  
  for (int i = 0; i < count; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)(target + i));
    jsval_t key = js_mkstr(js, idxstr, idxlen);
    js_setprop(js, arr, key, temp[i]);
  }
  
  free(temp);
  return arr;
}

static jsval_t builtin_array_toSorted(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "toSorted called on non-array");
  
  jsval_t result = array_shallow_copy(js, arr, get_array_length(js, arr));
  if (is_err(result)) return result;
  
  jsval_t saved_this = js->this_val;
  js->this_val = result;
  jsval_t sorted = builtin_array_sort(js, args, nargs);
  js->this_val = saved_this;
  
  if (is_err(sorted)) return sorted;
  return mkval(T_ARR, vdata(result));
}

static jsval_t builtin_array_toReversed(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "toReversed called on non-array");
  
  jsval_t result = array_shallow_copy(js, arr, get_array_length(js, arr));
  if (is_err(result)) return result;
  
  jsval_t saved_this = js->this_val;
  js->this_val = result;
  jsval_t reversed = builtin_array_reverse(js, NULL, 0);
  js->this_val = saved_this;
  
  if (is_err(reversed)) return reversed;
  return mkval(T_ARR, vdata(result));
}

static jsval_t builtin_array_toSpliced(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "toSpliced called on non-array");
  
  jsval_t result = array_shallow_copy(js, arr, get_array_length(js, arr));
  if (is_err(result)) return result;
  
  jsval_t saved_this = js->this_val;
  js->this_val = result;
  builtin_array_splice(js, args, nargs);
  js->this_val = saved_this;
  
  return mkval(T_ARR, vdata(result));
}

static jsval_t builtin_array_with(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "with called on non-array");
  }
  
  if (nargs < 2) return js_mkerr(js, "with requires index and value arguments");
  
  jsoff_t len = get_array_length(js, arr);
  
  int idx = (int) tod(args[0]);
  if (idx < 0) idx = (int)len + idx;
  if (idx < 0 || (jsoff_t)idx >= len) return js_mkerr(js, "Invalid index");
  
  jsval_t result = mkarr(js);
  if (is_err(result)) return result;
  
  for (jsoff_t i = 0; i < len; i++) {
    jsval_t elem = ((jsoff_t)idx == i) ? args[1] : arr_get(js, arr, i);
    arr_set(js, result, i, elem);
  }
  
  return mkval(T_ARR, vdata(result));
}

static jsval_t builtin_array_keys(ant_t *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "keys called on non-array");
  }
  
  jsoff_t len = get_array_length(js, arr);
  
  jsval_t result = mkarr(js);
  if (is_err(result)) return result;
  
  for (jsoff_t i = 0; i < len; i++) {
    arr_set(js, result, i, tov((double) i));
  }
  
  return mkval(T_ARR, vdata(result));
}

static jsval_t builtin_array_values(ant_t *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "values called on non-array");
  }
  
  jsoff_t len = get_array_length(js, arr);
  
  jsval_t result = mkarr(js);
  if (is_err(result)) return result;
  
  for (jsoff_t i = 0; i < len; i++) {
    arr_set(js, result, i, arr_get(js, arr, i));
  }
  
  return mkval(T_ARR, vdata(result));
}

static jsval_t builtin_array_entries(ant_t *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "entries called on non-array");
  }
  
  jsoff_t len = get_array_length(js, arr);
  
  jsval_t result = mkarr(js);
  if (is_err(result)) return result;
  
  for (jsoff_t i = 0; i < len; i++) {
    jsval_t entry = mkarr(js);
    if (is_err(entry)) return entry;
    
    jsval_t elem = arr_get(js, arr, i);
    
    arr_set(js, entry, 0, tov((double) i));
    arr_set(js, entry, 1, elem);
    
    arr_set(js, result, i, mkval(T_ARR, vdata(entry)));
  }
  
  return mkval(T_ARR, vdata(result));
}

static jsval_t builtin_array_toString(ant_t *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  
  jsval_t join_result;
  if (js_try_call_method(js, arr, "join", 4, NULL, 0, &join_result)) {
    if (is_err(join_result)) return join_result;
    return join_result;
  }
  
  return builtin_object_toString(js, args, nargs);
}

static jsval_t builtin_array_toLocaleString(ant_t *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR) return js_mkerr(js, "toLocaleString called on non-array");
  
  jsoff_t len = get_array_length(js, arr);
  if (len == 0) return js_mkstr(js, "", 0);
  
  char *result = NULL;
  size_t result_len = 0, result_cap = 256;
  result = (char *)ant_calloc(result_cap);
  if (!result) return js_mkerr(js, "oom");
  
  for (jsoff_t i = 0; i < len; i++) {
    if (i > 0) {
      if (result_len + 1 >= result_cap) {
        result_cap *= 2;
        char *new_result = (char *)ant_calloc(result_cap);
        if (!new_result) { free(result); return js_mkerr(js, "oom"); }
        memcpy(new_result, result, result_len);
        free(result);
        result = new_result;
      }
      result[result_len++] = ',';
    }
    
    if (!arr_has(js, arr, i)) continue;
    jsval_t elem = arr_get(js, arr, i);
    if (vtype(elem) == T_NULL || vtype(elem) == T_UNDEF) continue;
    
    char buf[64];
    size_t elem_len = tostr(js, elem, buf, sizeof(buf));
    
    if (result_len + elem_len >= result_cap) {
      while (result_len + elem_len >= result_cap) result_cap *= 2;
      char *new_result = (char *)ant_calloc(result_cap);
      if (!new_result) { free(result); return js_mkerr(js, "oom"); }
      memcpy(new_result, result, result_len);
      free(result);
      result = new_result;
    }
    memcpy(result + result_len, buf, elem_len);
    result_len += elem_len;
  }
  
  jsval_t ret = js_mkstr(js, result, result_len);
  free(result);
  return ret;
}

static jsval_t builtin_Array_isArray(ant_t *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs == 0) return mkval(T_BOOL, 0);
  return mkval(T_BOOL, vtype(args[0]) == T_ARR ? 1 : 0);
}

typedef struct {
  jsval_t write_target;
  jsval_t result;
  jsval_t mapFn;
  jsval_t mapThis;
  jsoff_t index;
} array_from_iter_ctx_t;

static iter_action_t array_from_iter_cb(ant_t *js, jsval_t value, void *ctx, jsval_t *out) {
  array_from_iter_ctx_t *fctx = (array_from_iter_ctx_t *)ctx;
  jsval_t elem = value;

  if (is_callable(fctx->mapFn)) {
    jsval_t call_args[2] = { elem, tov((double)fctx->index) };
    elem = sv_vm_call(js->vm, js, fctx->mapFn, fctx->mapThis, call_args, 2, NULL, false);
    if (is_err(elem)) { *out = elem; return ITER_ERROR; }
  }

  if (vtype(fctx->write_target) == T_ARR) arr_set(js, fctx->write_target, fctx->index, elem);
  else {
    char idxstr[16]; size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)fctx->index);
    js_setprop(js, fctx->write_target, js_mkstr(js, idxstr, idxlen), elem);
  }

  fctx->index++;
  return ITER_CONTINUE;
}

static jsval_t builtin_Array_from(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkarr(js);

  jsval_t src = args[0];
  jsval_t mapFn = (nargs >= 2 && is_callable(args[1])) ? args[1] : js_mkundef();
  jsval_t mapThis = (nargs >= 3) ? args[2] : js_mkundef();

  jsval_t ctor = js->this_val;
  bool use_ctor = (vtype(ctor) == T_FUNC || vtype(ctor) == T_CFUNC);
  jsval_t result = use_ctor ? array_alloc_from_ctor_with_length(js, ctor, 0) : mkarr(js);
  if (is_err(result)) return result;

  bool result_is_proxy = is_proxy(js, result);
  jsval_t write_target = result_is_proxy ? proxy_read_target(js, result) : result;
  jsval_t iter_sym = get_iterator_sym();

  if (vtype(src) == T_STR) {
    if (is_rope(js, src)) { src = rope_flatten(js, src); if (is_err(src)) return src; }
    jsoff_t slen = str_len_fast(js, src);
    array_from_iter_ctx_t ctx = { write_target, result, mapFn, mapThis, 0 };
    for (jsoff_t i = 0; i < slen; ) {
      jsoff_t off = vstr(js, src, NULL);
      utf8proc_int32_t cp;
      jsoff_t cb_len = (jsoff_t)utf8_next((const utf8proc_uint8_t *)&js->mem[off + i], (utf8proc_ssize_t)(slen - i), &cp);
      jsval_t ch = js_mkstr(js, (char *)&js->mem[off + i], cb_len);
      
      jsval_t out;
      iter_action_t act = array_from_iter_cb(js, ch, &ctx, &out);
      
      if (act == ITER_ERROR) return out;
      i += cb_len;
    }
    if (vtype(result) != T_ARR) js_setprop(js, result, js->length_str, tov((double)ctx.index));
  } else if (vtype(src) == T_ARR) {
    jsoff_t iter_off = (vtype(iter_sym) == T_SYMBOL) ? lkp_sym_proto(js, src, (jsoff_t)vdata(iter_sym)) : 0;
    bool default_iter = iter_off != 0 && vtype(loadval(js, iter_off + sizeof(jsoff_t) * 2)) == T_CFUNC;

    if (default_iter) {
      array_from_iter_ctx_t ctx = { write_target, result, mapFn, mapThis, 0 };
      jsoff_t len = get_array_length(js, src);
      for (jsoff_t i = 0; i < len; i++) {
        jsval_t unused;
        iter_action_t act = array_from_iter_cb(js, arr_get(js, src, i), &ctx, &unused);
        if (act == ITER_ERROR) return unused;
      }
      if (vtype(result) != T_ARR) js_setprop(js, result, js->length_str, tov((double)len));
    } else {
      array_from_iter_ctx_t ctx = { write_target, result, mapFn, mapThis, 0 };
      jsval_t iter_result = iter_foreach(js, src, array_from_iter_cb, &ctx);
      if (is_err(iter_result)) return iter_result;
      if (vtype(result) != T_ARR) js_setprop(js, result, js->length_str, tov((double)ctx.index));
    }
  } else {
    jsoff_t iter_prop = (vtype(iter_sym) == T_SYMBOL) ? lkp_sym_proto(js, src, (jsoff_t)vdata(iter_sym)) : 0;

    if (iter_prop != 0) {
      array_from_iter_ctx_t ctx = { write_target, result, mapFn, mapThis, 0 };
      jsval_t iter_result = iter_foreach(js, src, array_from_iter_cb, &ctx);
      if (is_err(iter_result)) return iter_result;
      if (vtype(result) != T_ARR) js_setprop(js, result, js->length_str, tov((double)ctx.index));
    } else if (vtype(src) == T_OBJ) {
      array_from_iter_ctx_t ctx = { write_target, result, mapFn, mapThis, 0 };
      jsoff_t len = get_array_length(js, src);
      for (jsoff_t i = 0; i < len; i++) {
        jsval_t unused;
        iter_action_t act = array_from_iter_cb(js, arr_get(js, src, i), &ctx, &unused);
        if (act == ITER_ERROR) return unused;
      }
      if (vtype(result) != T_ARR) js_setprop(js, result, js->length_str, tov((double)len));
    }
  }

  if (!use_ctor) return mkval(T_ARR, vdata(result));
  return result;
}

static jsval_t builtin_Array_of(ant_t *js, jsval_t *args, int nargs) {
  jsval_t ctor = js->this_val;
  bool use_ctor = (vtype(ctor) == T_FUNC || vtype(ctor) == T_CFUNC);
  jsval_t arr = use_ctor ? array_alloc_from_ctor_with_length(js, ctor, (jsoff_t)nargs) : mkarr(js);
  if (is_err(arr)) return arr;

  bool arr_is_proxy = is_proxy(js, arr);
  jsval_t write_target = arr_is_proxy ? proxy_read_target(js, arr) : arr;

  for (int i = 0; i < nargs; i++) {
    if (vtype(write_target) == T_ARR) arr_set(js, write_target, (jsoff_t)i, args[i]);
    else {
      char idxstr[16]; size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
      js_setprop(js, write_target, js_mkstr(js, idxstr, idxlen), args[i]);
    }
  }

  if (vtype(arr) != T_ARR) js_setprop(js, arr, js->length_str, tov((double) nargs));
  if (!use_ctor) return mkval(T_ARR, vdata(arr));

  return arr;
}

static jsval_t builtin_string_indexOf(ant_t *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "indexOf called on non-string");
  if (nargs == 0) return tov(-1);

  jsval_t search = args[0];
  if (vtype(search) != T_STR) return tov(-1);

  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  jsoff_t search_len, search_off = vstr(js, search, &search_len);
  
  jsoff_t start = 0;
  double dstr_len = D(str_len);
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    double pos = tod(args[1]);
    if (pos < 0) pos = 0;
    if (pos > dstr_len) pos = dstr_len;
    start = (jsoff_t) pos;
  }
  
  if (search_len == 0) return tov(D(start));
  if (start + search_len > str_len) return tov(-1);

  const char *str_ptr = (char *) &js->mem[str_off];
  const char *search_ptr = (char *) &js->mem[search_off];

  for (jsoff_t i = start; i <= str_len - search_len; i++) {
    if (memcmp(str_ptr + i, search_ptr, search_len) == 0) return tov(D(i));
  }
  return tov(-1);
}

static jsval_t builtin_string_substring(ant_t *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "substring called on non-string");
  jsoff_t byte_len, str_off = vstr(js, str, &byte_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  size_t utf16_len = utf16_strlen(str_ptr, byte_len);
  jsoff_t start = 0, end = (jsoff_t)utf16_len;
  double dstr_len2 = D(utf16_len);
  
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    double d = tod(args[0]);
    start = (jsoff_t) (d < 0 ? 0 : (d > dstr_len2 ? dstr_len2 : d));
  }
  
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    double d = tod(args[1]);
    end = (jsoff_t) (d < 0 ? 0 : (d > dstr_len2 ? dstr_len2 : d));
  }
  
  if (start > end) {
    jsoff_t tmp = start;
    start = end;
    end = tmp;
  }
  
  size_t byte_start, byte_end;
  utf16_range_to_byte_range(str_ptr, byte_len, start, end, &byte_start, &byte_end);
  return js_mkstr(js, str_ptr + byte_start, byte_end - byte_start);
}

static jsval_t builtin_string_substr(ant_t *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "substr called on non-string");
  jsoff_t byte_len, str_off = vstr(js, str, &byte_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  size_t utf16_len = utf16_strlen(str_ptr, byte_len);
  
  if (nargs < 1) return js_mkstr(js, str_ptr, byte_len);
  
  double d_start = tod(args[0]);
  jsoff_t start;
  if (d_start < 0) {
    start = (jsoff_t)((double)utf16_len + d_start);
    if ((int)start < 0) start = 0;
  } else {
    start = (jsoff_t)d_start;
  }
  if (start > (jsoff_t)utf16_len) start = (jsoff_t)utf16_len;
  
  jsoff_t len = (jsoff_t)utf16_len - start;
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    double d = tod(args[1]);
    if (d < 0) d = 0;
    len = (jsoff_t)d;
  }
  if (start + len > (jsoff_t)utf16_len) len = (jsoff_t)utf16_len - start;
  
  size_t byte_start, byte_end;
  utf16_range_to_byte_range(str_ptr, byte_len, start, start + len, &byte_start, &byte_end);
  return js_mkstr(js, str_ptr + byte_start, byte_end - byte_start);
}

static jsval_t string_split_impl(ant_t *js, jsval_t str, jsval_t *args, int nargs) {
  if (vtype(str) != T_STR) return js_mkerr(js, "split called on non-string");
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  jsval_t arr = mkarr(js);

  if (is_err(arr)) return arr;

  uint32_t limit = UINT32_MAX;
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    double d = tod(args[1]);
    if (d >= 0 && d <= UINT32_MAX) {
      limit = (uint32_t)d;
    }
  }
  if (limit == 0) {
    return mkval(T_ARR, vdata(arr));
  }
  if (nargs == 0) goto return_whole;

  jsval_t sep_arg = args[0];
  if (vtype(sep_arg) == T_OBJ) {
    jsoff_t source_off = lkp(js, sep_arg, "source", 6);
    if (source_off == 0) goto return_whole;
    jsval_t source_val = resolveprop(js, mkval(T_PROP, source_off));
    if (vtype(source_val) != T_STR) goto return_whole;

    jsoff_t plen, poff = vstr(js, source_val, &plen);
    const char *pattern_ptr = (char *) &js->mem[poff];

    if (plen == 0 || (plen == 4 && memcmp(pattern_ptr, "(?:)", 4) == 0)) {
      jsoff_t idx = 0;
      for (jsoff_t i = 0; i < str_len && idx < limit; i++) {
        jsval_t part = js_mkstr(js, str_ptr + i, 1);
        arr_set(js, arr, idx, part);
        idx++;
      }
      return mkval(T_ARR, vdata(arr));
    }

    char pcre2_pattern[512];
    size_t pcre2_len = js_to_pcre2_pattern(pattern_ptr, plen, pcre2_pattern, sizeof(pcre2_pattern));

    uint32_t options = PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_UNSET_BACKREF;
    int errcode;
    PCRE2_SIZE erroffset;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pcre2_pattern, pcre2_len, options, &errcode, &erroffset, NULL);
    if (re == NULL) goto return_whole;

    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(re, NULL);
    uint32_t capture_count;
    pcre2_pattern_info(re, PCRE2_INFO_CAPTURECOUNT, &capture_count);

    if (str_len == 0) {
      int rc = pcre2_match(re, (PCRE2_SPTR)str_ptr, 0, 0, 0, match_data, NULL);
      if (rc >= 0) {
        pcre2_match_data_free(match_data);
        pcre2_code_free(re);
        return mkval(T_ARR, vdata(arr));
      }
    }

    jsoff_t idx = 0;
    PCRE2_SIZE search_pos = 0;
    PCRE2_SIZE segment_start = 0;
    PCRE2_SIZE last_match_end = (PCRE2_SIZE)-1;
    bool had_any_split = false;
    
    while (idx < limit && search_pos <= str_len) {
      int rc = pcre2_match(re, (PCRE2_SPTR)str_ptr, str_len, search_pos, 0, match_data, NULL);
      if (rc < 0) break;

      PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
      PCRE2_SIZE match_start = ovector[0];
      PCRE2_SIZE match_end = ovector[1];

      if (match_start == match_end && match_start == last_match_end) {
        search_pos = match_end + 1;
        continue;
      }

      if (match_start == match_end && capture_count > 0) {
        bool is_pure_empty_capture = true;
        for (uint32_t i = 1; i <= capture_count; i++) {
          PCRE2_SIZE cap_start = ovector[2*i];
          PCRE2_SIZE cap_end = ovector[2*i+1];
          if (cap_start == PCRE2_UNSET || cap_end != cap_start) {
            is_pure_empty_capture = false;
            break;
          }
        }
        if (is_pure_empty_capture) {
          search_pos = match_end + 1;
          continue;
        }
      }
      
      had_any_split = true;

      jsval_t part = js_mkstr(js, str_ptr + segment_start, match_start - segment_start);
      arr_set(js, arr, idx, part);
      idx++;

      for (uint32_t i = 1; i <= capture_count && idx < limit; i++) {
        PCRE2_SIZE cap_start = ovector[2*i];
        PCRE2_SIZE cap_end = ovector[2*i+1];
        if (cap_start == PCRE2_UNSET) {
          arr_set(js, arr, idx, js_mkundef());
        } else {
          part = js_mkstr(js, str_ptr + cap_start, cap_end - cap_start);
          arr_set(js, arr, idx, part);
        }
        idx++;
      }

      last_match_end = match_end;
      segment_start = match_end;
      if (match_start == match_end) {
        search_pos = match_end + 1;
      } else {
        search_pos = match_end;
      }
    }

    if (!had_any_split) {
      pcre2_match_data_free(match_data);
      pcre2_code_free(re);
      arr_set(js, arr, 0, js_mkstr(js, str_ptr, str_len));
      return mkval(T_ARR, vdata(arr));
    }

    if (idx < limit) {
      jsval_t part = js_mkstr(js, str_ptr + segment_start, str_len - segment_start);
      arr_set(js, arr, idx, part);
      idx++;
    }

    pcre2_match_data_free(match_data);
    pcre2_code_free(re);
    return mkval(T_ARR, vdata(arr));
  }

  if (vtype(sep_arg) != T_STR) goto return_whole;

  jsoff_t sep_len, sep_off = vstr(js, sep_arg, &sep_len);
  const char *sep_ptr = (char *) &js->mem[sep_off];
  jsoff_t idx = 0, start = 0;

  if (sep_len == 0) {
    for (jsoff_t i = 0; i < str_len && idx < limit; i++) {
      jsval_t part = js_mkstr(js, str_ptr + i, 1);
      arr_set(js, arr, idx, part);
      idx++;
    }
    return mkval(T_ARR, vdata(arr));
  }

  for (jsoff_t i = 0; i + sep_len <= str_len && idx < limit; i++) {
    if (memcmp(str_ptr + i, sep_ptr, sep_len) != 0) continue;
    jsval_t part = js_mkstr(js, str_ptr + start, i - start);
    arr_set(js, arr, idx, part);
    idx++;
    start = i + sep_len;
    i += sep_len - 1;
  }
  if (idx < limit && start <= str_len) {
    jsval_t part = js_mkstr(js, str_ptr + start, str_len - start);
    arr_set(js, arr, idx, part);
    idx++;
  }

  return mkval(T_ARR, vdata(arr));

return_whole:
  if (limit > 0) {
    arr_set(js, arr, 0, str);
  }
  return mkval(T_ARR, vdata(arr));
}

static jsval_t builtin_string_split(ant_t *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "split called on non-string");

  if (nargs > 0 && is_object_type(args[0])) {
    bool called = false;
    jsval_t call_args[2];
    int call_nargs = 1;
    call_args[0] = str;
    if (nargs >= 2) {
      call_args[1] = args[1];
      call_nargs = 2;
    }
    jsval_t dispatched = maybe_call_symbol_method(
      js, args[0], get_split_sym(), args[0], call_args, call_nargs, &called
    );
    if (is_err(dispatched)) return dispatched;
    if (called) return dispatched;
  }

  return string_split_impl(js, str, args, nargs);
}

static jsval_t builtin_string_slice(ant_t *js, jsval_t *args, int nargs) {
  jsval_t this_unwrapped = unwrap_primitive(js, js->this_val);
  jsval_t str = js_tostring_val(js, this_unwrapped);
  if (is_err(str)) return str;
  
  jsoff_t byte_len, str_off = vstr(js, str, &byte_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  size_t utf16_len = utf16_strlen(str_ptr, byte_len);
  jsoff_t start = 0, end = (jsoff_t)utf16_len;
  double dstr_len = D(utf16_len);
  
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    double d = tod(args[0]);
    if (d < 0) {
      start = (jsoff_t) (d + dstr_len < 0 ? 0 : d + dstr_len);
    } else start = (jsoff_t) (d > dstr_len ? dstr_len : d);
  }
  
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    double d = tod(args[1]);
    if (d < 0) {
      end = (jsoff_t) (d + dstr_len < 0 ? 0 : d + dstr_len);
    } else end = (jsoff_t) (d > dstr_len ? dstr_len : d);
  }
  
  if (start > end) start = end;
  size_t byte_start, byte_end;
  utf16_range_to_byte_range(str_ptr, byte_len, start, end, &byte_start, &byte_end);
  return js_mkstr(js, str_ptr + byte_start, byte_end - byte_start);
}

static jsval_t builtin_string_includes(ant_t *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "includes called on non-string");
  if (nargs == 0) return mkval(T_BOOL, 0);
  jsval_t search = args[0];
  
  if (is_object_type(search)) {
    jsval_t maybe_err = reject_regexp_arg(js, search, "includes");
    if (is_err(maybe_err)) return maybe_err;
  }
  search = js_tostring_val(js, search);
  if (is_err(search)) return search;
  
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  jsoff_t search_len, search_off = vstr(js, search, &search_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  const char *search_ptr = (char *) &js->mem[search_off];
  
  jsoff_t start = 0;
  if (nargs >= 2) {
    double pos = tod(args[1]);
    if (isnan(pos) || pos < 0) pos = 0;
    if (pos > D(str_len)) return mkval(T_BOOL, 0);
    start = (jsoff_t) pos;
  }
  
  if (search_len == 0) return mkval(T_BOOL, 1);
  if (start + search_len > str_len) return mkval(T_BOOL, 0);
  for (jsoff_t i = start; i <= str_len - search_len; i++) {
    if (memcmp(str_ptr + i, search_ptr, search_len) == 0) return mkval(T_BOOL, 1);
  }
  
  return mkval(T_BOOL, 0);
}

static jsval_t builtin_string_startsWith(ant_t *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "startsWith called on non-string");
  if (nargs == 0) return mkval(T_BOOL, 0);
  jsval_t search = args[0];
  
  if (is_object_type(search)) {
    jsval_t maybe_err = reject_regexp_arg(js, search, "startsWith");
    if (is_err(maybe_err)) return maybe_err;
  }
  search = js_tostring_val(js, search);
  if (is_err(search)) return search;
  
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  jsoff_t search_len, search_off = vstr(js, search, &search_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  const char *search_ptr = (char *) &js->mem[search_off];
  
  if (search_len > str_len) return mkval(T_BOOL, 0);
  if (search_len == 0) return mkval(T_BOOL, 1);
  
  return mkval(T_BOOL, memcmp(str_ptr, search_ptr, search_len) == 0 ? 1 : 0);
}

static jsval_t builtin_string_endsWith(ant_t *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "endsWith called on non-string");
  if (nargs == 0) return mkval(T_BOOL, 0);
  jsval_t search = args[0];
  
  if (is_object_type(search)) {
    jsval_t maybe_err = reject_regexp_arg(js, search, "endsWith");
    if (is_err(maybe_err)) return maybe_err;
  }
  search = js_tostring_val(js, search);
  if (is_err(search)) return search;
  
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  jsoff_t search_len, search_off = vstr(js, search, &search_len);
  
  const char *str_ptr = (char *) &js->mem[str_off];
  const char *search_ptr = (char *) &js->mem[search_off];
  
  if (search_len > str_len) return mkval(T_BOOL, 0);
  if (search_len == 0) return mkval(T_BOOL, 1);
  
  return mkval(T_BOOL, memcmp(str_ptr + str_len - search_len, search_ptr, search_len) == 0 ? 1 : 0);
}

static jsval_t builtin_string_replace(ant_t *js, jsval_t *args, int nargs) {
  jsval_t this_unwrapped = unwrap_primitive(js, js->this_val);
  jsval_t str = js_tostring_val(js, this_unwrapped);
  if (is_err(str)) return str;
  if (nargs < 1) return str;
  
  if (is_object_type(args[0])) {
    bool called = false;
    jsval_t replacement_arg = nargs > 1 ? args[1] : js_mkundef();
    jsval_t call_args[2] = { str, replacement_arg };
    jsval_t dispatched = maybe_call_symbol_method(
      js, args[0], get_replace_sym(), args[0], call_args, 2, &called
    );
    if (is_err(dispatched)) return dispatched;
    if (called) return dispatched;
  }
  if (nargs < 2) return str;
  
  jsval_t search = args[0];
  jsval_t replacement = args[1];
  
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  
  bool is_regex = false;
  bool global_flag = false;
  bool is_func_replacement = (vtype(replacement) == T_FUNC);
  char *pattern_buf = NULL;
  jsoff_t pattern_len = 0;
  
  if (vtype(search) == T_OBJ) {
    jsoff_t pattern_off = lkp(js, search, "source", 6);
    if (pattern_off == 0) goto not_regex;
    
    jsval_t pattern_val = resolveprop(js, mkval(T_PROP, pattern_off));
    if (vtype(pattern_val) != T_STR) goto not_regex;
    
    is_regex = true;
    jsoff_t plen, poff = vstr(js, pattern_val, &plen);
    pattern_len = plen;
    pattern_buf = (char *)ant_calloc(plen + 1);
    if (!pattern_buf) return js_mkerr(js, "oom");
    memcpy(pattern_buf, &js->mem[poff], plen);
    pattern_buf[plen] = '\0';
    
    jsoff_t flags_off = lkp(js, search, "flags", 5);
    if (flags_off == 0) { free(pattern_buf); pattern_buf = NULL; goto not_regex; }
    
    jsval_t flags_val = resolveprop(js, mkval(T_PROP, flags_off));
    if (vtype(flags_val) != T_STR) { free(pattern_buf); pattern_buf = NULL; goto not_regex; }
    
    jsoff_t flen, foff = vstr(js, flags_val, &flen);
    const char *flags_str = (char *) &js->mem[foff];
    for (jsoff_t i = 0; i < flen; i++) {
      if (flags_str[i] == 'g') global_flag = true;
    }
  }
  not_regex:
  
  jsoff_t repl_len = 0;
  const char *repl_ptr = NULL;
  if (!is_func_replacement) {
    if (vtype(replacement) != T_STR) { if (pattern_buf) free(pattern_buf); return str; }
    jsoff_t repl_off;
    repl_off = vstr(js, replacement, &repl_len);
    repl_ptr = (char *) &js->mem[repl_off];
  }
  
  size_t result_cap = str_len + repl_len + 256;
  size_t result_len = 0;
  char *result = (char *)ant_calloc(result_cap);
  if (!result) return js_mkerr(js, "oom");

#define ENSURE_RESULT_CAP(need) do { \
  if (result_len + (need) >= result_cap) { \
    result_cap = (result_len + (need) + 1) * 2; \
    char *nr = (char *)ant_realloc(result, result_cap); \
    if (!nr) return js_mkerr(js, "oom"); \
    result = nr; \
  } \
} while(0)
  
  if (is_regex) {
    size_t pcre2_cap = pattern_len * 2 + 16;
    char *pcre2_pattern = (char *)ant_calloc(pcre2_cap);
    if (!pcre2_pattern) return js_mkerr(js, "oom");
    size_t pcre2_len = js_to_pcre2_pattern(pattern_buf, pattern_len, pcre2_pattern, pcre2_cap);

    uint32_t options = PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_UNSET_BACKREF;
    int errcode;
    PCRE2_SIZE erroffset;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pcre2_pattern, pcre2_len, options, &errcode, &erroffset, NULL);
    free(pcre2_pattern);
    if (re == NULL) return js_mkerr(js, "invalid regex pattern");

    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(re, NULL);
    uint32_t capture_count;
    pcre2_pattern_info(re, PCRE2_INFO_CAPTURECOUNT, &capture_count);

    PCRE2_SIZE pos = 0;
    bool replaced = false;

    while (pos <= str_len) {
      int rc = pcre2_match(re, (PCRE2_SPTR)str_ptr, str_len, pos, 0, match_data, NULL);
      if (rc < 0) break;

      PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
      PCRE2_SIZE match_start = ovector[0];
      PCRE2_SIZE match_end = ovector[1];

      PCRE2_SIZE before_len = match_start - pos;
      ENSURE_RESULT_CAP(before_len);
      memcpy(result + result_len, str_ptr + pos, before_len);
      result_len += before_len;

      if (is_func_replacement) {
        int nargs_cb = 1 + capture_count + 2;
        jsval_t *cb_args = (jsval_t *)ant_calloc(nargs_cb * sizeof(jsval_t));
        if (!cb_args) {
          pcre2_match_data_free(match_data);
          pcre2_code_free(re);
          return js_mkerr(js, "oom");
        }
        cb_args[0] = js_mkstr(js, str_ptr + match_start, match_end - match_start);
        for (uint32_t i = 1; i <= capture_count; i++) {
          PCRE2_SIZE cap_start = ovector[2*i];
          PCRE2_SIZE cap_end = ovector[2*i+1];
          if (cap_start == PCRE2_UNSET) {
            cb_args[i] = js_mkundef();
          } else {
            cb_args[i] = js_mkstr(js, str_ptr + cap_start, cap_end - cap_start);
          }
        }
        cb_args[1 + capture_count] = tov((double)match_start);
        cb_args[2 + capture_count] = str;

        jsval_t cb_result = sv_vm_call(js->vm, js, replacement, js_mkundef(), cb_args, nargs_cb, NULL, false);
        free(cb_args);

        if (vtype(cb_result) == T_ERR) {
          pcre2_match_data_free(match_data);
          pcre2_code_free(re);
          return cb_result;
        }

        if (vtype(cb_result) == T_STR) {
          jsoff_t cb_len, cb_off = vstr(js, cb_result, &cb_len);
          ENSURE_RESULT_CAP(cb_len);
          memcpy(result + result_len, &js->mem[cb_off], cb_len);
          result_len += cb_len;
        } else {
          char numbuf[32];
          size_t n = tostr(js, cb_result, numbuf, sizeof(numbuf));
          ENSURE_RESULT_CAP(n);
          memcpy(result + result_len, numbuf, n);
          result_len += n;
        }
      } else {
        repl_capture_t caps_buf[16], *caps = (int)capture_count <= 16 ? caps_buf : ant_calloc(sizeof(repl_capture_t) * capture_count);
        for (uint32_t ci = 0; ci < capture_count; ci++) {
          PCRE2_SIZE cs = ovector[2*(ci+1)], ce = ovector[2*(ci+1)+1];
          if (cs != PCRE2_UNSET) caps[ci] = (repl_capture_t){ str_ptr + cs, ce - cs };
          else caps[ci] = (repl_capture_t){ NULL, 0 };
        }
        repl_template(repl_ptr, repl_len, str_ptr + match_start, match_end - match_start,
          str_ptr, str_len, match_start, caps, (int)capture_count, &result, &result_len, &result_cap);
        if (caps != caps_buf) free(caps);
      }

      if (match_start == match_end) {
        if (pos < str_len) {
          ENSURE_RESULT_CAP(1);
          result[result_len++] = str_ptr[pos];
        }
        pos = match_end + 1;
      } else pos = match_end;

      replaced = true;
      if (!global_flag) break;
    }

    if (pos < str_len) {
      size_t remaining = str_len - pos;
      ENSURE_RESULT_CAP(remaining);
      memcpy(result + result_len, str_ptr + pos, remaining);
      result_len += remaining;
    }

    pcre2_match_data_free(match_data); pcre2_code_free(re);
    if (pattern_buf) free(pattern_buf);
    
    jsval_t ret = replaced ? js_mkstr(js, result, result_len) : str;
    free(result); return ret;
  } else {
    if (vtype(search) != T_STR) { free(result); return str; }
    jsoff_t search_len, search_off = vstr(js, search, &search_len);
    const char *search_ptr = (char *) &js->mem[search_off];
    
    if (search_len > str_len) { free(result); return str; }
    
    for (jsoff_t i = 0; i <= str_len - search_len; i++) {
      if (memcmp(str_ptr + i, search_ptr, search_len) == 0) {
        ENSURE_RESULT_CAP(i);
        memcpy(result + result_len, str_ptr, i);
        result_len += i;
        
        if (is_func_replacement) {
          jsval_t match_str = js_mkstr(js, search_ptr, search_len);
          jsval_t cb_args[1] = { match_str };
          jsval_t cb_result = sv_vm_call(js->vm, js, replacement, js_mkundef(), cb_args, 1, NULL, false);
          
          if (vtype(cb_result) == T_ERR) { free(result); return cb_result; }
          
          if (vtype(cb_result) == T_STR) {
            jsoff_t cb_len, cb_off = vstr(js, cb_result, &cb_len);
            ENSURE_RESULT_CAP(cb_len);
            memcpy(result + result_len, &js->mem[cb_off], cb_len);
            result_len += cb_len;
          } else {
            char numbuf[32];
            size_t n = tostr(js, cb_result, numbuf, sizeof(numbuf));
            ENSURE_RESULT_CAP(n);
            memcpy(result + result_len, numbuf, n);
            result_len += n;
          }
        } else {
          ENSURE_RESULT_CAP(repl_len);
          memcpy(result + result_len, repl_ptr, repl_len);
          result_len += repl_len;
        }
        
        jsoff_t after_start = i + search_len;
        jsoff_t after_len = str_len - after_start;
        if (after_len > 0) {
          ENSURE_RESULT_CAP(after_len);
          memcpy(result + result_len, str_ptr + after_start, after_len);
          result_len += after_len;
        }
        jsval_t ret = js_mkstr(js, result, result_len);
        free(result);
        return ret;
      }
    }
    free(result);
    return str;
  }
#undef ENSURE_RESULT_CAP
}

static jsval_t builtin_string_replaceAll(ant_t *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "replaceAll called on non-string");
  if (nargs < 2) return str;
  
  jsval_t search = args[0];
  jsval_t replacement = args[1];
  
  if (vtype(search) != T_STR) return js_mkerr(js, "replaceAll requires string search pattern");
  if (vtype(replacement) != T_STR) return js_mkerr(js, "replaceAll requires string replacement");
  
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  
  jsoff_t search_len, search_off = vstr(js, search, &search_len);
  const char *search_ptr = (char *) &js->mem[search_off];
  
  jsoff_t repl_len, repl_off = vstr(js, replacement, &repl_len);
  const char *repl_ptr = (char *) &js->mem[repl_off];
  
  if (search_len == 0) {
    size_t total_len = str_len + (str_len + 1) * repl_len;
    char *result = (char *)ant_calloc(total_len + 1);
    if (!result) return js_mkerr(js, "oom");
    
    size_t pos = 0;
    memcpy(result + pos, repl_ptr, repl_len);
    pos += repl_len;
    for (jsoff_t i = 0; i < str_len; i++) {
      result[pos++] = str_ptr[i];
      memcpy(result + pos, repl_ptr, repl_len);
      pos += repl_len;
    }
    jsval_t ret = js_mkstr(js, result, pos);
    free(result);
    return ret;
  }
  
  jsoff_t count = 0;
  for (jsoff_t i = 0; i <= str_len - search_len; i++) {
    if (memcmp(str_ptr + i, search_ptr, search_len) == 0) {
      count++;
      i += search_len - 1;
    }
  }
  
  if (count == 0) return str;
  
  size_t result_total = str_len - (count * search_len) + (count * repl_len);
  char *result = (char *)ant_calloc(result_total + 1);
  if (!result) return js_mkerr(js, "oom");
  
  size_t result_pos = 0;
  jsoff_t str_pos = 0;
  
  while (str_pos <= str_len - search_len) {
    if (memcmp(str_ptr + str_pos, search_ptr, search_len) == 0) {
      memcpy(result + result_pos, repl_ptr, repl_len);
      result_pos += repl_len;
      str_pos += search_len;
    } else {
      result[result_pos++] = str_ptr[str_pos++];
    }
  }
  
  while (str_pos < str_len) {
    result[result_pos++] = str_ptr[str_pos++];
  }
  
  jsval_t ret = js_mkstr(js, result, result_pos);
  free(result);
  return ret;
}


static jsval_t builtin_string_template(ant_t *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "template called on non-string");
  if (nargs < 1 || vtype(args[0]) != T_OBJ) return str;
  
  jsval_t data = args[0];
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  
  size_t result_cap = str_len + 256;
  size_t result_len = 0;
  char *result = (char *)ant_calloc(result_cap);
  if (!result) return js_mkerr(js, "oom");
  jsoff_t i = 0;

#define ENSURE_CAP(need) do { \
  if (result_len + (need) >= result_cap) { \
    result_cap = (result_len + (need) + 1) * 2; \
    char *nr = (char *)ant_realloc(result, result_cap); \
    if (!nr) return js_mkerr(js, "oom"); \
    result = nr; \
  } \
} while(0)
  
  while (i < str_len) {
    if (i < str_len - 3 && str_ptr[i] == '{' && str_ptr[i + 1] == '{') {
      jsoff_t start = i + 2;
      jsoff_t end = start;
      while (end < str_len - 1 && !(str_ptr[end] == '}' && str_ptr[end + 1] == '}')) {
        end++;
      }
      if (end < str_len - 1 && str_ptr[end] == '}' && str_ptr[end + 1] == '}') {
        jsoff_t key_len = end - start;
        jsoff_t prop_off = lkp(js, data, str_ptr + start, key_len);
        
        if (prop_off != 0) {
          jsval_t value = resolveprop(js, mkval(T_PROP, prop_off));
          if (vtype(value) == T_STR) {
            jsoff_t val_len, val_off = vstr(js, value, &val_len);
            ENSURE_CAP(val_len);
            memcpy(result + result_len, &js->mem[val_off], val_len);
            result_len += val_len;
          } else if (vtype(value) == T_NUM) {
            char numstr[32];
            snprintf(numstr, sizeof(numstr), "%g", tod(value));
            size_t num_len = strlen(numstr);
            ENSURE_CAP(num_len);
            memcpy(result + result_len, numstr, num_len);
            result_len += num_len;
          } else if (vtype(value) == T_BOOL) {
            const char *boolstr = vdata(value) ? "true" : "false";
            size_t bool_len = strlen(boolstr);
            ENSURE_CAP(bool_len);
            memcpy(result + result_len, boolstr, bool_len);
            result_len += bool_len;
          }
        }
        i = end + 2;
        continue;
      }
    }
    ENSURE_CAP(1);
    result[result_len++] = str_ptr[i++];
  }
  jsval_t ret = js_mkstr(js, result, result_len);
  free(result);
  return ret;
#undef ENSURE_CAP
}

static jsval_t builtin_string_charCodeAt(ant_t *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "charCodeAt called on non-string");
  
  double idx_d = nargs < 1 ? 0.0 : js_to_number(js, args[0]);
  if (isnan(idx_d)) idx_d = 0.0;
  if (isinf(idx_d) || idx_d > (double)LONG_MAX) return tov(JS_NAN);
  
  long idx_l = (long) idx_d;
  if (idx_l < 0) return tov(JS_NAN);
  
  jsoff_t byte_len; jsoff_t str_off = vstr(js, str, &byte_len);
  const char *str_data = (const char *)&js->mem[str_off];
  
  uint32_t code_unit = utf16_code_unit_at(str_data, byte_len, idx_l);
  if (code_unit == 0xFFFFFFFF) return tov(JS_NAN);
  
  return tov((double) code_unit);
}

static jsval_t builtin_string_codePointAt(ant_t *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "codePointAt called on non-string");
  
  double idx_d = nargs < 1 ? 0.0 : js_to_number(js, args[0]);
  if (isnan(idx_d)) idx_d = 0.0;
  if (isinf(idx_d) || idx_d > (double)LONG_MAX) return js_mkundef();
  
  long idx_l = (long) idx_d;
  if (idx_l < 0) return js_mkundef();
  
  jsoff_t byte_len;
  jsoff_t str_off = vstr(js, str, &byte_len);
  const char *str_data = (const char *)&js->mem[str_off];
  
  uint32_t cp = utf16_codepoint_at(str_data, byte_len, idx_l);
  if (cp == 0xFFFFFFFF) return js_mkundef();
  
  return tov((double) cp);
}

static jsval_t builtin_string_toLowerCase(ant_t *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "toLowerCase called on non-string");
  
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  if (str_len == 0) return js_mkstr(js, "", 0);

  const utf8proc_uint8_t *src = (const utf8proc_uint8_t *)str_ptr;
  utf8proc_ssize_t src_len = (utf8proc_ssize_t)str_len;

  jsoff_t out_len = 0;
  utf8proc_ssize_t pos = 0;
  while (pos < src_len) {
    utf8proc_int32_t cp;
    utf8proc_ssize_t n = utf8_next(src + pos, src_len - pos, &cp);
    if (cp < 0) { out_len++; pos++; continue; }
    utf8proc_uint8_t tmp[4];
    out_len += (jsoff_t)utf8proc_encode_char(utf8proc_tolower(cp), tmp);
    pos += n;
  }

  jsval_t result = js_mkstr(js, NULL, out_len);
  if (is_err(result)) return result;
  jsoff_t result_len, result_off = vstr(js, result, &result_len);
  char *result_ptr = (char *) &js->mem[result_off];

  pos = 0;
  jsoff_t wpos = 0;
  while (pos < src_len) {
    utf8proc_int32_t cp;
    utf8proc_ssize_t n = utf8_next(src + pos, src_len - pos, &cp);
    if (cp < 0) { result_ptr[wpos++] = (char)src[pos]; pos++; continue; }
    wpos += (jsoff_t)utf8proc_encode_char(utf8proc_tolower(cp), (utf8proc_uint8_t *)(result_ptr + wpos));
    pos += n;
  }

  return result;
}

static jsval_t builtin_string_toUpperCase(ant_t *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "toUpperCase called on non-string");
  
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  if (str_len == 0) return js_mkstr(js, "", 0);

  const utf8proc_uint8_t *src = (const utf8proc_uint8_t *)str_ptr;
  utf8proc_ssize_t src_len = (utf8proc_ssize_t)str_len;

  jsoff_t out_len = 0;
  utf8proc_ssize_t pos = 0;
  while (pos < src_len) {
    utf8proc_int32_t cp;
    utf8proc_ssize_t n = utf8_next(src + pos, src_len - pos, &cp);
    if (cp < 0) { out_len++; pos++; continue; }
    utf8proc_uint8_t tmp[4];
    out_len += (jsoff_t)utf8proc_encode_char(utf8proc_toupper(cp), tmp);
    pos += n;
  }

  jsval_t result = js_mkstr(js, NULL, out_len);
  if (is_err(result)) return result;
  jsoff_t result_len, result_off = vstr(js, result, &result_len);
  char *result_ptr = (char *) &js->mem[result_off];

  pos = 0;
  jsoff_t wpos = 0;
  while (pos < src_len) {
    utf8proc_int32_t cp;
    utf8proc_ssize_t n = utf8_next(src + pos, src_len - pos, &cp);
    if (cp < 0) { result_ptr[wpos++] = (char)src[pos]; pos++; continue; }
    wpos += (jsoff_t)utf8proc_encode_char(utf8proc_toupper(cp), (utf8proc_uint8_t *)(result_ptr + wpos));
    pos += n;
  }

  return result;
}

static jsval_t builtin_string_trim(ant_t *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "trim called on non-string");
  
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  
  jsoff_t start = 0, end = str_len;
  while (start < end && is_space(str_ptr[start])) start++;
  while (end > start && is_space(str_ptr[end - 1])) end--;
  
  return js_mkstr(js, str_ptr + start, end - start);
}

static jsval_t builtin_string_trimStart(ant_t *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "trimStart called on non-string");
  
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  
  jsoff_t start = 0;
  while (start < str_len && is_space(str_ptr[start])) start++;
  
  return js_mkstr(js, str_ptr + start, str_len - start);
}

static jsval_t builtin_string_trimEnd(ant_t *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "trimEnd called on non-string");
  
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  
  jsoff_t end = str_len;
  while (end > 0 && is_space(str_ptr[end - 1])) end--;
  
  return js_mkstr(js, str_ptr, end);
}

static jsval_t builtin_string_repeat(ant_t *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "repeat called on non-string");
  if (nargs < 1 || vtype(args[0]) != T_NUM) return js_mkerr(js, "repeat count required");
  
  double count_d = tod(args[0]);
  if (count_d < 0 || count_d != (double)(long)count_d) return js_mkerr(js, "invalid repeat count");
  jsoff_t count = (jsoff_t) count_d;
  
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  
  if (count == 0 || str_len == 0) return js_mkstr(js, "", 0);
  
  jsval_t result = js_mkstr(js, NULL, str_len * count);
  if (is_err(result)) return result;
  
  jsoff_t result_len, result_off = vstr(js, result, &result_len);
  char *result_ptr = (char *) &js->mem[result_off];
  
  for (jsoff_t i = 0; i < count; i++) {
    memcpy(result_ptr + i * str_len, str_ptr, str_len);
  }
  
  return result;
}

static jsval_t builtin_string_padStart(ant_t *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "padStart called on non-string");
  if (nargs < 1 || vtype(args[0]) != T_NUM) return str;
  
  jsoff_t target_len = (jsoff_t)tod(args[0]);
  if (target_len <= 0) return str;
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  size_t str_utf16_len = utf16_strlen(str_ptr, (size_t)str_len);
  
  if ((size_t)target_len <= str_utf16_len) return str;
  
  jsval_t pad_val = js_mkstr(js, " ", 1);
  if (nargs >= 2 && vtype(args[1]) != T_UNDEF) {
    pad_val = coerce_to_str(js, args[1]);
    if (is_err(pad_val)) return pad_val;
  }
  jsoff_t pad_len, pad_off = vstr(js, pad_val, &pad_len);
  const char *pad_str = (char *)&js->mem[pad_off];
  size_t pad_utf16_len = utf16_strlen(pad_str, (size_t)pad_len);
  
  if (pad_utf16_len == 0) return str;
  
  size_t fill_utf16_len = (size_t)target_len - str_utf16_len;
  size_t full_repeats = fill_utf16_len / pad_utf16_len;
  size_t rem_utf16 = fill_utf16_len % pad_utf16_len;
  size_t rem_bytes = 0;
  if (rem_utf16 > 0) {
    int off = utf16_index_to_byte_offset(pad_str, (size_t)pad_len, rem_utf16, NULL);
    if (off < 0) return str;
    rem_bytes = (size_t)off;
  }
  size_t fill_bytes = full_repeats * (size_t)pad_len + rem_bytes;
  size_t total_bytes = fill_bytes + (size_t)str_len;

  jsval_t result = js_mkstr(js, NULL, (jsoff_t)total_bytes);
  if (is_err(result)) return result;
  
  jsoff_t result_len, result_off = vstr(js, result, &result_len);
  char *result_ptr = (char *) &js->mem[result_off];
  
  size_t pos = 0;
  for (size_t i = 0; i < full_repeats; i++) {
    memcpy(result_ptr + pos, pad_str, (size_t)pad_len);
    pos += (size_t)pad_len;
  }
  if (rem_bytes > 0) {
    memcpy(result_ptr + pos, pad_str, rem_bytes);
    pos += rem_bytes;
  }
  memcpy(result_ptr + pos, str_ptr, (size_t)str_len);
  
  return result;
}

static jsval_t builtin_string_padEnd(ant_t *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "padEnd called on non-string");
  if (nargs < 1 || vtype(args[0]) != T_NUM) return str;
  
  jsoff_t target_len = (jsoff_t)tod(args[0]);
  if (target_len <= 0) return str;
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  size_t str_utf16_len = utf16_strlen(str_ptr, (size_t)str_len);
  
  if ((size_t)target_len <= str_utf16_len) return str;
  
  jsval_t pad_val = js_mkstr(js, " ", 1);
  if (nargs >= 2 && vtype(args[1]) != T_UNDEF) {
    pad_val = coerce_to_str(js, args[1]);
    if (is_err(pad_val)) return pad_val;
  }
  jsoff_t pad_len, pad_off = vstr(js, pad_val, &pad_len);
  const char *pad_str = (char *) &js->mem[pad_off];
  size_t pad_utf16_len = utf16_strlen(pad_str, (size_t)pad_len);
  
  if (pad_utf16_len == 0) return str;
  
  size_t fill_utf16_len = (size_t)target_len - str_utf16_len;
  size_t full_repeats = fill_utf16_len / pad_utf16_len;
  size_t rem_utf16 = fill_utf16_len % pad_utf16_len;
  size_t rem_bytes = 0;
  if (rem_utf16 > 0) {
    int off = utf16_index_to_byte_offset(pad_str, (size_t)pad_len, rem_utf16, NULL);
    if (off < 0) return str;
    rem_bytes = (size_t)off;
  }
  size_t fill_bytes = full_repeats * (size_t)pad_len + rem_bytes;
  size_t total_bytes = (size_t)str_len + fill_bytes;

  jsval_t result = js_mkstr(js, NULL, (jsoff_t)total_bytes);
  if (is_err(result)) return result;
  
  jsoff_t result_len, result_off = vstr(js, result, &result_len);
  char *result_ptr = (char *) &js->mem[result_off];
  
  memcpy(result_ptr, str_ptr, (size_t)str_len);
  size_t pos = (size_t)str_len;
  for (size_t i = 0; i < full_repeats; i++) {
    memcpy(result_ptr + pos, pad_str, (size_t)pad_len);
    pos += (size_t)pad_len;
  }
  if (rem_bytes > 0) {
    memcpy(result_ptr + pos, pad_str, rem_bytes);
    pos += rem_bytes;
  }
  
  return result;
}

static jsval_t builtin_string_charAt(ant_t *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "charAt called on non-string");
  
  double idx_d = nargs < 1 ? 0.0 : js_to_number(js, args[0]);
  if (isnan(idx_d)) idx_d = 0;
  else if (idx_d < 0) idx_d = -floor(-idx_d);
  else idx_d = floor(idx_d);
  if (idx_d < 0 || isinf(idx_d)) return js_mkstr(js, "", 0);
  
  jsoff_t idx = (jsoff_t) idx_d;
  jsoff_t byte_len;
  jsoff_t str_off = vstr(js, str, &byte_len);
  const char *str_data = (const char *)&js->mem[str_off];
  
  size_t char_bytes;
  int byte_offset = utf16_index_to_byte_offset(str_data, byte_len, idx, &char_bytes);
  if (byte_offset < 0) return js_mkstr(js, "", 0);
  
  return js_mkstr(js, str_data + byte_offset, char_bytes);
}

static jsval_t builtin_string_at(ant_t *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "at called on non-string");
  
  double idx_d = nargs < 1 ? 0.0 : js_to_number(js, args[0]);
  if (isnan(idx_d) || isinf(idx_d)) return js_mkundef();

  jsoff_t byte_len; jsoff_t str_off = vstr(js, str, &byte_len);
  const char *str_data = (const char *)&js->mem[str_off];
  size_t utf16_len = utf16_strlen(str_data, byte_len);
  
  long idx = (long) idx_d;
  if (idx < 0) idx += (long) utf16_len;
  if (idx < 0 || idx >= (long) utf16_len) return js_mkundef();

  size_t char_bytes;
  int byte_offset = utf16_index_to_byte_offset(str_data, byte_len, idx, &char_bytes);
  if (byte_offset < 0) return js_mkundef();
  
  return js_mkstr(js, str_data + byte_offset, char_bytes);
}

static jsval_t builtin_string_localeCompare(ant_t *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "localeCompare called on non-string");
  if (nargs < 1) return tov(0);
  
  jsval_t that = args[0];
  if (vtype(that) != T_STR) {
    char buf[64];
    size_t n = tostr(js, that, buf, sizeof(buf));
    that = js_mkstr(js, buf, n);
  }
  
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  jsoff_t that_len, that_off = vstr(js, that, &that_len);
  const char *str_ptr = (char *)&js->mem[str_off];
  const char *that_ptr = (char *)&js->mem[that_off];
  
  int result = strcoll(str_ptr, that_ptr);
  if (result < 0) return tov(-1);
  if (result > 0) return tov(1);
  return tov(0);
}

static jsval_t builtin_string_lastIndexOf(ant_t *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "lastIndexOf called on non-string");
  if (nargs == 0) return tov(-1);

  jsval_t search = args[0];
  if (vtype(search) != T_STR) return tov(-1);

  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  jsoff_t search_len, search_off = vstr(js, search, &search_len);
  
  jsoff_t max_start = str_len;
  double dstr_len = D(str_len);
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    double pos = tod(args[1]);
    if (isnan(pos)) pos = dstr_len;
    if (pos < 0) pos = 0;
    if (pos > dstr_len) pos = dstr_len;
    max_start = (jsoff_t) pos;
  }
  
  if (search_len == 0) return tov((double) (max_start > str_len ? str_len : max_start));
  if (search_len > str_len) return tov(-1);

  const char *str_ptr = (char *) &js->mem[str_off];
  const char *search_ptr = (char *) &js->mem[search_off];

  jsoff_t start = (max_start + search_len > str_len) ? str_len - search_len : max_start;
  for (jsoff_t i = start + 1; i > 0; i--) {
    if (memcmp(str_ptr + i - 1, search_ptr, search_len) == 0) return tov((double)(i - 1));
  }
  return tov(-1);
}

static jsval_t builtin_string_concat(ant_t *js, jsval_t *args, int nargs) {
  jsval_t this_unwrapped = unwrap_primitive(js, js->this_val);
  jsval_t str = js_tostring_val(js, this_unwrapped);
  if (is_err(str)) return str;

  jsoff_t total_len;
  jsoff_t base_off = vstr(js, str, &total_len);
  
  jsval_t *str_args = NULL;
  if (nargs > 0) {
    str_args = (jsval_t *)ant_calloc(nargs * sizeof(jsval_t));
    if (!str_args) return js_mkerr(js, "oom");
    for (int i = 0; i < nargs; i++) {
      str_args[i] = js_tostring_val(js, args[i]);
      if (is_err(str_args[i])) { 
        free(str_args);
        return str_args[i];
      }
      jsoff_t arg_len;
      vstr(js, str_args[i], &arg_len);
      total_len += arg_len;
    }
  }

  char *result = (char *)ant_calloc(total_len + 1);
  if (!result) { 
    if (str_args) free(str_args);
    return js_mkerr(js, "oom");
  }

  jsoff_t base_len;
  base_off = vstr(js, str, &base_len);
  memcpy(result, &js->mem[base_off], base_len);
  jsoff_t pos = base_len;

  for (int i = 0; i < nargs; i++) {
    jsoff_t arg_len, arg_off = vstr(js, str_args[i], &arg_len);
    memcpy(result + pos, &js->mem[arg_off], arg_len);
    pos += arg_len;
  }
  result[pos] = '\0';

  jsval_t ret = js_mkstr(js, result, pos);
  free(result); if (str_args) free(str_args);
  
  return ret;
}

static jsval_t builtin_string_normalize(ant_t *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "normalize called on non-string");

  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (const char *)&js->mem[str_off];

  if (str_len == 0) return js_mkstr(js, "", 0);
  utf8proc_option_t opts = UTF8PROC_COMPOSE | UTF8PROC_STABLE;

  if (nargs >= 1 && vtype(args[0]) != T_UNDEF) {
    jsval_t form_val = js_tostring_val(js, args[0]);
    if (is_err(form_val)) return form_val;
    jsoff_t flen, foff = vstr(js, form_val, &flen);
    const char *form = (const char *)&js->mem[foff];

    if (flen == 3 && memcmp(form, "NFC", 3) == 0) {
      opts = UTF8PROC_COMPOSE | UTF8PROC_STABLE;
    } else if (flen == 3 && memcmp(form, "NFD", 3) == 0) {
      opts = UTF8PROC_DECOMPOSE | UTF8PROC_STABLE;
    } else if (flen == 4 && memcmp(form, "NFKC", 4) == 0) {
      opts = UTF8PROC_COMPOSE | UTF8PROC_STABLE | UTF8PROC_COMPAT;
    } else if (flen == 4 && memcmp(form, "NFKD", 4) == 0) {
      opts = UTF8PROC_DECOMPOSE | UTF8PROC_STABLE | UTF8PROC_COMPAT;
    } else return js_mkerr_typed(js, JS_ERR_RANGE, "The normalization form should be one of NFC, NFD, NFKC, NFKD");
  }

  utf8proc_uint8_t *result = NULL;
  utf8proc_ssize_t rlen = utf8proc_map(
    (const utf8proc_uint8_t *)str_ptr, (utf8proc_ssize_t)str_len, &result, opts
  );

  if (rlen < 0 || !result) {
    if (result) free(result);
    return js_mkstr(js, str_ptr, str_len);
  }

  jsval_t ret = js_mkstr(js, (const char *)result, (jsoff_t)rlen);
  free(result);
  
  return ret;
}

static jsval_t builtin_string_fromCharCode(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_mkstr(js, "", 0);

  char *buf = (char *)ant_calloc(nargs + 1);
  if (!buf) return js_mkerr(js, "oom");

  for (int i = 0; i < nargs; i++) {
    if (vtype(args[i]) != T_NUM) { buf[i] = 0; continue; }
    int code = (int) tod(args[i]);
    buf[i] = (char)(code & 0xFF);
  }
  buf[nargs] = '\0';

  jsval_t ret = js_mkstr(js, buf, nargs);
  free(buf);
  return ret;
}

static jsval_t builtin_string_fromCodePoint(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_mkstr(js, "", 0);

  char *buf = (char *)ant_calloc(nargs * 4 + 1);
  if (!buf) return js_mkerr(js, "oom");

  size_t len = 0;
  for (int i = 0; i < nargs; i++) {
    if (vtype(args[i]) != T_NUM) continue;
    double d = tod(args[i]);
    if (d < 0 || d > 0x10FFFF || d != floor(d)) {
      free(buf);
      return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid code point");
    }
    uint32_t cp = (uint32_t)d;
    if (cp < 0x80) {
      buf[len++] = (char)cp;
    } else if (cp < 0x800) {
      buf[len++] = (char)(0xC0 | (cp >> 6));
      buf[len++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      buf[len++] = (char)(0xE0 | (cp >> 12));
      buf[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      buf[len++] = (char)(0x80 | (cp & 0x3F));
    } else {
      buf[len++] = (char)(0xF0 | (cp >> 18));
      buf[len++] = (char)(0x80 | ((cp >> 12) & 0x3F));
      buf[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      buf[len++] = (char)(0x80 | (cp & 0x3F));
    }
  }
  buf[len] = '\0';

  jsval_t ret = js_mkstr(js, buf, len);
  free(buf);
  return ret;
}

static bool string_builder_append_value(
  ant_t *js, char **buf,
  size_t *len, size_t *cap,
  jsval_t value, jsval_t *err
) {
  jsval_t s = js_tostring_val(js, value);
  if (is_err(s)) {
    if (err) *err = s;
    return false;
  }

  jsoff_t slen = 0;
  jsoff_t soff = vstr(js, s, &slen);
  
  size_t need = *len + (size_t)slen + 1;
  if (need > *cap) {
    size_t next = (*cap == 0) ? 64 : *cap;
    while (next < need) next *= 2;
    char *grown = (char *)realloc(*buf, next);
    if (!grown) {
      if (err) *err = js_mkerr(js, "oom");
      return false;
    }
    *buf = grown;
    *cap = next;
  }

  if (slen > 0) memcpy(*buf + *len, &js->mem[soff], (size_t)slen);
  *len += (size_t)slen;
  (*buf)[*len] = '\0';
  return true;
}

static jsval_t builtin_string_raw(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1 || is_null(args[0]) || is_undefined(args[0])) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "String.raw requires a template object");
  }

  jsval_t tmpl = args[0];
  if (!is_object_type(tmpl)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "String.raw requires a template object");
  }

  jsval_t raw = js_get(js, tmpl, "raw");
  if (is_null(raw) || is_undefined(raw) || !is_object_type(raw)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "String.raw requires template.raw");
  }

  jsval_t raw_len_val = js_get(js, raw, "length");
  double raw_len_num = js_to_number(js, raw_len_val);
  if (!isfinite(raw_len_num) || raw_len_num <= 0) return js_mkstr(js, "", 0);

  size_t literal_count = (size_t)raw_len_num;
  if (literal_count == 0) return js_mkstr(js, "", 0);

  char *buf = NULL;
  size_t len = 0; size_t cap = 0;
  jsval_t err = js_mkundef();

  for (size_t i = 0; i < literal_count; i++) {
    jsval_t chunk = js_mkundef();
    if (vtype(raw) == T_ARR) chunk = js_arr_get(js, raw, (jsoff_t)i);
    else {
      char key[32];
      snprintf(key, sizeof(key), "%zu", i);
      chunk = js_get(js, raw, key);
    }

    if (!string_builder_append_value(js, &buf, &len, &cap, chunk, &err)) {
      free(buf);
      return is_err(err) ? err : js_mkerr(js, "oom");
    }

    if (i + 1 < literal_count && (int)(i + 1) < nargs) {
      if (!string_builder_append_value(js, &buf, &len, &cap, args[i + 1], &err)) {
        free(buf); return is_err(err) ? err : js_mkerr(js, "oom");
      }
    }
  }

  jsval_t out = js_mkstr(js, buf ? buf : "", len);
  free(buf);
  
  return out;
}

static jsval_t builtin_number_toString(ant_t *js, jsval_t *args, int nargs) {
  jsval_t num = unwrap_primitive(js, js->this_val);
  if (vtype(num) != T_NUM) return js_mkerr(js, "toString called on non-number");
  
  int radix = 10;
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    radix = (int)tod(args[0]);
    if (radix < 2 || radix > 36) {
      return js_mkerr(js, "radix must be between 2 and 36");
    }
  }
  
  if (radix == 10) {
    char buf[64];
    size_t len = strnum(num, buf, sizeof(buf));
    return js_mkstr(js, buf, len);
  }
  
  double val = tod(num);
  
  if (isnan(val)) return js_mkstr(js, "NaN", 3);
  if (isinf(val)) return val > 0 ? js_mkstr(js, "Infinity", 8) : js_mkstr(js, "-Infinity", 9);
  
  char buf[128];
  char *p = buf + sizeof(buf) - 1;
  *p = '\0';
  
  bool negative = val < 0;
  if (negative) val = -val;
  
  long long int_part = (long long)val;
  double frac_part = val - (double)int_part;
  
  if (int_part == 0) {
    *--p = '0';
  } else {
    while (int_part > 0 && p > buf) {
      int digit = int_part % radix;
      *--p = (char)(digit < 10 ? '0' + digit : 'a' + (digit - 10));
      int_part /= radix;
    }
  }
  
  if (negative && p > buf) {
    *--p = '-';
  }
  
  size_t int_len = strlen(p);
  
  if (frac_part > 0.0000001) {
    char frac_buf[64];
    int frac_pos = 0;
    frac_buf[frac_pos++] = '.';
    
    for (int i = 0; i < 16 && frac_part > 0.0000001 && frac_pos < 63; i++) {
      frac_part *= radix;
      int digit = (int)frac_part;
      frac_buf[frac_pos++] = (char)(digit < 10 ? '0' + digit : 'a' + (digit - 10));
      frac_part -= digit;
    }
    frac_buf[frac_pos] = '\0';
    
    char result[192];
    snprintf(result, sizeof(result), "%s%s", p, frac_buf);
    return js_mkstr(js, result, strlen(result));
  }
  
  return js_mkstr(js, p, int_len);
}

static jsval_t builtin_number_toFixed(ant_t *js, jsval_t *args, int nargs) {
  jsval_t num = unwrap_primitive(js, js->this_val);
  if (vtype(num) != T_NUM) return js_mkerr(js, "toFixed called on non-number");
  
  double d = tod(num);
  if (isnan(d)) return js_mkstr(js, "NaN", 3);
  if (isinf(d)) return d > 0 ? js_mkstr(js, "Infinity", 8) : js_mkstr(js, "-Infinity", 9);
  
  int digits = 0;
  if (nargs >= 1 && vtype(args[0]) != T_UNDEF) {
    digits = (int) tod(args[0]);
    if (digits < 0 || digits > 100) {
      return js_mkerr_typed(js, JS_ERR_RANGE, "toFixed() digits argument must be between 0 and 100");
    }
  }
  
  bool negative = d < 0;
  if (negative) d = -d;
  
  if (d >= 1e21) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.0f", negative ? -d : d);
    return js_mkstr(js, buf, strlen(buf));
  }
  
  double scale = pow(10, digits);
  double scaled = d * scale;
  double rounded = floor(scaled + 0.5);
  
  char digit_buf[128];
  snprintf(digit_buf, sizeof(digit_buf), "%.0f", rounded);
  int digit_len = (int)strlen(digit_buf);
  
  while (digit_len < digits + 1) {
    memmove(digit_buf + 1, digit_buf, digit_len + 1);
    digit_buf[0] = '0';
    digit_len++;
  }
  
  char buf[128];
  int pos = 0;
  
  if (negative && rounded != 0) buf[pos++] = '-';
  int int_digits = digit_len - digits;
  if (int_digits <= 0) int_digits = 1;
  
  for (int i = 0; i < int_digits; i++) {
    buf[pos++] = digit_buf[i];
  }
  
  if (digits > 0) {
    buf[pos++] = '.';
    for (int i = int_digits; i < digit_len; i++) {
      buf[pos++] = digit_buf[i];
    }
  }
  
  buf[pos] = '\0';
  return js_mkstr(js, buf, pos);
}

static jsval_t builtin_number_toPrecision(ant_t *js, jsval_t *args, int nargs) {
  jsval_t num = unwrap_primitive(js, js->this_val);
  if (vtype(num) != T_NUM) return js_mkerr(js, "toPrecision called on non-number");
  
  double d = tod(num);
  if (isnan(d)) return js_mkstr(js, "NaN", 3);
  if (isinf(d)) return d > 0 ? js_mkstr(js, "Infinity", 8) : js_mkstr(js, "-Infinity", 9);
  
  if (nargs < 1 || vtype(args[0]) == T_UNDEF) {
    char buf[64];
    size_t len = strnum(num, buf, sizeof(buf));
    return js_mkstr(js, buf, len);
  }
  
  int precision = (int) tod(args[0]);
  if (precision < 1 || precision > 100) {
    return js_mkerr_typed(js, JS_ERR_RANGE, "toPrecision() argument must be between 1 and 100");
  }
  
  bool negative = d < 0;
  if (negative) d = -d;
  
  if (d == 0) {
    char buf[128];
    int pos = 0;
    if (negative) buf[pos++] = '-';
    buf[pos++] = '0';
    if (precision > 1) {
      buf[pos++] = '.';
      for (int i = 1; i < precision; i++) buf[pos++] = '0';
    }
    buf[pos] = '\0';
    return js_mkstr(js, buf, pos);
  }
  
  int exp = (int) floor(log10(d));
  bool use_exp = (exp < -(precision - 1) - 1) || (exp >= precision);
  
  if (use_exp) {
    double mantissa = d / pow(10, exp);
    double scale = pow(10, precision - 1);
    double rounded = floor(mantissa * scale + 0.5);
    
    if (rounded >= scale * 10) {
      rounded /= 10;
      exp++;
    }
    
    char digit_buf[32];
    snprintf(digit_buf, sizeof(digit_buf), "%.0f", rounded);
    int digit_len = (int)strlen(digit_buf);
    
    char buf[128];
    int pos = 0;
    if (negative) buf[pos++] = '-';
    buf[pos++] = digit_buf[0];
    if (precision > 1) {
      buf[pos++] = '.';
      for (int i = 1; i < precision; i++) {
        buf[pos++] = (i < digit_len) ? digit_buf[i] : '0';
      }
    }
    buf[pos++] = 'e';
    buf[pos++] = (exp >= 0) ? '+' : '-';
    if (exp < 0) exp = -exp;
    snprintf(buf + pos, sizeof(buf) - pos, "%d", exp);
    return js_mkstr(js, buf, strlen(buf));
  } else {
    int digits_after_point = precision - exp - 1;
    if (digits_after_point < 0) digits_after_point = 0;
    
    double scale = pow(10, digits_after_point);
    double rounded = floor(d * scale + 0.5);
    
    char digit_buf[64];
    snprintf(digit_buf, sizeof(digit_buf), "%.0f", rounded);
    int digit_len = (int)strlen(digit_buf);
    
    while (digit_len < digits_after_point + 1) {
      memmove(digit_buf + 1, digit_buf, digit_len + 1);
      digit_buf[0] = '0';
      digit_len++;
    }
    
    char buf[128];
    int pos = 0;
    if (negative) buf[pos++] = '-';
    
    int int_digits = digit_len - digits_after_point;
    for (int i = 0; i < int_digits; i++) {
      buf[pos++] = digit_buf[i];
    }
    
    if (digits_after_point > 0) {
      buf[pos++] = '.';
      for (int i = int_digits; i < digit_len; i++) {
        buf[pos++] = digit_buf[i];
      }
    }
    
    buf[pos] = '\0';
    return js_mkstr(js, buf, pos);
  }
}

static jsval_t builtin_number_toExponential(ant_t *js, jsval_t *args, int nargs) {
  jsval_t num = unwrap_primitive(js, js->this_val);
  if (vtype(num) != T_NUM) return js_mkerr(js, "toExponential called on non-number");
  
  double d = tod(num);
  if (isnan(d)) return js_mkstr(js, "NaN", 3);
  if (isinf(d)) return d > 0 ? js_mkstr(js, "Infinity", 8) : js_mkstr(js, "-Infinity", 9);
  
  int digits = -1;
  if (nargs >= 1 && vtype(args[0]) != T_UNDEF) {
    digits = (int) tod(args[0]);
    if (digits < 0 || digits > 100) {
      return js_mkerr_typed(js, JS_ERR_RANGE, "toExponential() argument must be between 0 and 100");
    }
  }
  
  bool negative = d < 0;
  if (negative) d = -d;
  
  int exp = 0;
  if (d != 0) {
    exp = (int) floor(log10(d));
    double test = d / pow(10, exp);
    if (test >= 10) { exp++; test /= 10; }
    if (test < 1) { exp--; test *= 10; }
  }
  
  if (digits < 0) {
    char temp[32];
    snprintf(temp, sizeof(temp), "%.15g", d);
    int sig = 0;
    for (int i = 0; temp[i] && temp[i] != 'e' && temp[i] != 'E'; i++) {
      if (temp[i] == '.') continue;
      if (temp[i] >= '0' && temp[i] <= '9') if (temp[i] != '0' || sig > 0) sig++;
    }
    digits = sig > 0 ? sig - 1 : 0;
    if (digits > 20) digits = 20;
  }
  
  double mantissa = d / pow(10, exp);
  double scale = pow(10, digits);
  double scaled = mantissa * scale;
  double rounded = floor(scaled + 0.5);
  
  if (rounded >= scale * 10) {
    rounded /= 10;
    exp++;
  }
  
  char buf[64];
  int pos = 0;
  
  if (negative) buf[pos++] = '-';
  
  char digit_buf[32];
  snprintf(digit_buf, sizeof(digit_buf), "%.0f", rounded);
  int digit_len = (int)strlen(digit_buf);
  
  while (digit_len < digits + 1) {
    memmove(digit_buf + 1, digit_buf, digit_len + 1);
    digit_buf[0] = '0';
    digit_len++;
  }
  
  buf[pos++] = digit_buf[0];
  
  if (digits > 0) {
    buf[pos++] = '.';
    for (int i = 1; i <= digits; i++) {
      buf[pos++] = (i < digit_len) ? digit_buf[i] : '0';
    }
  }
  
  buf[pos++] = 'e';
  buf[pos++] = (exp >= 0) ? '+' : '-';
  if (exp < 0) exp = -exp;
  snprintf(buf + pos, sizeof(buf) - pos, "%d", exp);
  
  return js_mkstr(js, buf, strlen(buf));
}

static jsval_t builtin_number_valueOf(ant_t *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  jsval_t num = unwrap_primitive(js, js->this_val);
  if (vtype(num) != T_NUM) return js_mkerr(js, "valueOf called on non-number");
  return num;
}

static jsval_t builtin_number_toLocaleString(ant_t *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  jsval_t num = unwrap_primitive(js, js->this_val);
  if (vtype(num) != T_NUM) return js_mkerr(js, "toLocaleString called on non-number");
  double d = tod(num);
  char raw[64];
  strnum(num, raw, sizeof(raw));
  if (!isfinite(d) || strchr(raw, 'e') || strchr(raw, 'E'))
    return js_mkstr(js, raw, strlen(raw));
  char *dot = strchr(raw, '.');
  size_t int_len = dot ? (size_t)(dot - raw) : strlen(raw);
  size_t start = (raw[0] == '-') ? 1 : 0;
  size_t frac_len = dot ? strlen(dot) : 0;
  char buf[128];
  size_t pos = 0;
  if (start) buf[pos++] = '-';
  for (size_t i = start; i < int_len; i++) {
    buf[pos++] = raw[i];
    size_t remaining = int_len - 1 - i;
    if (remaining > 0 && remaining % 3 == 0) buf[pos++] = ',';
  }
  if (frac_len) memcpy(buf + pos, dot, frac_len);
  pos += frac_len;
  buf[pos] = '\0';
  return js_mkstr(js, buf, pos);
}

static jsval_t builtin_string_valueOf(ant_t *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "valueOf called on non-string");
  return str;
}

static jsval_t builtin_string_toString(ant_t *js, jsval_t *args, int nargs) {
  return builtin_string_valueOf(js, args, nargs);
}

static jsval_t builtin_boolean_valueOf(ant_t *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  jsval_t b = unwrap_primitive(js, js->this_val);
  if (vtype(b) != T_BOOL) return js_mkerr(js, "valueOf called on non-boolean");
  return b;
}

static jsval_t builtin_boolean_toString(ant_t *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  jsval_t b = unwrap_primitive(js, js->this_val);
  if (vtype(b) != T_BOOL) return js_mkerr(js, "toString called on non-boolean");
  return vdata(b) ? js_mkstr(js, "true", 4) : js_mkstr(js, "false", 5);
}

static jsval_t builtin_parseInt(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return tov(JS_NAN);
  
  jsval_t str_val = args[0];
  if (vtype(str_val) != T_STR) {
    const char *str = js_str(js, str_val);
    str_val = js_mkstr(js, str, strlen(str));
  }
  
  jsoff_t str_len, str_off = vstr(js, str_val, &str_len);
  const char *str = (char *) &js->mem[str_off];
  
  int radix = 0;
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    radix = (int) tod(args[1]);
    if (radix != 0 && (radix < 2 || radix > 36)) return tov(JS_NAN);
  }
  
  jsoff_t i = 0;
  while (i < str_len && is_space(str[i])) i++;
  
  if (i >= str_len) return tov(JS_NAN);
  
  int sign = 1;
  if (str[i] == '-') {
    sign = -1;
    i++;
  } else if (str[i] == '+') {
    i++;
  }
  
  if ((radix == 0 || radix == 16) && i + 1 < str_len && str[i] == '0' && (str[i + 1] == 'x' || str[i + 1] == 'X')) {
    radix = 16;
    i += 2;
  }
  
  if (radix == 0) radix = 10;
  
  double result = 0;
  bool found_digit = false;
  
  while (i < str_len) {
    char ch = str[i];
    int digit = -1;
    
    if (ch >= '0' && ch <= '9') {
      digit = ch - '0';
    } else if (ch >= 'a' && ch <= 'z') {
      digit = ch - 'a' + 10;
    } else if (ch >= 'A' && ch <= 'Z') {
      digit = ch - 'A' + 10;
    }
    
    if (digit < 0 || digit >= radix) break;
    
    result = result * radix + digit;
    found_digit = true;
    i++;
  }
  
  if (!found_digit) return tov(JS_NAN);
  
  return tov(sign * result);
}

static jsval_t builtin_parseFloat(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return tov(JS_NAN);
  
  jsval_t str_val = args[0];
  if (vtype(str_val) != T_STR) {
    const char *str = js_str(js, str_val);
    str_val = js_mkstr(js, str, strlen(str));
  }
  
  jsoff_t str_len, str_off = vstr(js, str_val, &str_len);
  const char *str = (char *) &js->mem[str_off];
  
  jsoff_t i = 0;
  while (i < str_len && is_space(str[i])) i++;
  
  if (i >= str_len) return tov(JS_NAN);
  
  char *end;
  double result = strtod(&str[i], &end);
  
  if (end == &str[i]) return tov(JS_NAN);
  
  return tov(result);
}

static jsval_t builtin_btoa(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "btoa requires 1 argument");
  
  jsval_t str_val = args[0];
  if (vtype(str_val) != T_STR) {
    const char *str = js_str(js, str_val);
    str_val = js_mkstr(js, str, strlen(str));
  }
  
  jsoff_t str_len, str_off = vstr(js, str_val, &str_len);
  const char *str = (char *) &js->mem[str_off];
  
  size_t out_len;
  char *out = ant_base64_encode((const uint8_t *)str, str_len, &out_len);
  if (!out) return js_mkerr(js, "out of memory");
  
  jsval_t result = js_mkstr(js, out, out_len);
  free(out);
  
  return result;
}

static jsval_t builtin_atob(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "atob requires 1 argument");
  
  jsval_t str_val = args[0];
  if (vtype(str_val) != T_STR) {
    const char *str = js_str(js, str_val);
    str_val = js_mkstr(js, str, strlen(str));
  }
  
  jsoff_t str_len, str_off = vstr(js, str_val, &str_len);
  const char *str = (char *) &js->mem[str_off];
  if (str_len == 0) return js_mkstr(js, "", 0);
  
  size_t out_len;
  uint8_t *out = ant_base64_decode(str, str_len, &out_len);
  if (!out) return js_mkerr(js, "atob: invalid base64 string");
  
  jsval_t result = js_mkstr(js, (char *)out, out_len);
  free(out);
  
  return result;
}

static jsval_t builtin_resolve_internal(ant_t *js, jsval_t *args, int nargs);
static jsval_t builtin_reject_internal(ant_t *js, jsval_t *args, int nargs);
static void resolve_promise(ant_t *js, jsval_t p, jsval_t val);
static void reject_promise(ant_t *js, jsval_t p, jsval_t val);

static size_t strpromise(ant_t *js, jsval_t value, char *buf, size_t len) {
  uint32_t pid = get_promise_id(js, value);
  promise_data_entry_t *pd = get_promise_data(pid, false);
  
  const char *content;
  char *allocated = NULL;
  
  if (!pd || pd->state == 0) {
    content = "<pending>";
  } else if (pd->state == 2) {
    char *val = tostr_alloc(js, pd->value);
    allocated = ant_calloc(strlen(val) + 12);
    sprintf(allocated, "<rejected> %s", val);
    free(val);
    content = allocated;
  } else { content = allocated = tostr_alloc(js, pd->value); }
  
  size_t result = (pd && pd->trigger_pid)
    ? (size_t)snprintf(buf, len, "Promise {\n  %s,\n  Symbol(async_id): %u,\n  Symbol(trigger_async_id): %u\n}", content, pid, pd->trigger_pid)
    : (size_t)snprintf(buf, len, "Promise {\n  %s,\n  Symbol(async_id): %u\n}", content, pid);
  
  if (allocated) free(allocated);
  return result;
}

static promise_data_entry_t *get_promise_data(uint32_t promise_id, bool create) {
  promise_data_entry_t *entry = NULL;
  HASH_FIND(hh, promise_registry, &promise_id, sizeof(uint32_t), entry);
  if (entry) return entry;
  if (!create) return NULL;
  
  entry = (promise_data_entry_t *)malloc(sizeof(promise_data_entry_t));
  entry->promise_id = promise_id;
  entry->trigger_pid = 0;
  entry->obj_offset = 0;
  entry->state = 0;
  entry->value = js_mkundef();
  entry->has_rejection_handler = false;
  utarray_new(entry->handlers, &promise_handler_icd);
  HASH_ADD(hh, promise_registry, promise_id, sizeof(uint32_t), entry);
  
  return entry;
}

static uint32_t get_promise_id(ant_t *js, jsval_t p) {
  jsval_t p_obj = js_as_obj(p);
  jsval_t pid_val = get_slot(js, p_obj, SLOT_PID);
  if (vtype(pid_val) == T_UNDEF) return 0;
  return (uint32_t)tod(pid_val);
}

static jsval_t mkpromise(ant_t *js) {
  jsval_t obj = mkobj(js, 0);
  if (is_err(obj)) return obj;

  uint32_t pid = next_promise_id++;
  set_slot(js, obj, SLOT_PID, tov((double)pid));

  jsval_t promise_ctor = js_get(js, js_glob(js), "Promise");
  if (vtype(promise_ctor) == T_FUNC || vtype(promise_ctor) == T_CFUNC) {
    set_slot(js, obj, SLOT_CTOR, promise_ctor);
  }

  jsval_t promise_proto = get_ctor_proto(js, "Promise", 7);
  if (is_object_type(promise_proto)) {
    set_slot(js, obj, SLOT_PROTO, promise_proto);
  }
  promise_data_entry_t *pd = get_promise_data(pid, true);
  if (pd) pd->obj_offset = (jsoff_t)vdata(js_as_obj(obj));
  
  return mkval(T_PROMISE, vdata(obj));
}

static inline void trigger_handlers(ant_t *js, jsval_t p) {
  uint32_t pid = get_promise_id(js, p);
  queue_promise_trigger(pid);
}

void js_process_promise_handlers(ant_t *js, uint32_t pid) {
  promise_data_entry_t *pd = get_promise_data(pid, false);
  if (!pd) return;
  
  int state = pd->state;
  jsval_t val = pd->value;
  
  unsigned int len = utarray_len(pd->handlers);
  if (len == 0) { return; } pd->processing = true;
  
  for (unsigned int i = 0; i < len; i++) {
    promise_handler_t *h = (promise_handler_t *)utarray_eltptr(pd->handlers, i);
    jsval_t handler = (state == 1) ? h->onFulfilled : h->onRejected;
    
    if (vtype(handler) == T_FUNC || vtype(handler) == T_CFUNC) {
      jsval_t res;
      if (vtype(handler) == T_CFUNC) {
        jsval_t (*fn)(ant_t *, jsval_t *, int) = (jsval_t(*)(ant_t *, jsval_t *, int)) vdata(handler);
        res = fn(js, &val, 1);
      } else {
        jsval_t call_args[] = { val };
        res = sv_vm_call(js->vm, js, handler, js_mkundef(), call_args, 1, NULL, false);
      }
       
      if (is_err(res)) {
        jsval_t reject_val = js->thrown_value;
        if (vtype(reject_val) == T_UNDEF) reject_val = res;
        js->thrown_exists = false;
        js->thrown_value = js_mkundef();
        js->thrown_stack = js_mkundef();
        reject_promise(js, h->nextPromise, reject_val);
      } else resolve_promise(js, h->nextPromise, res);
    } else {
      if (state == 1) resolve_promise(js, h->nextPromise, val);
      else reject_promise(js, h->nextPromise, val);
    }
  }

  pd->processing = false;
  utarray_clear(pd->handlers);
}

static void resolve_promise(ant_t *js, jsval_t p, jsval_t val) {
  uint32_t pid = get_promise_id(js, p);
  promise_data_entry_t *pd = get_promise_data(pid, false);
  if (!pd || pd->state != 0) return;

  if (vtype(val) == T_PROMISE) {
    uint32_t val_pid = get_promise_id(js, val);
    if (val_pid == pid) {
      jsval_t err = js_mkerr(js, "TypeError: Chaining cycle");
      return reject_promise(js, p, err);
    }
    
    jsval_t res_obj = mkobj(js, 0);
    set_slot(js, res_obj, SLOT_DATA, p);
    set_slot(js, res_obj, SLOT_CFUNC, js_mkfun(builtin_resolve_internal));
    jsval_t res_fn = js_obj_to_func(res_obj);
    
    jsval_t rej_obj = mkobj(js, 0);
    set_slot(js, rej_obj, SLOT_DATA, p);
    set_slot(js, rej_obj, SLOT_CFUNC, js_mkfun(builtin_reject_internal));
    jsval_t rej_fn = js_obj_to_func(rej_obj);
    
    jsval_t call_args[] = { res_fn, rej_fn };
    jsval_t then_prop = js_get(js, val, "then");
    
    if (vtype(then_prop) == T_FUNC || vtype(then_prop) == T_CFUNC) {
      (void)sv_vm_call(js->vm, js, then_prop, val, call_args, 2, NULL, false); return;
    }
  }

  pd->state = 1;
  pd->value = val;
  trigger_handlers(js, p);
}

static void reject_promise(ant_t *js, jsval_t p, jsval_t val) {
  uint32_t pid = get_promise_id(js, p);
  promise_data_entry_t *pd = get_promise_data(pid, false);
  if (!pd || pd->state != 0) return;

  pd->state = 2;
  pd->value = val;
  
  if (!pd->has_rejection_handler) {
    promise_data_entry_t *existing = NULL;
    HASH_FIND(hh_unhandled, unhandled_rejections, &pd->promise_id, sizeof(uint32_t), existing);
    if (!existing) {
      HASH_ADD(hh_unhandled, unhandled_rejections, promise_id, sizeof(uint32_t), pd);
    }
  }
  
  trigger_handlers(js, p);
}

static jsval_t builtin_resolve_internal(ant_t *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t p = get_slot(js, me, SLOT_DATA);
  if (vtype(p) != T_PROMISE) return js_mkundef();
  resolve_promise(js, p, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static jsval_t builtin_reject_internal(ant_t *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t p = get_slot(js, me, SLOT_DATA);
  if (vtype(p) != T_PROMISE) return js_mkundef();
  reject_promise(js, p, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static jsval_t builtin_Promise(ant_t *js, jsval_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Promise constructor cannot be invoked without 'new'");
  }
  
  if (nargs == 0 || (vtype(args[0]) != T_FUNC && vtype(args[0]) != T_CFUNC)) {
    const char *val_str = nargs == 0 ? "undefined" : js_str(js, args[0]);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Promise resolver %s is not a function", val_str);
  }
  
  jsval_t p = mkpromise(js);
  jsval_t new_target = js->new_target;
  jsval_t p_obj = js_as_obj(p);
  
  jsval_t promise_proto = get_ctor_proto(js, "Promise", 7);
  jsval_t instance_proto = js_instance_proto_from_new_target(js, promise_proto);
  
  if (vtype(new_target) == T_FUNC || vtype(new_target) == T_CFUNC) set_slot(js, p_obj, SLOT_CTOR, new_target);
  if (is_object_type(instance_proto)) set_slot(js, p_obj, SLOT_PROTO, instance_proto);
  
  jsval_t res_obj = mkobj(js, 0);
  set_slot(js, res_obj, SLOT_DATA, p);
  set_slot(js, res_obj, SLOT_CFUNC, js_mkfun(builtin_resolve_internal));
  
  jsval_t res_fn = js_obj_to_func(res_obj);
  jsval_t rej_obj = mkobj(js, 0);
  
  set_slot(js, rej_obj, SLOT_DATA, p);
  set_slot(js, rej_obj, SLOT_CFUNC, js_mkfun(builtin_reject_internal));
  
  jsval_t rej_fn = js_obj_to_func(rej_obj);
  jsval_t exec_args[] = { res_fn, rej_fn };
  sv_vm_call(js->vm, js, args[0], js_mkundef(), exec_args, 2, NULL, false);
  
  return p;
}

static jsval_t builtin_Promise_resolve(ant_t *js, jsval_t *args, int nargs) {
  jsval_t val = nargs > 0 ? args[0] : js_mkundef();
  if (vtype(val) == T_PROMISE) return val;
  jsval_t p = mkpromise(js);
  resolve_promise(js, p, val);
  return p;
}

static jsval_t builtin_Promise_reject(ant_t *js, jsval_t *args, int nargs) {
  jsval_t val = nargs > 0 ? args[0] : js_mkundef();
  jsval_t p = mkpromise(js);
  reject_promise(js, p, val);
  return p;
}

static jsval_t promise_species_noop_executor(ant_t *js, jsval_t *args, int nargs) {
  return js_mkundef();
}

static jsval_t builtin_promise_then(ant_t *js, jsval_t *args, int nargs) {
  jsval_t p = js->this_val;
  if (vtype(p) != T_PROMISE) return js_mkerr(js, "not a promise");
  
  jsval_t promise_ctor = js_get(js, js_glob(js), "Promise");
  jsval_t species_ctor = promise_ctor;
  jsval_t p_obj = js_as_obj(p);
  jsval_t ctor = js_get(js, p_obj, "constructor");
  
  if (is_err(ctor)) return ctor;
  if (vtype(ctor) == T_UNDEF) ctor = get_slot(js, p_obj, SLOT_CTOR);
  
  jsval_t species = get_ctor_species_value(js, ctor);
  if (is_err(species)) return species;
  
  if (vtype(species) == T_FUNC || vtype(species) == T_CFUNC) {
    species_ctor = species;
  } else if (vtype(species) == T_NULL) {
    species_ctor = promise_ctor;
  } else if (vtype(species) != T_UNDEF) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Promise species is not a constructor");
  }
  
  jsval_t nextP = mkpromise(js);
  if ((vtype(species_ctor) == T_FUNC || vtype(species_ctor) == T_CFUNC)
      && !(vtype(species_ctor) == vtype(promise_ctor)
           && vdata(species_ctor) == vdata(promise_ctor))) {
    jsval_t species_proto = js_get(js, species_ctor, "prototype");
    if (is_object_type(species_proto))
      set_proto(js, js_as_obj(nextP), species_proto);
    set_slot(js, js_as_obj(nextP), SLOT_CTOR, species_ctor);
  } else {
    jsval_t p_proto = get_slot(js, js_as_obj(p), SLOT_PROTO);
    if (vtype(p_proto) == T_OBJ) {
      set_slot(js, js_as_obj(nextP), SLOT_PROTO, p_proto);
      jsval_t p_ctor = get_slot(js, js_as_obj(p), SLOT_CTOR);
      if (vtype(p_ctor) == T_FUNC) set_slot(js, js_as_obj(nextP), SLOT_CTOR, p_ctor);
    }
  }
  
  jsval_t onFulfilled = nargs > 0 ? args[0] : js_mkundef();
  jsval_t onRejected = nargs > 1 ? args[1] : js_mkundef();
  
  uint32_t pid = get_promise_id(js, p);
  uint32_t next_pid = get_promise_id(js, nextP);
  
  promise_data_entry_t *next_pd = get_promise_data(next_pid, false);
  if (next_pd) next_pd->trigger_pid = pid;
  
  promise_data_entry_t *pd = get_promise_data(pid, false);
  if (pd) {
    promise_handler_t h = { onFulfilled, onRejected, nextP };
    utarray_push_back(pd->handlers, &h);
    
    if (vtype(onRejected) == T_FUNC || vtype(onRejected) == T_CFUNC) {
      pd->has_rejection_handler = true;
      promise_data_entry_t *in_unhandled = NULL;
      HASH_FIND(hh_unhandled, unhandled_rejections, &pd->promise_id, sizeof(uint32_t), in_unhandled);
      if (in_unhandled) HASH_DELETE(hh_unhandled, unhandled_rejections, pd);
    }
  }
  
  if (pd && pd->state != 0) trigger_handlers(js, p);
  return nextP;
}

static jsval_t builtin_promise_catch(ant_t *js, jsval_t *args, int nargs) {
  jsval_t args_then[] = { js_mkundef(), nargs > 0 ? args[0] : js_mkundef() };
  return builtin_promise_then(js, args_then, 2);
}

static jsval_t finally_value_thunk(ant_t *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  return get_slot(js, me, SLOT_DATA);
}

static jsval_t finally_thrower(ant_t *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t reason = get_slot(js, me, SLOT_DATA);
  jsval_t rejected = js_mkpromise(js);
  js_reject_promise(js, rejected, reason);
  return rejected;
}

static jsval_t finally_identity_reject(ant_t *js, jsval_t *args, int nargs) {
  jsval_t reason = nargs > 0 ? args[0] : js_mkundef();
  jsval_t rejected = js_mkpromise(js);
  js_reject_promise(js, rejected, reason);
  return rejected;
}

static jsval_t finally_fulfilled_wrapper(ant_t *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t callback = get_slot(js, me, SLOT_DATA);
  jsval_t value = nargs > 0 ? args[0] : js_mkundef();
  
  jsval_t result = js_mkundef();
  if (vtype(callback) == T_FUNC || vtype(callback) == T_CFUNC) {
    result = sv_vm_call(js->vm, js, callback, js_mkundef(), NULL, 0, NULL, false);
    if (is_err(result)) return result;
  }
  
  if (vtype(result) == T_PROMISE || (vtype(result) == T_OBJ && vtype(js_get(js, result, "then")) == T_FUNC)) {
    jsval_t thunk_obj = mkobj(js, 0);
    set_slot(js, thunk_obj, SLOT_DATA, value);
    set_slot(js, thunk_obj, SLOT_CFUNC, js_mkfun(finally_value_thunk));
    jsval_t thunk_fn = js_obj_to_func(thunk_obj);
    
    jsval_t identity_rej_fn = js_mkfun(finally_identity_reject);
    
    jsval_t then_fn = js_get(js, result, "then");
    jsval_t call_args[] = { thunk_fn, identity_rej_fn };
    return sv_vm_call(js->vm, js, then_fn, result, call_args, 2, NULL, false);
  }
  
  return value;
}

static jsval_t finally_rejected_wrapper(ant_t *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t callback = get_slot(js, me, SLOT_DATA);
  jsval_t reason = nargs > 0 ? args[0] : js_mkundef();
  
  jsval_t result = js_mkundef();
  if (vtype(callback) == T_FUNC || vtype(callback) == T_CFUNC) {
    result = sv_vm_call(js->vm, js, callback, js_mkundef(), NULL, 0, NULL, false);
    if (is_err(result)) return result;
  }
  
  if (vtype(result) == T_PROMISE || (vtype(result) == T_OBJ && vtype(js_get(js, result, "then")) == T_FUNC)) {
    jsval_t thrower_obj = mkobj(js, 0);
    set_slot(js, thrower_obj, SLOT_DATA, reason);
    set_slot(js, thrower_obj, SLOT_CFUNC, js_mkfun(finally_thrower));
    
    jsval_t thrower_fn = js_obj_to_func(thrower_obj);
    jsval_t identity_rej_fn = js_mkfun(finally_identity_reject);
    
    jsval_t then_prop = js_get(js, result, "then");
    jsval_t call_args[] = { thrower_fn, identity_rej_fn };
    
    return sv_vm_call(js->vm, js, then_prop, result, call_args, 2, NULL, false);
  }
  
  jsval_t rejected = js_mkpromise(js);
  js_reject_promise(js, rejected, reason);
  return rejected;
}

static jsval_t builtin_promise_finally(ant_t *js, jsval_t *args, int nargs) {
  jsval_t callback = nargs > 0 ? args[0] : js_mkundef();
  
  jsval_t fulfilled_obj = mkobj(js, 0);
  set_slot(js, fulfilled_obj, SLOT_DATA, callback);
  set_slot(js, fulfilled_obj, SLOT_CFUNC, js_mkfun(finally_fulfilled_wrapper));
  jsval_t fulfilled_fn = js_obj_to_func(fulfilled_obj);
  
  jsval_t rejected_obj = mkobj(js, 0);
  set_slot(js, rejected_obj, SLOT_DATA, callback);
  set_slot(js, rejected_obj, SLOT_CFUNC, js_mkfun(finally_rejected_wrapper));
  jsval_t rejected_fn = js_obj_to_func(rejected_obj);
  
  jsval_t args_then[] = { fulfilled_fn, rejected_fn };
  return builtin_promise_then(js, args_then, 2);
}

static jsval_t builtin_Promise_try(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return builtin_Promise_resolve(js, args, 0);
  jsval_t fn = args[0];
  jsval_t *call_args = nargs > 1 ? &args[1] : NULL;
  int call_nargs = nargs > 1 ? nargs - 1 : 0;
  jsval_t res = sv_vm_call(js->vm, js, fn, js_mkundef(), call_args, call_nargs, NULL, false);
  if (is_err(res)) {
    jsval_t reject_val = js->thrown_value;
    if (vtype(reject_val) == T_UNDEF) reject_val = res;
    js->thrown_exists = false;
    js->thrown_value = js_mkundef();
    js->thrown_stack = js_mkundef();
    jsval_t rej_args[] = { reject_val };
    return builtin_Promise_reject(js, rej_args, 1);
  }
  jsval_t res_args[] = { res };
  return builtin_Promise_resolve(js, res_args, 1);
}

static jsval_t builtin_Promise_withResolvers(ant_t *js, jsval_t *args, int nargs) {
  jsval_t p = mkpromise(js);
  
  jsval_t res_obj = mkobj(js, 0);
  set_slot(js, res_obj, SLOT_DATA, p);
  set_slot(js, res_obj, SLOT_CFUNC, js_mkfun(builtin_resolve_internal));
  jsval_t res_fn = js_obj_to_func(res_obj);
  
  jsval_t rej_obj = mkobj(js, 0);
  set_slot(js, rej_obj, SLOT_DATA, p);
  set_slot(js, rej_obj, SLOT_CFUNC, js_mkfun(builtin_reject_internal));
  jsval_t rej_fn = js_obj_to_func(rej_obj);
  
  jsval_t result = js_newobj(js);
  js_setprop(js, result, js_mkstr(js, "promise", 7), p);
  js_setprop(js, result, js_mkstr(js, "resolve", 7), res_fn);
  js_setprop(js, result, js_mkstr(js, "reject", 6), rej_fn);
  
  return result;
}

static jsval_t mkpromise_with_ctor(ant_t *js, jsval_t ctor) {
  jsval_t p = mkpromise(js);
  if (vtype(ctor) != T_FUNC && vtype(ctor) != T_CFUNC) return p;

  jsval_t proto = js_get(js, ctor, "prototype");
  if (is_err(proto)) return proto;
  if (is_object_type(proto)) {
    jsval_t p_obj = js_as_obj(p);
    set_slot(js, p_obj, SLOT_CTOR, ctor);
    set_slot(js, p_obj, SLOT_PROTO, proto);
  }
  return p;
}

static jsval_t builtin_Promise_all_resolve_handler(ant_t *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t tracker = js_get(js, me, "tracker");
  jsval_t index_val = js_get(js, me, "index");
  
  int index = (int)tod(index_val);
  jsval_t value = nargs > 0 ? args[0] : js_mkundef();
  
  jsval_t results = js_get(js, tracker, "results");
  arr_set(js, results, (jsoff_t)index, value);
  
  jsval_t remaining_val = js_get(js, tracker, "remaining");
  int remaining = (int)tod(remaining_val) - 1;
  js_setprop(js, tracker, js_mkstr(js, "remaining", 9), tov((double)remaining));
  
  if (remaining == 0) {
    jsval_t result_promise = get_slot(js, tracker, SLOT_DATA);
    resolve_promise(js, result_promise, mkval(T_ARR, vdata(results)));
  }
  
  return js_mkundef();
}

static jsval_t builtin_Promise_all_reject_handler(ant_t *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t tracker = js_get(js, me, "tracker");
  jsval_t result_promise = get_slot(js, tracker, SLOT_DATA);
  
  jsval_t reason = nargs > 0 ? args[0] : js_mkundef();
  reject_promise(js, result_promise, reason);
  
  return js_mkundef();
}

typedef struct {
  jsval_t tracker;
  int index;
} promise_all_iter_ctx_t;

static iter_action_t promise_all_iter_cb(ant_t *js, jsval_t value, void *ctx, jsval_t *out) {
  promise_all_iter_ctx_t *pctx = (promise_all_iter_ctx_t *)ctx;
  jsval_t item = value;
  
  if (vtype(item) != T_PROMISE) {
    jsval_t wrap_args[] = { item };
    item = builtin_Promise_resolve(js, wrap_args, 1);
  }
  
  jsval_t resolve_obj = mkobj(js, 0);
  set_slot(js, resolve_obj, SLOT_CFUNC, js_mkfun(builtin_Promise_all_resolve_handler));
  js_setprop(js, resolve_obj, js_mkstr(js, "index", 5), tov((double)pctx->index));
  js_setprop(js, resolve_obj, js_mkstr(js, "tracker", 7), pctx->tracker);
  jsval_t resolve_fn = js_obj_to_func(resolve_obj);
  
  jsval_t reject_obj = mkobj(js, 0);
  set_slot(js, reject_obj, SLOT_CFUNC, js_mkfun(builtin_Promise_all_reject_handler));
  js_setprop(js, reject_obj, js_mkstr(js, "tracker", 7), pctx->tracker);
  jsval_t reject_fn = js_obj_to_func(reject_obj);
  
  jsval_t then_args[] = { resolve_fn, reject_fn };
  jsval_t saved_this = js->this_val;
  js->this_val = item;
  builtin_promise_then(js, then_args, 2);
  js->this_val = saved_this;
  
  pctx->index++;
  return ITER_CONTINUE;
}

static jsval_t builtin_Promise_all(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Promise.all requires an iterable");
  
  jsval_t iterable = args[0];
  uint8_t t = vtype(iterable);
  if (t != T_ARR && t != T_OBJ) return js_mkerr(js, "Promise.all requires an iterable");
  
  jsval_t ctor = js->this_val;
  if (vtype(ctor) != T_FUNC && vtype(ctor) != T_CFUNC) ctor = js_mkundef();
  
  jsval_t result_promise = mkpromise_with_ctor(js, ctor);
  if (is_err(result_promise)) return result_promise;
  
  jsval_t tracker = mkobj(js, 0);
  jsval_t results = mkarr(js);
  
  js_setprop(js, tracker, js_mkstr(js, "remaining", 9), tov(0.0));
  js_setprop(js, tracker, js_mkstr(js, "results", 7), results);
  set_slot(js, tracker, SLOT_DATA, result_promise);
  
  promise_all_iter_ctx_t ctx = { .tracker = tracker, .index = 0 };
  jsval_t iter_result = iter_foreach(js, iterable, promise_all_iter_cb, &ctx);
  
  if (is_err(iter_result)) return iter_result;
  
  int len = ctx.index;
  {
    jsoff_t doff = get_dense_buf(js, results);
    if (doff) {
      if ((jsoff_t)len > dense_capacity(js, doff)) doff = dense_grow(js, results, (jsoff_t)len);
      if (doff) dense_set_length(js, doff, (jsoff_t)len);
    }
  }
  
  if (len == 0) {
    resolve_promise(js, result_promise, mkval(T_ARR, vdata(results)));
    return result_promise;
  }
  
  js_setprop(js, tracker, js_mkstr(js, "remaining", 9), tov((double)len));
  return result_promise;
}

typedef struct {
  jsval_t result_promise;
  jsval_t resolve_fn;
  jsval_t reject_fn;
  bool settled;
} promise_race_iter_ctx_t;

static iter_action_t promise_race_iter_cb(ant_t *js, jsval_t value, void *ctx, jsval_t *out) {
  promise_race_iter_ctx_t *pctx = (promise_race_iter_ctx_t *)ctx;
  jsval_t item = value;
  
  if (vtype(item) != T_PROMISE) {
    resolve_promise(js, pctx->result_promise, item);
    pctx->settled = true;
    return ITER_BREAK;
  }
  
  uint32_t item_pid = get_promise_id(js, item);
  promise_data_entry_t *pd = get_promise_data(item_pid, false);
  if (pd) {
    if (pd->state == 1) {
      resolve_promise(js, pctx->result_promise, pd->value);
      pctx->settled = true;
      return ITER_BREAK;
    } else if (pd->state == 2) {
      reject_promise(js, pctx->result_promise, pd->value);
      pctx->settled = true;
      return ITER_BREAK;
    }
  }
  
  jsval_t then_args[] = { pctx->resolve_fn, pctx->reject_fn };
  jsval_t saved_this = js->this_val;
  js->this_val = item;
  builtin_promise_then(js, then_args, 2);
  js->this_val = saved_this;
  
  return ITER_CONTINUE;
}

static jsval_t builtin_Promise_race(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Promise.race requires an iterable");
  
  jsval_t iterable = args[0];
  uint8_t t = vtype(iterable);
  if (t != T_ARR && t != T_OBJ) return js_mkerr(js, "Promise.race requires an iterable");
  
  jsval_t ctor = js->this_val;
  if (vtype(ctor) != T_FUNC && vtype(ctor) != T_CFUNC) ctor = js_mkundef();
  jsval_t result_promise = mkpromise_with_ctor(js, ctor);
  if (is_err(result_promise)) return result_promise;
  
  jsval_t resolve_obj = mkobj(js, 0);
  set_slot(js, resolve_obj, SLOT_CFUNC, js_mkfun(builtin_resolve_internal));
  set_slot(js, resolve_obj, SLOT_DATA, result_promise);
  jsval_t resolve_fn = js_obj_to_func(resolve_obj);
  
  jsval_t reject_obj = mkobj(js, 0);
  set_slot(js, reject_obj, SLOT_CFUNC, js_mkfun(builtin_reject_internal));
  set_slot(js, reject_obj, SLOT_DATA, result_promise);
  jsval_t reject_fn = js_obj_to_func(reject_obj);
  
  promise_race_iter_ctx_t ctx = {
    .result_promise = result_promise,
    .resolve_fn = resolve_fn,
    .reject_fn = reject_fn,
    .settled = false
  };
  
  jsval_t iter_result = iter_foreach(js, iterable, promise_race_iter_cb, &ctx);
  if (is_err(iter_result)) return iter_result;
  
  return result_promise;
}

static jsval_t mk_aggregate_error(ant_t *js, jsval_t errors) {
  jsval_t args[] = { errors, js_mkstr(js, "All promises were rejected", 26) };
  jsoff_t off = lkp(js, js_glob(js), "AggregateError", 14);
  jsval_t ctor = off ? resolveprop(js, mkval(T_PROP, off)) : js_mkundef();
  return sv_vm_call(js->vm, js, ctor, js_mkundef(), args, 2, NULL, false);
}

static bool promise_any_try_resolve(ant_t *js, jsval_t tracker, jsval_t value) {
  if (js_truthy(js, js_get(js, tracker, "resolved"))) return false;
  js_set(js, tracker, "resolved", js_true);
  resolve_promise(js, get_slot(js, tracker, SLOT_DATA), value);
  return true;
}

static void promise_any_record_rejection(ant_t *js, jsval_t tracker, int index, jsval_t reason) {
  jsval_t errors = resolveprop(js, js_get(js, tracker, "errors"));
  arr_set(js, errors, (jsoff_t)index, reason);
  
  int remaining = (int)tod(js_get(js, tracker, "remaining")) - 1;
  js_set(js, tracker, "remaining", tov((double)remaining));
  
  if (remaining == 0) reject_promise(js, get_slot(js, tracker, SLOT_DATA), mk_aggregate_error(js, errors));
}

static jsval_t builtin_Promise_any_resolve_handler(ant_t *js, jsval_t *args, int nargs) {
  jsval_t tracker = js_get(js, js->this_val, "tracker");
  promise_any_try_resolve(js, tracker, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static jsval_t builtin_Promise_any_reject_handler(ant_t *js, jsval_t *args, int nargs) {
  jsval_t tracker = js_get(js, js->this_val, "tracker");
  if (js_truthy(js, js_get(js, tracker, "resolved"))) return js_mkundef();
  
  int index = (int)tod(js_get(js, js->this_val, "index"));
  promise_any_record_rejection(js, tracker, index, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static jsval_t builtin_Promise_any(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Promise.any requires an array");
  
  jsval_t arr = args[0];
  if (vtype(arr) != T_ARR) return js_mkerr(js, "Promise.any requires an array");
  
  int len = (int)get_array_length(js, arr);
  
  if (len == 0) {
    jsval_t reject_args[] = { mk_aggregate_error(js, mkarr(js)) };
    return builtin_Promise_reject(js, reject_args, 1);
  }
  
  jsval_t result_promise = mkpromise(js);
  jsval_t tracker = mkobj(js, 0);
  jsval_t errors = mkarr(js);
  
  set_slot(js, tracker, SLOT_DATA, result_promise);

  js_setprop(js, tracker, js_mkstr(js, "remaining", 9), tov((double)len));
  js_setprop(js, tracker, js_mkstr(js, "errors", 6), errors);
  js_setprop(js, tracker, js_mkstr(js, "resolved", 8), js_false);
  
  {
    jsoff_t doff = get_dense_buf(js, errors);
    if (doff) {
      if ((jsoff_t)len > dense_capacity(js, doff)) doff = dense_grow(js, errors, (jsoff_t)len);
      if (doff) dense_set_length(js, doff, (jsoff_t)len);
    }
  }
  
  for (int i = 0; i < len; i++) {
    jsval_t item = arr_get(js, arr, (jsoff_t)i);
    item = resolveprop(js, item);
    
    if (vtype(item) != T_PROMISE) {
      promise_any_try_resolve(js, tracker, item);
      return result_promise;
    }
    
    uint32_t item_pid = get_promise_id(js, item);
    promise_data_entry_t *pd = get_promise_data(item_pid, false);
    if (pd) {
      pd->has_rejection_handler = true;
      promise_data_entry_t *in_unhandled = NULL;
      HASH_FIND(hh_unhandled, unhandled_rejections, &pd->promise_id, sizeof(uint32_t), in_unhandled);
      if (in_unhandled) HASH_DELETE(hh_unhandled, unhandled_rejections, pd);
      
      if (pd->state == 1) {
        promise_any_try_resolve(js, tracker, pd->value);
        return result_promise;
      } else if (pd->state == 2) {
        promise_any_record_rejection(js, tracker, i, pd->value);
        continue;
      }
    }
    
    jsval_t resolve_obj = mkobj(js, 0);
    set_slot(js, resolve_obj, SLOT_CFUNC, js_mkfun(builtin_Promise_any_resolve_handler));
    js_setprop(js, resolve_obj, js_mkstr(js, "tracker", 7), tracker);
    
    jsval_t reject_obj = mkobj(js, 0);
    set_slot(js, reject_obj, SLOT_CFUNC, js_mkfun(builtin_Promise_any_reject_handler));
    js_setprop(js, reject_obj, js_mkstr(js, "index", 5), tov((double)i));
    js_setprop(js, reject_obj, js_mkstr(js, "tracker", 7), tracker);
    
    jsval_t then_args[] = { js_obj_to_func(resolve_obj), js_obj_to_func(reject_obj) };
    jsval_t saved_this = js->this_val;
    js->this_val = item;
    builtin_promise_then(js, then_args, 2);
    js->this_val = saved_this;
  }
  
  return result_promise;
}

static jsval_t handle_proxy_instanceof(ant_t *js, jsval_t l, jsval_t r, uint8_t ltype) {
  jsval_t target = proxy_read_target(js, r);
  uint8_t ttype = vtype(target);
  
  if (ttype != T_FUNC && ttype != T_CFUNC) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Right-hand side of 'instanceof' is not callable");
  }
  
  {
    jsval_t has_instance = js_get_sym(js, r, get_hasInstance_sym());
    if (is_err(has_instance)) return has_instance;
    uint8_t hit = vtype(has_instance);
    if (hit == T_FUNC || hit == T_CFUNC) {
      jsval_t args[1] = { l };
      jsval_t result = sv_vm_call(js->vm, js, has_instance, r, args, 1, NULL, false);
      if (is_err(result)) return result;
      return js_bool(js_truthy(js, result));
    }
    if (hit != T_UNDEF && hit != T_NULL) {
      return js_mkerr_typed(js, JS_ERR_TYPE, "Symbol.hasInstance is not callable");
    }
  }
  
  jsval_t proto_val = proxy_get(js, r, "prototype", 9);
  uint8_t pt = vtype(proto_val);
  
  if (pt != T_OBJ && pt != T_ARR && pt != T_FUNC) {
    return mkval(T_BOOL, 0);
  }
  
  if (ltype != T_OBJ && ltype != T_ARR && ltype != T_FUNC && ltype != T_PROMISE) {
    return mkval(T_BOOL, 0);
  }
  
  jsval_t current = get_proto(js, l);
  for (int depth = 0; vtype(current) != T_NULL && depth < 32; depth++) {
    if (vdata(current) == vdata(proto_val)) {
      return mkval(T_BOOL, 1);
    }
    current = get_proto(js, current);
  }
  
  return mkval(T_BOOL, 0);
}

static jsval_t handle_cfunc_instanceof(jsval_t l, jsval_t r, uint8_t ltype) {
  jsval_t (*fn)(ant_t *, jsval_t *, int) = (jsval_t(*)(ant_t *, jsval_t *, int)) vdata(r);
  
  if (fn == builtin_Object) return mkval(T_BOOL, ltype == T_OBJ ? 1 : 0);
  if (fn == builtin_Function) return mkval(T_BOOL, (ltype == T_FUNC || ltype == T_CFUNC) ? 1 : 0);
  if (fn == builtin_String) return mkval(T_BOOL, ltype == T_STR ? 1 : 0);
  if (fn == builtin_Number) return mkval(T_BOOL, ltype == T_NUM ? 1 : 0);
  if (fn == builtin_Boolean) return mkval(T_BOOL, ltype == T_BOOL ? 1 : 0);
  if (fn == builtin_Array) return mkval(T_BOOL, ltype == T_ARR ? 1 : 0);
  if (fn == builtin_Promise) return mkval(T_BOOL, ltype == T_PROMISE ? 1 : 0);
  
  return mkval(T_BOOL, 0);
}

static jsval_t walk_prototype_chain(ant_t *js, jsval_t l, jsval_t ctor_proto) {
  jsval_t current = get_proto(js, l);
  const int MAX_DEPTH = 32;
  
  for (int depth = 0; vtype(current) != T_NULL && depth < MAX_DEPTH; depth++) {
    if (vdata(current) == vdata(ctor_proto)) return mkval(T_BOOL, 1);
    current = get_proto(js, current);
  }
  
  return mkval(T_BOOL, 0);
}

jsval_t do_instanceof(ant_t *js, jsval_t l, jsval_t r) {
  uint8_t ltype = vtype(l);
  uint8_t rtype = vtype(r);
  
  if (rtype != T_FUNC && rtype != T_CFUNC) {
    if (is_proxy(js, r)) return handle_proxy_instanceof(js, l, r, ltype);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Right-hand side of 'instanceof' is not callable");
  }
  
  if (rtype == T_CFUNC) {
    return handle_cfunc_instanceof(l, r, ltype);
  }
  
  {
    jsval_t has_instance = js_get_sym(js, r, get_hasInstance_sym());
    if (is_err(has_instance)) return has_instance;
    uint8_t hit = vtype(has_instance);
    if (hit == T_FUNC || hit == T_CFUNC) {
      jsval_t args[1] = { l };
      jsval_t result = sv_vm_call(js->vm, js, has_instance, r, args, 1, NULL, false);
      if (is_err(result)) return result;
      return js_bool(js_truthy(js, result));
    }
    if (hit != T_UNDEF && hit != T_NULL) {
      return js_mkerr_typed(js, JS_ERR_TYPE, "Symbol.hasInstance is not callable");
    }
  }
  
  jsval_t func_obj = js_func_obj(r);
  jsoff_t proto_off = lkp_interned(js, func_obj, INTERN_PROTOTYPE, 9);
  if (proto_off == 0) return mkval(T_BOOL, 0);
  
  jsval_t ctor_proto = resolveprop(js, mkval(T_PROP, proto_off));
  uint8_t pt = vtype(ctor_proto);
  if (pt != T_OBJ && pt != T_ARR && pt != T_FUNC) return mkval(T_BOOL, 0);
  
  if (ltype == T_STR || ltype == T_NUM || ltype == T_BOOL) {
    jsval_t type_proto = get_prototype_for_type(js, ltype);
    return mkval(T_BOOL, vdata(ctor_proto) == vdata(type_proto) ? 1 : 0);
  }
  
  if (ltype != T_OBJ && ltype != T_ARR && ltype != T_FUNC && ltype != T_PROMISE) {
    return mkval(T_BOOL, 0);
  }
  
  return walk_prototype_chain(js, l, ctor_proto);
}

jsval_t do_in(ant_t *js, jsval_t l, jsval_t r) {
  jsoff_t prop_len;
  const char *prop_name;
  char num_buf[32];
  
  jsval_t key = js_to_primitive(js, l, 1);
  if (is_err(key)) return key;
  
  bool is_sym = (vtype(key) == T_SYMBOL);
  
  if (is_sym) {
    const char *d = js_sym_desc(js, key);
    prop_name = d ? d : "symbol";
    prop_len = (jsoff_t)strlen(prop_name);
  } else if (vtype(key) == T_NUM) {
    prop_len = (jsoff_t)strnum(key, num_buf, sizeof(num_buf));
    prop_name = num_buf;
  } else {
    jsval_t key_str = js_tostring_val(js, key);
    if (is_err(key_str)) return key_str;
    jsoff_t prop_off = vstr(js, key_str, &prop_len);
    prop_name = (char *)&js->mem[prop_off];
  }
  
  if (!is_object_type(r)) {
    if (vtype(r) == T_CFUNC) return mkval(T_BOOL, 0);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot use 'in' operator to search for '%.*s' in non-object", (int)prop_len, prop_name);
  }
  
  if (is_proxy(js, r)) {
    jsval_t result = is_sym ? proxy_has_val(js, r, key) : proxy_has(js, r, prop_name, prop_len);
    if (is_err(result)) return result;
    return js_bool(js_truthy(js, result));
  }
  
  if (!is_sym && vtype(r) == T_ARR) {
    unsigned long idx;
    jsoff_t arr_len = get_array_length(js, r);
    if (parse_array_index(prop_name, prop_len, arr_len, &idx)) return mkval(T_BOOL, arr_has(js, r, (jsoff_t)idx) ? 1 : 0);
    if (prop_len == 6 && memcmp(prop_name, "length", 6) == 0) return mkval(T_BOOL, 1);
  }
  
  jsoff_t found = is_sym ? lkp_sym_proto(js, r, (jsoff_t)vdata(key)) : lkp_proto(js, r, prop_name, prop_len);
  return mkval(T_BOOL, found != 0 ? 1 : 0);
}

static jsval_t builtin_import(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "import() requires a string specifier");
  jsval_t ns = js_esm_import_sync(js, args[0]);
  if (is_err(ns)) return builtin_Promise_reject(js, &ns, 1);

  jsval_t promise_args[] = { ns };
  return builtin_Promise_resolve(js, promise_args, 1);
}

static jsval_t builtin_import_meta_resolve(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "import.meta.resolve() requires a string specifier");
  if (vtype(js->import_meta) == T_OBJ) {
    jsval_t filename = js_get(js, js->import_meta, "filename");
    if (vtype(filename) == T_STR) {
      jsoff_t n = 0; jsoff_t off = vstr(js, filename, &n);
      return js_esm_resolve_specifier(js, args[0], (const char *)&js->mem[off]);
    }
  } return js_esm_resolve_specifier(js, args[0], NULL);
}

void js_setup_import_meta(ant_t *js, const char *filename) {
  if (!filename) return;
  
  jsval_t import_meta = mkobj(js, 0);
  if (is_err(import_meta)) return;
  bool is_url = esm_is_url(filename);
  
  jsval_t url_val = is_url ? js_mkstr(js, filename, strlen(filename)) : js_esm_make_file_url(js, filename);
  if (!is_err(url_val)) js_setprop(js, import_meta, js_mkstr(js, "url", 3), url_val);
  
  jsval_t filename_val = js_mkstr(js, filename, strlen(filename));
  if (!is_err(filename_val)) js_setprop(js, import_meta, js_mkstr(js, "filename", 8), filename_val);
  
  if (is_url) {
    char *filename_copy = strdup(filename);
    if (filename_copy) {
      char *last_slash = strrchr(filename_copy, '/');
      char *scheme_end = strstr(filename_copy, "://");
      if (last_slash && scheme_end && last_slash > scheme_end + 2) {
        *last_slash = '\0';
        jsval_t dirname_val = js_mkstr(js, filename_copy, strlen(filename_copy));
        if (!is_err(dirname_val)) js_setprop(js, import_meta, js_mkstr(js, "dirname", 7), dirname_val);
      }
      free(filename_copy);
    }
  } else {
    char *filename_copy = strdup(filename);
    if (filename_copy) {
      char *dir = dirname(filename_copy);
      if (dir) {
        jsval_t dirname_val = js_mkstr(js, dir, strlen(dir));
        if (!is_err(dirname_val)) js_setprop(js, import_meta, js_mkstr(js, "dirname", 7), dirname_val);
      }
      free(filename_copy);
    }
  }
  
  js_setprop(js, import_meta, js_mkstr(js, "main", 4), js_true);
  jsval_t resolve_fn = js_mkfun(builtin_import_meta_resolve);
  js_setprop(js, import_meta, js_mkstr(js, "resolve", 7), resolve_fn);
  
  jsval_t glob = js_glob(js);
  jsoff_t import_off = lkp(js, glob, "import", 6);
  
  if (import_off != 0) {
    jsval_t import_fn = resolveprop(js, mkval(T_PROP, import_off));
    if (vtype(import_fn) == T_FUNC) {
      jsval_t import_obj = js_func_obj(import_fn);
      js_setprop(js, import_obj, js_mkstr(js, "meta", 4), import_meta);
    }
  }
}

static proxy_data_t *get_proxy_data(jsval_t obj) {
  if (vtype(obj) != T_OBJ) return NULL;
  jsoff_t off = (jsoff_t)vdata(obj);
  proxy_data_t *data = NULL;
  HASH_FIND(hh, proxy_registry, &off, sizeof(jsoff_t), data);
  return data;
}

bool is_proxy(ant_t *js, jsval_t obj) {
  (void)js;
  return get_proxy_data(obj) != NULL;
}

static jsval_t proxy_read_target(ant_t *js, jsval_t obj) {
  proxy_data_t *data = get_proxy_data(obj);
  return data ? data->target : obj;
}

static jsoff_t proxy_aware_length(ant_t *js, jsval_t obj) {
  jsval_t src = is_proxy(js, obj) ? proxy_read_target(js, obj) : obj;
  if (vtype(src) == T_ARR) {
    jsoff_t doff = get_dense_buf(js, src);
    if (doff) return dense_length(js, doff);
  }
  jsoff_t off = lkp_interned(js, src, INTERN_LENGTH, 6);
  if (off == 0) return 0;
  jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
  return vtype(len_val) == T_NUM ? (jsoff_t)tod(len_val) : 0;
}

static jsval_t proxy_aware_get_elem(ant_t *js, jsval_t obj, const char *key, size_t key_len) {
  jsval_t src = is_proxy(js, obj) ? proxy_read_target(js, obj) : obj;
  jsoff_t off = lkp(js, src, key, key_len);
  return off ? resolveprop(js, mkval(T_PROP, off)) : js_mkundef();
}

static jsval_t throw_proxy_error(ant_t *js, const char *message) {
  jsval_t err_obj = mkobj(js, 0);
  js_setprop(js, err_obj, js_mkstr(js, "message", 7), js_mkstr(js, message, strlen(message)));
  js_setprop(js, err_obj, js_mkstr(js, "name", 4), js_mkstr(js, "TypeError", 9));
  return js_throw(js, err_obj);
}

static bool proxy_target_is_extensible(ant_t *js, jsval_t obj) {
  uint8_t t = vtype(obj);
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return false;

  jsval_t as_obj = js_as_obj(obj);
  if (js_truthy(js, get_slot(js, as_obj, SLOT_FROZEN))) return false;
  if (js_truthy(js, get_slot(js, as_obj, SLOT_SEALED))) return false;

  jsval_t ext_slot = get_slot(js, as_obj, SLOT_EXTENSIBLE);
  if (vtype(ext_slot) != T_UNDEF) return js_truthy(js, ext_slot);
  return true;
}

static jsoff_t proxy_target_prop_flags(ant_t *js, jsval_t target, jsoff_t prop_off) {
  if (prop_off == 0) return 0;
  jsoff_t obj_off = (jsoff_t)vdata(js_as_obj(target));
  jsoff_t head = loadoff(js, obj_off);
  if ((head & ~(3U | FLAGMASK)) == prop_off) return head;
  return loadoff(js, prop_off);
}

static bool proxy_target_prop_is_nonconfig(ant_t *js, jsval_t target, jsoff_t prop_off) {
  return (proxy_target_prop_flags(js, target, prop_off) & NONCONFIGMASK) != 0;
}

static bool proxy_target_prop_is_const(ant_t *js, jsval_t target, jsoff_t prop_off) {
  return (proxy_target_prop_flags(js, target, prop_off) & CONSTMASK) != 0;
}

static jsval_t proxy_get(ant_t *js, jsval_t proxy, const char *key, size_t key_len) {
  proxy_data_t *data = get_proxy_data(proxy);
  if (!data) return js_mkundef();
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'get' on a proxy that has been revoked");
  
  jsval_t target = data->target;
  jsval_t handler = data->handler;
  
  jsoff_t get_trap_off = vtype(handler) == T_OBJ 
    ? lkp_interned(js, handler, INTERN_GET, 3) 
    : 0;
  
  if (get_trap_off != 0) {
    jsval_t get_trap = resolveprop(js, mkval(T_PROP, get_trap_off));
    if (vtype(get_trap) == T_FUNC || vtype(get_trap) == T_CFUNC) {
      jsval_t key_val = js_mkstr(js, key, key_len);
      
      jsval_t args[3] = { target, key_val, proxy };
      jsval_t result = sv_vm_call(js->vm, js, get_trap, js_mkundef(), args, 3, NULL, false);
      if (is_err(result)) return result;

      jsoff_t prop_off = lkp(js, target, key, key_len);
      if (prop_off != 0 && proxy_target_prop_is_nonconfig(js, target, prop_off) &&
          proxy_target_prop_is_const(js, target, prop_off)) {
        jsval_t target_value = resolveprop(js, mkval(T_PROP, prop_off));
        if (!strict_eq_values(js, result, target_value))
          return js_mkerr_typed(js, JS_ERR_TYPE, "'get' on proxy: trap returned invalid value for non-configurable, non-writable property");
      }

      descriptor_entry_t *desc = lookup_descriptor(js, (jsoff_t)vdata(js_as_obj(target)), key, key_len);
      if (desc && !desc->configurable) {
        if (!desc->has_getter && !desc->has_setter && !desc->writable && prop_off != 0) {
          jsval_t target_value = resolveprop(js, mkval(T_PROP, prop_off));
          if (!strict_eq_values(js, result, target_value))
            return js_mkerr_typed(js, JS_ERR_TYPE, "'get' on proxy: trap returned invalid value for non-configurable, non-writable property");
        }
        if ((desc->has_getter || desc->has_setter) && !desc->has_getter && vtype(result) != T_UNDEF)
          return js_mkerr_typed(js, JS_ERR_TYPE, "'get' on proxy: trap returned non-undefined for property with undefined getter");
      }

      return result;
    }
  }
  
  char key_buf[256];
  size_t len = key_len < sizeof(key_buf) - 1 ? key_len : sizeof(key_buf) - 1;
  memcpy(key_buf, key, len);
  key_buf[len] = '\0';
  
  jsoff_t off = lkp(js, target, key_buf, len);
  if (off != 0) return resolveprop(js, mkval(T_PROP, off));
  
  jsoff_t proto_off = lkp_proto(js, target, key_buf, len);
  if (proto_off != 0) return resolveprop(js, mkval(T_PROP, proto_off));
  
  return js_mkundef();
}

static jsval_t proxy_set(ant_t *js, jsval_t proxy, const char *key, size_t key_len, jsval_t value) {
  proxy_data_t *data = get_proxy_data(proxy);
  if (!data) return js_mkundef();
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'set' on a proxy that has been revoked");
  
  jsval_t target = data->target;
  jsval_t handler = data->handler;
  
  jsoff_t set_trap_off = vtype(handler) == T_OBJ ? lkp_interned(js, handler, INTERN_SET, 3) : 0;
  if (set_trap_off != 0) {
    jsval_t set_trap = resolveprop(js, mkval(T_PROP, set_trap_off));
    if (vtype(set_trap) == T_FUNC || vtype(set_trap) == T_CFUNC) {
      jsval_t key_val = js_mkstr(js, key, key_len);
      jsval_t args[4] = { target, key_val, value, proxy };
      jsval_t result = sv_vm_call(js->vm, js, set_trap, js_mkundef(), args, 4, NULL, false);
      if (is_err(result)) return result;
      if (js_truthy(js, result)) {
        jsoff_t prop_off = lkp(js, target, key, key_len);
        if (prop_off != 0 && proxy_target_prop_is_nonconfig(js, target, prop_off) &&
            proxy_target_prop_is_const(js, target, prop_off)) {
          jsval_t target_value = resolveprop(js, mkval(T_PROP, prop_off));
          if (!strict_eq_values(js, value, target_value))
            return js_mkerr_typed(js, JS_ERR_TYPE, "'set' on proxy: trap returned truthy for non-configurable, non-writable property with different value");
        }

        descriptor_entry_t *desc = lookup_descriptor(js, (jsoff_t)vdata(js_as_obj(target)), key, key_len);
        if (desc && !desc->configurable) {
          if (!desc->has_getter && !desc->has_setter && !desc->writable && prop_off != 0) {
            jsval_t target_value = resolveprop(js, mkval(T_PROP, prop_off));
            if (!strict_eq_values(js, value, target_value))
              return js_mkerr_typed(js, JS_ERR_TYPE, "'set' on proxy: trap returned truthy for non-configurable, non-writable property with different value");
          }
          if ((desc->has_getter || desc->has_setter) && !desc->has_setter)
            return js_mkerr_typed(js, JS_ERR_TYPE, "'set' on proxy: trap returned truthy for property with undefined setter");
        }
      }
      return js_true;
    }
  }
  
  jsval_t key_str = js_mkstr(js, key, key_len);
  js_setprop(js, target, key_str, value);
  return js_true;
}

static jsval_t proxy_has(ant_t *js, jsval_t proxy, const char *key, size_t key_len) {
  proxy_data_t *data = get_proxy_data(proxy);
  if (!data) return js_false;
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'has' on a proxy that has been revoked");
  
  jsval_t target = data->target;
  jsval_t handler = data->handler;
  
  jsoff_t has_trap_off = vtype(handler) == T_OBJ ? lkp(js, handler, "has", 3) : 0;
  if (has_trap_off != 0) {
    jsval_t has_trap = resolveprop(js, mkval(T_PROP, has_trap_off));
    if (vtype(has_trap) == T_FUNC || vtype(has_trap) == T_CFUNC) {
      jsval_t key_val = js_mkstr(js, key, key_len);
      jsval_t args[2] = { target, key_val };
      jsval_t result = sv_vm_call(js->vm, js, has_trap, js_mkundef(), args, 2, NULL, false);
      if (is_err(result)) return result;

      if (!js_truthy(js, result)) {
        jsoff_t prop_off = lkp(js, target, key, key_len);
        descriptor_entry_t *desc = lookup_descriptor(js, (jsoff_t)vdata(js_as_obj(target)), key, key_len);
        bool has_own = (prop_off != 0) || (desc != NULL);

        if ((prop_off != 0 && proxy_target_prop_is_nonconfig(js, target, prop_off)) || (desc && !desc->configurable))
          return js_mkerr_typed(js, JS_ERR_TYPE, "'has' on proxy: trap returned falsy for non-configurable property");

        if (has_own && !proxy_target_is_extensible(js, target))
          return js_mkerr_typed(js, JS_ERR_TYPE, "'has' on proxy: trap returned falsy for existing property on non-extensible target");
      }

      return result;
    }
  }
  
  char key_buf[256];
  size_t len = key_len < sizeof(key_buf) - 1 ? key_len : sizeof(key_buf) - 1;
  memcpy(key_buf, key, len);
  key_buf[len] = '\0';
  
  jsoff_t off = lkp_proto(js, target, key_buf, len);
  return js_bool(off != 0);
}

static jsval_t proxy_delete(ant_t *js, jsval_t proxy, const char *key, size_t key_len) {
  proxy_data_t *data = get_proxy_data(proxy);
  if (!data) return js_true;
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'deleteProperty' on a proxy that has been revoked");
  
  jsval_t target = data->target;
  jsval_t handler = data->handler;
  
  jsoff_t delete_trap_off = vtype(handler) == T_OBJ ? lkp(js, handler, "deleteProperty", 14) : 0;
  if (delete_trap_off != 0) {
    jsval_t delete_trap = resolveprop(js, mkval(T_PROP, delete_trap_off));
    if (vtype(delete_trap) == T_FUNC || vtype(delete_trap) == T_CFUNC) {
      jsval_t key_val = js_mkstr(js, key, key_len);
      jsval_t args[2] = { target, key_val };
      jsval_t result = sv_vm_call(js->vm, js, delete_trap, js_mkundef(), args, 2, NULL, false);
      if (is_err(result)) return result;
      if (js_truthy(js, result)) {
        jsoff_t prop_off = lkp(js, target, key, key_len);
        if (prop_off != 0 && is_nonconfig_prop(js, prop_off))
          return js_mkerr_typed(js, JS_ERR_TYPE, "'deleteProperty' on proxy: trap returned truthy for non-configurable property");
        descriptor_entry_t *desc = lookup_descriptor(js, (jsoff_t)vdata(js_as_obj(target)), key, key_len);
        if (desc && !desc->configurable)
          return js_mkerr_typed(js, JS_ERR_TYPE, "'deleteProperty' on proxy: trap returned truthy for non-configurable property");
      }
      return result;
    }
  }
  
  jsval_t key_str = js_mkstr(js, key, key_len);
  js_setprop(js, target, key_str, js_mkundef());
  return js_true;
}

static jsval_t proxy_get_val(ant_t *js, jsval_t proxy, jsval_t key_val) {
  proxy_data_t *data = get_proxy_data(proxy);
  if (!data) return js_mkundef();
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'get' on a proxy that has been revoked");

  jsval_t target = data->target;
  jsval_t handler = data->handler;

  jsoff_t get_trap_off = vtype(handler) == T_OBJ
    ? lkp_interned(js, handler, INTERN_GET, 3) : 0;
  if (get_trap_off != 0) {
    jsval_t get_trap = resolveprop(js, mkval(T_PROP, get_trap_off));
    if (vtype(get_trap) == T_FUNC || vtype(get_trap) == T_CFUNC) {
      jsval_t args[3] = { target, key_val, proxy };
      return sv_vm_call(js->vm, js, get_trap, js_mkundef(), args, 3, NULL, false);
    }
  }

  if (vtype(key_val) == T_SYMBOL) {
    jsoff_t off = lkp_sym_proto(js, target, (jsoff_t)vdata(key_val));
    return off != 0 ? resolveprop(js, mkval(T_PROP, off)) : js_mkundef();
  }

  return proxy_get(js, proxy, "", 0);
}

static jsval_t proxy_has_val(ant_t *js, jsval_t proxy, jsval_t key_val) {
  proxy_data_t *data = get_proxy_data(proxy);
  if (!data) return js_false;
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'has' on a proxy that has been revoked");

  jsval_t target = data->target;
  jsval_t handler = data->handler;

  jsoff_t has_trap_off = vtype(handler) == T_OBJ ? lkp(js, handler, "has", 3) : 0;
  if (has_trap_off != 0) {
    jsval_t has_trap = resolveprop(js, mkval(T_PROP, has_trap_off));
    if (vtype(has_trap) == T_FUNC || vtype(has_trap) == T_CFUNC) {
      jsval_t args[2] = { target, key_val };
      return sv_vm_call(js->vm, js, has_trap, js_mkundef(), args, 2, NULL, false);
    }
  }

  if (vtype(key_val) == T_SYMBOL) {
    jsoff_t off = lkp_sym_proto(js, target, (jsoff_t)vdata(key_val));
    return js_bool(off != 0);
  }
  return js_false;
}

static jsval_t proxy_delete_val(ant_t *js, jsval_t proxy, jsval_t key_val) {
  proxy_data_t *data = get_proxy_data(proxy);
  if (!data) return js_true;
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'deleteProperty' on a proxy that has been revoked");

  jsval_t target = data->target;
  jsval_t handler = data->handler;

  jsoff_t delete_trap_off = vtype(handler) == T_OBJ ? lkp(js, handler, "deleteProperty", 14) : 0;
  if (delete_trap_off != 0) {
    jsval_t delete_trap = resolveprop(js, mkval(T_PROP, delete_trap_off));
    if (vtype(delete_trap) == T_FUNC || vtype(delete_trap) == T_CFUNC) {
      jsval_t args[2] = { target, key_val };
      jsval_t result = sv_vm_call(js->vm, js, delete_trap, js_mkundef(), args, 2, NULL, false);
      if (is_err(result)) return result;
      if (js_truthy(js, result) && vtype(key_val) == T_SYMBOL) {
        jsoff_t prop_off = lkp_sym(js, target, (jsoff_t)vdata(key_val));
        if (prop_off != 0 && is_nonconfig_prop(js, prop_off))
          return js_mkerr_typed(js, JS_ERR_TYPE, "'deleteProperty' on proxy: trap returned truthy for non-configurable property");
      }
      return result;
    }
  }

  if (vtype(key_val) == T_SYMBOL) {
    jsoff_t sym_off = (jsoff_t)vdata(key_val);
    jsoff_t prop_off = lkp_sym(js, target, sym_off);
    if (prop_off != 0) {
      jsoff_t obj_off = (jsoff_t)vdata(js_as_obj(target));
      jsoff_t first_prop = loadoff(js, obj_off) & ~(3U | FLAGMASK);
      if (first_prop == prop_off) {
        unlink_prop(js, obj_off, prop_off, 0);
      } else {
        for (jsoff_t prev = first_prop; prev != 0; ) {
          jsoff_t np = loadoff(js, prev) & ~(3U | FLAGMASK);
          if (np == prop_off) { unlink_prop(js, obj_off, prop_off, prev); break; }
          prev = np;
        }
      }
    }
  }
  return js_true;
}

jsval_t js_proxy_apply(ant_t *js, jsval_t proxy, jsval_t this_arg, jsval_t *args, int argc) {
  proxy_data_t *data = get_proxy_data(proxy);
  if (!data) return js_mkerr_typed(js, JS_ERR_TYPE, "object is not a function");
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'apply' on a proxy that has been revoked");

  jsval_t target = data->target;
  jsval_t handler = data->handler;
  uint8_t target_type = vtype(target);

  if (target_type != T_FUNC && target_type != T_CFUNC && !(target_type == T_OBJ && is_proxy(js, target)))
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s is not a function", typestr(target_type));

  jsoff_t trap_off = vtype(handler) == T_OBJ ? lkp(js, handler, "apply", 5) : 0;
  if (trap_off != 0) {
    jsval_t trap = resolveprop(js, mkval(T_PROP, trap_off));
    if (vtype(trap) == T_FUNC || vtype(trap) == T_CFUNC) {
      jsval_t args_arr = mkarr(js);
      for (int i = 0; i < argc; i++)
        js_arr_push(js, args_arr, args[i]);
      jsval_t trap_args[3] = { target, this_arg, args_arr };
      return sv_vm_call(js->vm, js, trap, handler, trap_args, 3, NULL, false);
    }
  }

  return sv_vm_call(js->vm, js, target, this_arg, args, argc, NULL, false);
}

jsval_t js_proxy_construct(ant_t *js, jsval_t proxy, jsval_t *args, int argc, jsval_t new_target) {
  proxy_data_t *data = get_proxy_data(proxy);
  if (!data) return js_mkerr_typed(js, JS_ERR_TYPE, "not a constructor");
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'construct' on a proxy that has been revoked");

  jsval_t target = data->target;
  jsval_t handler = data->handler;

  if (vtype(target) != T_FUNC)
    return js_mkerr_typed(js, JS_ERR_TYPE, "not a constructor");

  jsoff_t trap_off = vtype(handler) == T_OBJ ? lkp(js, handler, "construct", 9) : 0;
  if (trap_off != 0) {
    jsval_t trap = resolveprop(js, mkval(T_PROP, trap_off));
    if (vtype(trap) == T_FUNC || vtype(trap) == T_CFUNC) {
      jsval_t args_arr = mkarr(js);
      for (int i = 0; i < argc; i++)
        js_arr_push(js, args_arr, args[i]);
      jsval_t trap_args[3] = { target, args_arr, new_target };
      jsval_t result = sv_vm_call(js->vm, js, trap, js_mkundef(), trap_args, 3, NULL, false);
      if (is_err(result)) return result;
      if (!is_object_type(result))
        return js_mkerr_typed(js, JS_ERR_TYPE, "'construct' on proxy: trap returned non-Object");
      return result;
    }
  }

  jsval_t obj = mkobj(js, 0);
  jsval_t proto = js_getprop_fallback(js, target, "prototype");
  if (is_object_type(proto)) js_set_proto(js, obj, proto);
  jsval_t saved = js->new_target;
  js->new_target = new_target;
  jsval_t ctor_this = obj;
  jsval_t result = sv_vm_call(js->vm, js, target, obj, args, argc, &ctor_this, true);
  js->new_target = saved;
  if (is_err(result)) return result;
  return is_object_type(result) ? result : (is_object_type(ctor_this) ? ctor_this : obj);
}

static jsval_t mkproxy(ant_t *js, jsval_t target, jsval_t handler) {
  jsval_t proxy_obj = mkobj(js, 0);
  jsoff_t off = (jsoff_t)vdata(proxy_obj);
  
  proxy_data_t *data = (proxy_data_t *)ant_calloc(sizeof(proxy_data_t));
  if (!data) return js_mkerr(js, "out of memory");
  
  data->obj_offset = off;
  data->target = target;
  data->handler = handler;
  data->revoked = false;
  
  HASH_ADD(hh, proxy_registry, obj_offset, sizeof(jsoff_t), data);
  return proxy_obj;
}

static jsval_t create_proxy_checked(ant_t *js, jsval_t *args, int nargs, bool require_new) {
  if (require_new && vtype(js->new_target) == T_UNDEF) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Proxy constructor requires 'new'");
  }
  if (nargs < 2) return js_mkerr(js, "Proxy requires two arguments: target and handler");

  jsval_t target = args[0];
  jsval_t handler = args[1];

  uint8_t target_type = vtype(target);
  if (target_type != T_OBJ && target_type != T_FUNC && target_type != T_ARR) {
    return js_mkerr(js, "Proxy target must be an object");
  }

  uint8_t handler_type = vtype(handler);
  if (handler_type != T_OBJ && handler_type != T_FUNC) {
    return js_mkerr(js, "Proxy handler must be an object");
  }

  return mkproxy(js, target, handler);
}

static jsval_t builtin_Proxy(ant_t *js, jsval_t *args, int nargs) {
  return create_proxy_checked(js, args, nargs, true);
}

static jsval_t proxy_revoke_fn(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t func = js->current_func;
  jsval_t ref_slot = get_slot(js, func, SLOT_PROXY_REF);
  
  if (vtype(ref_slot) != T_UNDEF && vdata(ref_slot) != 0) {
    jsval_t proxy = resolveprop(js, mkval(T_PROP, (jsoff_t)vdata(ref_slot)));
    proxy_data_t *data = get_proxy_data(proxy);
    if (data) data->revoked = true;
  }
  
  return js_mkundef();
}

static jsval_t builtin_Proxy_revocable(ant_t *js, jsval_t *args, int nargs) {
  jsval_t proxy = create_proxy_checked(js, args, nargs, false);
  if (is_err(proxy)) return proxy;
  
  jsval_t revoke_obj = mkobj(js, 0);
  set_slot(js, revoke_obj, SLOT_CFUNC, js_mkfun(proxy_revoke_fn));
  set_slot(js, revoke_obj, SLOT_PROXY_REF, proxy);
  
  jsval_t revoke_func = js_obj_to_func(revoke_obj);
  
  jsval_t result = mkobj(js, 0);
  js_setprop(js, result, js_mkstr(js, "proxy", 5), proxy);
  js_setprop(js, result, js_mkstr(js, "revoke", 6), revoke_func);
  
  return result;
}

ant_t *js_create(void *buf, size_t len) {
  assert(
    (uintptr_t)buf <= ((1ULL << 53) - 1) &&
    "ANT_PTR: pointer exceeds 53-bit double-precision integer limit"
  );
  
  intern_init();
  ant_t *js = NULL;
  
  if (len < sizeof(*js) + esize(T_OBJ)) return js;
  memset(buf, 0, len);
  
  js = (ant_t *) buf;
  js->mem = (uint8_t *) (js + 1);
  js->size = (jsoff_t) (len - sizeof(*js));
  js->brk = NANBOX_HEAP_OFFSET;
  js->global = mkobj(js, 0);
  js->size = js->size / 8U * 8U;
  js->this_val = js->global;
  js->new_target = js_mkundef();
  js->length_str = ANT_STRING("length");

  jsval_t glob = js->global;
  jsval_t object_proto = js_mkobj(js);
  set_proto(js, object_proto, js_mknull());
  
  js_setprop(js, object_proto, js_mkstr(js, "toString", 8), js_mkfun(builtin_object_toString));
  js_set_descriptor(js, object_proto, "toString", 8, JS_DESC_W | JS_DESC_C);
  
  js_setprop(js, object_proto, js_mkstr(js, "valueOf", 7), js_mkfun(builtin_object_valueOf));
  js_set_descriptor(js, object_proto, "valueOf", 7, JS_DESC_W | JS_DESC_C);
  
  js_setprop(js, object_proto, js_mkstr(js, "toLocaleString", 14), js_mkfun(builtin_object_toLocaleString));
  js_set_descriptor(js, object_proto, "toLocaleString", 14, JS_DESC_W | JS_DESC_C);
  
  js_setprop(js, object_proto, js_mkstr(js, "hasOwnProperty", 14), js_mkfun(builtin_object_hasOwnProperty));
  js_set_descriptor(js, object_proto, "hasOwnProperty", 14, JS_DESC_W | JS_DESC_C);
  
  js_setprop(js, object_proto, js_mkstr(js, "isPrototypeOf", 13), js_mkfun(builtin_object_isPrototypeOf));
  js_set_descriptor(js, object_proto, "isPrototypeOf", 13, JS_DESC_W | JS_DESC_C);
  
  js_setprop(js, object_proto, js_mkstr(js, "propertyIsEnumerable", 20), js_mkfun(builtin_object_propertyIsEnumerable));
  js_set_descriptor(js, object_proto, "propertyIsEnumerable", 20, JS_DESC_W | JS_DESC_C);
  
  jsval_t proto_getter = js_mkfun(builtin_proto_getter);
  jsval_t proto_setter = js_mkfun(builtin_proto_setter);
  js_set_accessor_desc(js, object_proto, STR_PROTO, STR_PROTO_LEN, proto_getter, proto_setter, JS_DESC_C);
  
  jsval_t function_proto_obj = js_mkobj(js);
  set_proto(js, function_proto_obj, object_proto);
  set_slot(js, function_proto_obj, SLOT_CFUNC, js_mkfun(builtin_function_empty));
  js_setprop(js, function_proto_obj, ANT_STRING("call"), js_mkfun(builtin_function_call));
  js_setprop(js, function_proto_obj, ANT_STRING("apply"), js_mkfun(builtin_function_apply));
  js_setprop(js, function_proto_obj, ANT_STRING("bind"), js_mkfun(builtin_function_bind));
  js_setprop(js, function_proto_obj, ANT_STRING("toString"), js_mkfun(builtin_function_toString));
  jsval_t function_proto = js_obj_to_func(function_proto_obj);
  set_slot(js, glob, SLOT_FUNC_PROTO, function_proto);
  
  jsval_t array_proto = js_mkobj(js);
  set_proto(js, array_proto, object_proto);
  js_setprop(js, array_proto, js_mkstr(js, "push", 4), js_mkfun(builtin_array_push));
  js_setprop(js, array_proto, js_mkstr(js, "pop", 3), js_mkfun(builtin_array_pop));
  js_setprop(js, array_proto, js_mkstr(js, "slice", 5), js_mkfun(builtin_array_slice));
  js_setprop(js, array_proto, js_mkstr(js, "join", 4), js_mkfun(builtin_array_join));
  js_setprop(js, array_proto, js_mkstr(js, "includes", 8), js_mkfun(builtin_array_includes));
  js_setprop(js, array_proto, js_mkstr(js, "every", 5), js_mkfun(builtin_array_every));
  js_setprop(js, array_proto, js_mkstr(js, "reverse", 7), js_mkfun(builtin_array_reverse));
  js_setprop(js, array_proto, js_mkstr(js, "map", 3), js_mkfun(builtin_array_map));
  js_setprop(js, array_proto, js_mkstr(js, "filter", 6), js_mkfun(builtin_array_filter));
  js_setprop(js, array_proto, js_mkstr(js, "reduce", 6), js_mkfun(builtin_array_reduce));
  js_setprop(js, array_proto, js_mkstr(js, "flat", 4), js_mkfun(builtin_array_flat));
  js_setprop(js, array_proto, js_mkstr(js, "concat", 6), js_mkfun(builtin_array_concat));
  js_setprop(js, array_proto, js_mkstr(js, "at", 2), js_mkfun(builtin_array_at));
  js_setprop(js, array_proto, js_mkstr(js, "fill", 4), js_mkfun(builtin_array_fill));
  js_setprop(js, array_proto, js_mkstr(js, "find", 4), js_mkfun(builtin_array_find));
  js_setprop(js, array_proto, js_mkstr(js, "findIndex", 9), js_mkfun(builtin_array_findIndex));
  js_setprop(js, array_proto, js_mkstr(js, "findLast", 8), js_mkfun(builtin_array_findLast));
  js_setprop(js, array_proto, js_mkstr(js, "findLastIndex", 13), js_mkfun(builtin_array_findLastIndex));
  js_setprop(js, array_proto, js_mkstr(js, "flatMap", 7), js_mkfun(builtin_array_flatMap));
  js_setprop(js, array_proto, js_mkstr(js, "forEach", 7), js_mkfun(builtin_array_forEach));
  js_setprop(js, array_proto, js_mkstr(js, "indexOf", 7), js_mkfun(builtin_array_indexOf));
  js_setprop(js, array_proto, js_mkstr(js, "lastIndexOf", 11), js_mkfun(builtin_array_lastIndexOf));
  js_setprop(js, array_proto, js_mkstr(js, "reduceRight", 11), js_mkfun(builtin_array_reduceRight));
  js_setprop(js, array_proto, js_mkstr(js, "shift", 5), js_mkfun(builtin_array_shift));
  js_setprop(js, array_proto, js_mkstr(js, "unshift", 7), js_mkfun(builtin_array_unshift));
  js_setprop(js, array_proto, js_mkstr(js, "some", 4), js_mkfun(builtin_array_some));
  js_setprop(js, array_proto, js_mkstr(js, "sort", 4), js_mkfun(builtin_array_sort));
  js_setprop(js, array_proto, js_mkstr(js, "splice", 6), js_mkfun(builtin_array_splice));
  js_setprop(js, array_proto, js_mkstr(js, "copyWithin", 10), js_mkfun(builtin_array_copyWithin));
  js_setprop(js, array_proto, js_mkstr(js, "toReversed", 10), js_mkfun(builtin_array_toReversed));
  js_setprop(js, array_proto, js_mkstr(js, "toSorted", 8), js_mkfun(builtin_array_toSorted));
  js_setprop(js, array_proto, js_mkstr(js, "toSpliced", 9), js_mkfun(builtin_array_toSpliced));
  js_setprop(js, array_proto, js_mkstr(js, "with", 4), js_mkfun(builtin_array_with));
  js_setprop(js, array_proto, js_mkstr(js, "keys", 4), js_mkfun(builtin_array_keys));
  js_setprop(js, array_proto, js_mkstr(js, "values", 6), js_mkfun(builtin_array_values));
  js_setprop(js, array_proto, js_mkstr(js, "entries", 7), js_mkfun(builtin_array_entries));
  js_setprop(js, array_proto, js_mkstr(js, "toString", 8), js_mkfun(builtin_array_toString));
  js_setprop(js, array_proto, js_mkstr(js, "toLocaleString", 14), js_mkfun(builtin_array_toLocaleString));
  
  jsval_t string_proto = js_mkobj(js);
  set_proto(js, string_proto, object_proto);
  js_setprop(js, string_proto, js_mkstr(js, "indexOf", 7), js_mkfun(builtin_string_indexOf));
  js_setprop(js, string_proto, js_mkstr(js, "substring", 9), js_mkfun(builtin_string_substring));
  js_setprop(js, string_proto, js_mkstr(js, "substr", 6), js_mkfun(builtin_string_substr));
  js_setprop(js, string_proto, js_mkstr(js, "split", 5), js_mkfun(builtin_string_split));
  js_setprop(js, string_proto, js_mkstr(js, "slice", 5), js_mkfun(builtin_string_slice));
  js_setprop(js, string_proto, js_mkstr(js, "includes", 8), js_mkfun(builtin_string_includes));
  js_setprop(js, string_proto, js_mkstr(js, "startsWith", 10), js_mkfun(builtin_string_startsWith));
  js_setprop(js, string_proto, js_mkstr(js, "endsWith", 8), js_mkfun(builtin_string_endsWith));
  js_setprop(js, string_proto, js_mkstr(js, "replace", 7), js_mkfun(builtin_string_replace));
  js_setprop(js, string_proto, js_mkstr(js, "replaceAll", 10), js_mkfun(builtin_string_replaceAll));
  js_setprop(js, string_proto, js_mkstr(js, "template", 8), js_mkfun(builtin_string_template));
  js_setprop(js, string_proto, js_mkstr(js, "charCodeAt", 10), js_mkfun(builtin_string_charCodeAt));
  js_setprop(js, string_proto, js_mkstr(js, "codePointAt", 11), js_mkfun(builtin_string_codePointAt));
  js_setprop(js, string_proto, js_mkstr(js, "toLowerCase", 11), js_mkfun(builtin_string_toLowerCase));
  js_setprop(js, string_proto, js_mkstr(js, "toUpperCase", 11), js_mkfun(builtin_string_toUpperCase));
  js_setprop(js, string_proto, js_mkstr(js, "toLocaleLowerCase", 17), js_mkfun(builtin_string_toLowerCase));
  js_setprop(js, string_proto, js_mkstr(js, "toLocaleUpperCase", 17), js_mkfun(builtin_string_toUpperCase));
  js_setprop(js, string_proto, js_mkstr(js, "trim", 4), js_mkfun(builtin_string_trim));
  js_setprop(js, string_proto, js_mkstr(js, "trimStart", 9), js_mkfun(builtin_string_trimStart));
  js_setprop(js, string_proto, js_mkstr(js, "trimEnd", 7), js_mkfun(builtin_string_trimEnd));
  js_setprop(js, string_proto, js_mkstr(js, "repeat", 6), js_mkfun(builtin_string_repeat));
  js_setprop(js, string_proto, js_mkstr(js, "padStart", 8), js_mkfun(builtin_string_padStart));
  js_setprop(js, string_proto, js_mkstr(js, "padEnd", 6), js_mkfun(builtin_string_padEnd));
  js_setprop(js, string_proto, js_mkstr(js, "charAt", 6), js_mkfun(builtin_string_charAt));
  js_setprop(js, string_proto, js_mkstr(js, "at", 2), js_mkfun(builtin_string_at));
  js_setprop(js, string_proto, js_mkstr(js, "lastIndexOf", 11), js_mkfun(builtin_string_lastIndexOf));
  js_setprop(js, string_proto, js_mkstr(js, "concat", 6), js_mkfun(builtin_string_concat));
  js_setprop(js, string_proto, js_mkstr(js, "localeCompare", 13), js_mkfun(builtin_string_localeCompare));
  js_setprop(js, string_proto, js_mkstr(js, "normalize", 9), js_mkfun(builtin_string_normalize));
  js_setprop(js, string_proto, js_mkstr(js, "valueOf", 7), js_mkfun(builtin_string_valueOf));
  js_setprop(js, string_proto, js_mkstr(js, "toString", 8), js_mkfun(builtin_string_toString));

  jsval_t number_proto = js_mkobj(js);
  set_proto(js, number_proto, object_proto);
  js_setprop(js, number_proto, js_mkstr(js, "toString", 8), js_mkfun(builtin_number_toString));
  js_setprop(js, number_proto, js_mkstr(js, "toFixed", 7), js_mkfun(builtin_number_toFixed));
  js_setprop(js, number_proto, js_mkstr(js, "toPrecision", 11), js_mkfun(builtin_number_toPrecision));
  js_setprop(js, number_proto, js_mkstr(js, "toExponential", 13), js_mkfun(builtin_number_toExponential));
  js_setprop(js, number_proto, js_mkstr(js, "valueOf", 7), js_mkfun(builtin_number_valueOf));
  js_setprop(js, number_proto, js_mkstr(js, "toLocaleString", 14), js_mkfun(builtin_number_toLocaleString));
  
  jsval_t boolean_proto = js_mkobj(js);
  set_proto(js, boolean_proto, object_proto);
  js_setprop(js, boolean_proto, js_mkstr(js, "valueOf", 7), js_mkfun(builtin_boolean_valueOf));
  js_setprop(js, boolean_proto, js_mkstr(js, "toString", 8), js_mkfun(builtin_boolean_toString));
  
  jsval_t bigint_proto = js_mkobj(js);
  set_proto(js, bigint_proto, object_proto);
  js_setprop(js, bigint_proto, js_mkstr(js, "toString", 8), js_mkfun(builtin_bigint_toString));
  
  jsval_t error_proto = js_mkobj(js);
  set_proto(js, error_proto, object_proto);
  js_setprop(js, error_proto, ANT_STRING("name"), ANT_STRING("Error"));
  js_setprop(js, error_proto, ANT_STRING("message"), js_mkstr(js, "", 0));
  js_setprop(js, error_proto, js_mkstr(js, "toString", 8), js_mkfun(builtin_Error_toString));
  
  jsval_t err_ctor_obj = mkobj(js, 0);
  set_proto(js, err_ctor_obj, function_proto);
  set_slot(js, err_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_Error));
  js_setprop_nonconfigurable(js, err_ctor_obj, "prototype", 9, error_proto);
  js_setprop(js, err_ctor_obj, ANT_STRING("name"), ANT_STRING("Error"));
  jsval_t err_ctor_func = js_obj_to_func(err_ctor_obj);
  js_setprop(js, glob, ANT_STRING("Error"), err_ctor_func);
  js_setprop(js, error_proto, js_mkstr(js, "constructor", 11), err_ctor_func);
  js_set_descriptor(js, error_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  #define REGISTER_ERROR_SUBTYPE(name_str) do { \
    jsval_t proto = js_mkobj(js); \
    set_proto(js, proto, error_proto); \
    js_setprop(js, proto, ANT_STRING("name"), ANT_STRING(name_str)); \
    jsval_t ctor = mkobj(js, 0); \
    set_proto(js, ctor, function_proto); \
    set_slot(js, ctor, SLOT_CFUNC, js_mkfun(builtin_Error)); \
    js_setprop_nonconfigurable(js, ctor, "prototype", 9, proto); \
    js_setprop(js, ctor, ANT_STRING("name"), ANT_STRING(name_str)); \
    jsval_t ctor_func = js_obj_to_func(ctor); \
    js_setprop(js, proto, ANT_STRING("constructor"), ctor_func); \
    js_set_descriptor(js, proto, "constructor", 11, JS_DESC_W | JS_DESC_C); \
    js_setprop(js, glob, ANT_STRING(name_str), ctor_func); \
  } while(0)
  
  REGISTER_ERROR_SUBTYPE("EvalError");
  REGISTER_ERROR_SUBTYPE("RangeError");
  REGISTER_ERROR_SUBTYPE("ReferenceError");
  REGISTER_ERROR_SUBTYPE("SyntaxError");
  REGISTER_ERROR_SUBTYPE("TypeError");
  REGISTER_ERROR_SUBTYPE("URIError");
  REGISTER_ERROR_SUBTYPE("InternalError");
  
  #undef REGISTER_ERROR_SUBTYPE
  
  jsval_t proto = js_mkobj(js);
  set_proto(js, proto, error_proto);
  js_setprop(js, proto, ANT_STRING("name"), ANT_STRING("AggregateError"));
  jsval_t ctor = mkobj(js, 0);
  set_proto(js, ctor, function_proto);
  set_slot(js, ctor, SLOT_CFUNC, js_mkfun(builtin_AggregateError));
  js_setprop_nonconfigurable(js, ctor, "prototype", 9, proto);
  js_setprop(js, ctor, ANT_STRING("name"), ANT_STRING("AggregateError"));
  js_setprop(js, proto, ANT_STRING("constructor"), js_obj_to_func(ctor));
  js_set_descriptor(js, proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  js_setprop(js, glob, ANT_STRING("AggregateError"), js_obj_to_func(ctor));
  
  jsval_t promise_proto = js_mkobj(js);
  set_proto(js, promise_proto, object_proto);
  js_setprop(js, promise_proto, js_mkstr(js, "then", 4), js_mkfun(builtin_promise_then));
  js_setprop(js, promise_proto, js_mkstr(js, "catch", 5), js_mkfun(builtin_promise_catch));
  js_setprop(js, promise_proto, js_mkstr(js, "finally", 7), js_mkfun(builtin_promise_finally));
  // Symbol.toStringTag is set in init_symbol_module after symbols are initialized
  
  jsval_t obj_func_obj = mkobj(js, 0);
  set_proto(js, obj_func_obj, function_proto);
  set_slot(js, obj_func_obj, SLOT_BUILTIN, tov(BUILTIN_OBJECT));
  js_setprop(js, obj_func_obj, js_mkstr(js, "keys", 4), js_mkfun(builtin_object_keys));
  js_setprop(js, obj_func_obj, js_mkstr(js, "values", 6), js_mkfun(builtin_object_values));
  js_setprop(js, obj_func_obj, js_mkstr(js, "entries", 7), js_mkfun(builtin_object_entries));
  js_setprop(js, obj_func_obj, js_mkstr(js, "is", 2), js_mkfun(builtin_object_is));
  js_setprop(js, obj_func_obj, js_mkstr(js, "getPrototypeOf", 14), js_mkfun(builtin_object_getPrototypeOf));
  js_setprop(js, obj_func_obj, js_mkstr(js, "setPrototypeOf", 14), js_mkfun(builtin_object_setPrototypeOf));
  js_setprop(js, obj_func_obj, js_mkstr(js, "create", 6), js_mkfun(builtin_object_create));
  js_setprop(js, obj_func_obj, js_mkstr(js, "hasOwn", 6), js_mkfun(builtin_object_hasOwn));
  js_setprop(js, obj_func_obj, js_mkstr(js, "groupBy", 7), js_mkfun(builtin_object_groupBy));
  js_setprop(js, obj_func_obj, js_mkstr(js, "defineProperty", 14), js_mkfun(builtin_object_defineProperty));
  js_setprop(js, obj_func_obj, js_mkstr(js, "defineProperties", 16), js_mkfun(builtin_object_defineProperties));
  js_setprop(js, obj_func_obj, js_mkstr(js, "assign", 6), js_mkfun(builtin_object_assign));
  js_setprop(js, obj_func_obj, js_mkstr(js, "freeze", 6), js_mkfun(builtin_object_freeze));
  js_setprop(js, obj_func_obj, js_mkstr(js, "isFrozen", 8), js_mkfun(builtin_object_isFrozen));
  js_setprop(js, obj_func_obj, js_mkstr(js, "seal", 4), js_mkfun(builtin_object_seal));
  js_setprop(js, obj_func_obj, js_mkstr(js, "isSealed", 8), js_mkfun(builtin_object_isSealed));
  js_setprop(js, obj_func_obj, js_mkstr(js, "fromEntries", 11), js_mkfun(builtin_object_fromEntries));
  js_setprop(js, obj_func_obj, js_mkstr(js, "getOwnPropertyDescriptor", 24), js_mkfun(builtin_object_getOwnPropertyDescriptor));
  js_setprop(js, obj_func_obj, js_mkstr(js, "getOwnPropertyNames", 19), js_mkfun(builtin_object_getOwnPropertyNames));
  js_setprop(js, obj_func_obj, js_mkstr(js, "getOwnPropertySymbols", 21), js_mkfun(builtin_object_getOwnPropertySymbols));
  js_setprop(js, obj_func_obj, js_mkstr(js, "isExtensible", 12), js_mkfun(builtin_object_isExtensible));
  js_setprop(js, obj_func_obj, js_mkstr(js, "preventExtensions", 17), js_mkfun(builtin_object_preventExtensions));
  js_setprop(js, obj_func_obj, ANT_STRING("name"), ANT_STRING("Object"));
  js_setprop_nonconfigurable(js, obj_func_obj, "prototype", 9, object_proto);
  jsval_t obj_func = js_obj_to_func(obj_func_obj);
  js_setprop(js, glob, js_mkstr(js, "Object", 6), obj_func);
  
  jsval_t func_ctor_obj = mkobj(js, 0);
  set_proto(js, func_ctor_obj, function_proto);
  set_slot(js, func_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_Function));
  js_setprop_nonconfigurable(js, func_ctor_obj, "prototype", 9, function_proto);
  js_setprop(js, func_ctor_obj, js->length_str, tov(1.0));
  js_set_descriptor(js, func_ctor_obj, "length", 6, JS_DESC_C);
  js_setprop(js, func_ctor_obj, ANT_STRING("name"), ANT_STRING("Function"));
  jsval_t func_ctor_func = js_obj_to_func(func_ctor_obj);
  js_setprop(js, glob, js_mkstr(js, "Function", 8), func_ctor_func);
  
  jsval_t async_func_proto_obj = js_mkobj(js);
  set_proto(js, async_func_proto_obj, function_proto);
  set_slot(js, async_func_proto_obj, SLOT_ASYNC, js_true);
  jsval_t async_func_proto = js_obj_to_func(async_func_proto_obj);
  set_slot(js, glob, SLOT_ASYNC_PROTO, async_func_proto);
  
  jsval_t async_func_ctor_obj = mkobj(js, 0);
  set_proto(js, async_func_ctor_obj, function_proto);
  set_slot(js, async_func_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_AsyncFunction));
  js_setprop_nonconfigurable(js, async_func_ctor_obj, "prototype", 9, async_func_proto);
  js_setprop(js, async_func_ctor_obj, js->length_str, tov(1.0));
  js_set_descriptor(js, async_func_ctor_obj, "length", 6, JS_DESC_C);
  js_setprop(js, async_func_ctor_obj, ANT_STRING("name"), ANT_STRING("AsyncFunction"));
  jsval_t async_func_ctor = js_obj_to_func(async_func_ctor_obj);
  
  js_setprop(js, async_func_proto_obj, js_mkstr(js, "constructor", 11), async_func_ctor);
  js_set_descriptor(js, async_func_proto_obj, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  jsval_t str_ctor_obj = mkobj(js, 0);
  set_proto(js, str_ctor_obj, function_proto);
  set_slot(js, str_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_String));
  js_setprop_nonconfigurable(js, str_ctor_obj, "prototype", 9, string_proto);
  js_setprop(js, str_ctor_obj, js_mkstr(js, "fromCharCode", 12), js_mkfun(builtin_string_fromCharCode));
  js_setprop(js, str_ctor_obj, js_mkstr(js, "fromCodePoint", 13), js_mkfun(builtin_string_fromCodePoint));
  js_setprop(js, str_ctor_obj, js_mkstr(js, "raw", 3), js_mkfun(builtin_string_raw));
  js_setprop(js, str_ctor_obj, ANT_STRING("name"), ANT_STRING("String"));
  jsval_t str_ctor_func = js_obj_to_func(str_ctor_obj);
  js_setprop(js, glob, js_mkstr(js, "String", 6), str_ctor_func);
  
  jsval_t number_ctor_obj = mkobj(js, 0);
  set_proto(js, number_ctor_obj, function_proto);
  
  set_slot(js, number_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_Number));
  js_setprop(js, number_ctor_obj, js_mkstr(js, "isNaN", 5), js_mkfun(builtin_Number_isNaN));
  js_setprop(js, number_ctor_obj, js_mkstr(js, "isFinite", 8), js_mkfun(builtin_Number_isFinite));
  js_setprop(js, number_ctor_obj, js_mkstr(js, "isInteger", 9), js_mkfun(builtin_Number_isInteger));
  js_setprop(js, number_ctor_obj, js_mkstr(js, "isSafeInteger", 13), js_mkfun(builtin_Number_isSafeInteger));
  js_setprop(js, number_ctor_obj, js_mkstr(js, "parseInt", 8), js_mkfun(builtin_parseInt));
  js_setprop(js, number_ctor_obj, js_mkstr(js, "parseFloat", 10), js_mkfun(builtin_parseFloat));
  
  js_setprop(js, number_ctor_obj, js_mkstr(js, "MAX_VALUE", 9), tov(1.7976931348623157e+308));
  js_setprop(js, number_ctor_obj, js_mkstr(js, "MIN_VALUE", 9), tov(5e-324));
  js_setprop(js, number_ctor_obj, js_mkstr(js, "MAX_SAFE_INTEGER", 16), tov(9007199254740991.0));
  js_setprop(js, number_ctor_obj, js_mkstr(js, "MIN_SAFE_INTEGER", 16), tov(-9007199254740991.0));
  js_setprop(js, number_ctor_obj, js_mkstr(js, "POSITIVE_INFINITY", 17), tov(JS_INF));
  js_setprop(js, number_ctor_obj, js_mkstr(js, "NEGATIVE_INFINITY", 17), tov(JS_NEG_INF));
  js_setprop(js, number_ctor_obj, js_mkstr(js, "NaN", 3), tov(JS_NAN));
  js_setprop(js, number_ctor_obj, js_mkstr(js, "EPSILON", 7), tov(2.220446049250313e-16));
  
  js_setprop_nonconfigurable(js, number_ctor_obj, "prototype", 9, number_proto);
  js_setprop(js, number_ctor_obj, ANT_STRING("name"), ANT_STRING("Number"));
  jsval_t number_ctor_func = js_obj_to_func(number_ctor_obj);
  js_setprop(js, glob, js_mkstr(js, "Number", 6), number_ctor_func);
  
  jsval_t bool_ctor_obj = mkobj(js, 0);
  set_proto(js, bool_ctor_obj, function_proto);
  set_slot(js, bool_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_Boolean));
  js_setprop_nonconfigurable(js, bool_ctor_obj, "prototype", 9, boolean_proto);
  js_setprop(js, bool_ctor_obj, ANT_STRING("name"), ANT_STRING("Boolean"));
  jsval_t bool_ctor_func = js_obj_to_func(bool_ctor_obj);
  js_setprop(js, glob, js_mkstr(js, "Boolean", 7), bool_ctor_func);
  
  jsval_t arr_ctor_obj = mkobj(js, 0);
  set_proto(js, arr_ctor_obj, function_proto);
  set_slot(js, arr_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_Array));
  js_setprop_nonconfigurable(js, arr_ctor_obj, "prototype", 9, array_proto);
  js_setprop(js, arr_ctor_obj, js_mkstr(js, "isArray", 7), js_mkfun(builtin_Array_isArray));
  js_setprop(js, arr_ctor_obj, js_mkstr(js, "from", 4), js_mkfun(builtin_Array_from));
  js_setprop(js, arr_ctor_obj, js_mkstr(js, "of", 2), js_mkfun(builtin_Array_of));
  js_setprop(js, arr_ctor_obj, js->length_str, tov(1.0));
  js_set_descriptor(js, arr_ctor_obj, "length", 6, JS_DESC_C);
  js_setprop(js, arr_ctor_obj, ANT_STRING("name"), ANT_STRING("Array"));
  jsval_t arr_ctor_func = js_obj_to_func(arr_ctor_obj);
  js_setprop(js, glob, js_mkstr(js, "Array", 5), arr_ctor_func);
  
  jsval_t proxy_ctor_obj = mkobj(js, 0);
  set_proto(js, proxy_ctor_obj, function_proto);
  set_slot(js, proxy_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_Proxy));
  js_setprop(js, proxy_ctor_obj, js_mkstr(js, "revocable", 9), js_mkfun(builtin_Proxy_revocable));
  js_setprop(js, proxy_ctor_obj, ANT_STRING("name"), ANT_STRING("Proxy"));
  js_setprop(js, glob, js_mkstr(js, "Proxy", 5), js_obj_to_func(proxy_ctor_obj));
  
  jsval_t p_ctor_obj = mkobj(js, 0);
  set_proto(js, p_ctor_obj, function_proto);
  set_slot(js, p_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_Promise));
  js_setprop(js, p_ctor_obj, js_mkstr(js, "resolve", 7), js_mkfun(builtin_Promise_resolve));
  js_setprop(js, p_ctor_obj, js_mkstr(js, "reject", 6), js_mkfun(builtin_Promise_reject));
  js_setprop(js, p_ctor_obj, js_mkstr(js, "try", 3), js_mkfun(builtin_Promise_try));
  js_setprop(js, p_ctor_obj, js_mkstr(js, "withResolvers", 13), js_mkfun(builtin_Promise_withResolvers));
  js_setprop(js, p_ctor_obj, js_mkstr(js, "all", 3), js_mkfun(builtin_Promise_all));
  js_setprop(js, p_ctor_obj, js_mkstr(js, "race", 4), js_mkfun(builtin_Promise_race));
  js_setprop(js, p_ctor_obj, js_mkstr(js, "any", 3), js_mkfun(builtin_Promise_any));
  js_setprop_nonconfigurable(js, p_ctor_obj, "prototype", 9, promise_proto);
  js_setprop(js, p_ctor_obj, ANT_STRING("name"), ANT_STRING("Promise"));
  js_setprop(js, glob, js_mkstr(js, "Promise", 7), js_obj_to_func(p_ctor_obj));
  
  jsval_t bigint_ctor_obj = mkobj(js, 0);
  set_proto(js, bigint_ctor_obj, function_proto);
  set_slot(js, bigint_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_BigInt));
  js_setprop(js, bigint_ctor_obj, js_mkstr(js, "asIntN", 6), js_mkfun(builtin_BigInt_asIntN));
  js_setprop(js, bigint_ctor_obj, js_mkstr(js, "asUintN", 7), js_mkfun(builtin_BigInt_asUintN));
  js_setprop_nonconfigurable(js, bigint_ctor_obj, "prototype", 9, bigint_proto);
  js_setprop(js, bigint_ctor_obj, ANT_STRING("name"), ANT_STRING("BigInt"));
  js_setprop(js, glob, js_mkstr(js, "BigInt", 6), js_obj_to_func(bigint_ctor_obj));
  
  js_setprop(js, glob, js_mkstr(js, "parseInt", 8), js_mkfun(builtin_parseInt));
  js_setprop(js, glob, js_mkstr(js, "parseFloat", 10), js_mkfun(builtin_parseFloat));
  js_setprop(js, glob, js_mkstr(js, "eval", 4), js_mkfun(builtin_eval));
  js_setprop(js, glob, js_mkstr(js, "isNaN", 5), js_mkfun(builtin_global_isNaN));
  js_setprop(js, glob, js_mkstr(js, "isFinite", 8), js_mkfun(builtin_global_isFinite));
  js_setprop(js, glob, js_mkstr(js, "btoa", 4), js_mkfun(builtin_btoa));
  js_setprop(js, glob, js_mkstr(js, "atob", 4), js_mkfun(builtin_atob));
  js_setprop(js, glob, js_mkstr(js, "NaN", 3), tov(JS_NAN));
  js_set_descriptor(js, glob, "NaN", 3, 0);
  js_setprop(js, glob, js_mkstr(js, "Infinity", 8), tov(JS_INF));
  js_set_descriptor(js, glob, "Infinity", 8, 0);
  js_setprop(js, glob, js_mkstr(js, "undefined", 9), js_mkundef());
  js_set_descriptor(js, glob, "undefined", 9, 0);
  
  jsval_t math_obj = mkobj(js, 0);
  set_proto(js, math_obj, object_proto);
  js_setprop(js, math_obj, js_mkstr(js, "E", 1), tov(M_E));
  js_setprop(js, math_obj, js_mkstr(js, "LN10", 4), tov(M_LN10));
  js_setprop(js, math_obj, js_mkstr(js, "LN2", 3), tov(M_LN2));
  js_setprop(js, math_obj, js_mkstr(js, "LOG10E", 6), tov(M_LOG10E));
  js_setprop(js, math_obj, js_mkstr(js, "LOG2E", 5), tov(M_LOG2E));
  js_setprop(js, math_obj, js_mkstr(js, "PI", 2), tov(M_PI));
  js_setprop(js, math_obj, js_mkstr(js, "SQRT1_2", 7), tov(M_SQRT1_2));
  js_setprop(js, math_obj, js_mkstr(js, "SQRT2", 5), tov(M_SQRT2));
  js_setprop(js, math_obj, js_mkstr(js, "abs", 3), js_mkfun(builtin_Math_abs));
  js_setprop(js, math_obj, js_mkstr(js, "acos", 4), js_mkfun(builtin_Math_acos));
  js_setprop(js, math_obj, js_mkstr(js, "acosh", 5), js_mkfun(builtin_Math_acosh));
  js_setprop(js, math_obj, js_mkstr(js, "asin", 4), js_mkfun(builtin_Math_asin));
  js_setprop(js, math_obj, js_mkstr(js, "asinh", 5), js_mkfun(builtin_Math_asinh));
  js_setprop(js, math_obj, js_mkstr(js, "atan", 4), js_mkfun(builtin_Math_atan));
  js_setprop(js, math_obj, js_mkstr(js, "atanh", 5), js_mkfun(builtin_Math_atanh));
  js_setprop(js, math_obj, js_mkstr(js, "atan2", 5), js_mkfun(builtin_Math_atan2));
  js_setprop(js, math_obj, js_mkstr(js, "cbrt", 4), js_mkfun(builtin_Math_cbrt));
  js_setprop(js, math_obj, js_mkstr(js, "ceil", 4), js_mkfun(builtin_Math_ceil));
  js_setprop(js, math_obj, js_mkstr(js, "clz32", 5), js_mkfun(builtin_Math_clz32));
  js_setprop(js, math_obj, js_mkstr(js, "cos", 3), js_mkfun(builtin_Math_cos));
  js_setprop(js, math_obj, js_mkstr(js, "cosh", 4), js_mkfun(builtin_Math_cosh));
  js_setprop(js, math_obj, js_mkstr(js, "exp", 3), js_mkfun(builtin_Math_exp));
  js_setprop(js, math_obj, js_mkstr(js, "expm1", 5), js_mkfun(builtin_Math_expm1));
  js_setprop(js, math_obj, js_mkstr(js, "floor", 5), js_mkfun(builtin_Math_floor));
  js_setprop(js, math_obj, js_mkstr(js, "fround", 6), js_mkfun(builtin_Math_fround));
  js_setprop(js, math_obj, js_mkstr(js, "hypot", 5), js_mkfun(builtin_Math_hypot));
  js_setprop(js, math_obj, js_mkstr(js, "imul", 4), js_mkfun(builtin_Math_imul));
  js_setprop(js, math_obj, js_mkstr(js, "log", 3), js_mkfun(builtin_Math_log));
  js_setprop(js, math_obj, js_mkstr(js, "log1p", 5), js_mkfun(builtin_Math_log1p));
  js_setprop(js, math_obj, js_mkstr(js, "log10", 5), js_mkfun(builtin_Math_log10));
  js_setprop(js, math_obj, js_mkstr(js, "log2", 4), js_mkfun(builtin_Math_log2));
  js_setprop(js, math_obj, js_mkstr(js, "max", 3), js_mkfun(builtin_Math_max));
  js_setprop(js, math_obj, js_mkstr(js, "min", 3), js_mkfun(builtin_Math_min));
  js_setprop(js, math_obj, js_mkstr(js, "pow", 3), js_mkfun(builtin_Math_pow));
  js_setprop(js, math_obj, js_mkstr(js, "random", 6), js_mkfun(builtin_Math_random));
  js_setprop(js, math_obj, js_mkstr(js, "round", 5), js_mkfun(builtin_Math_round));
  js_setprop(js, math_obj, js_mkstr(js, "sign", 4), js_mkfun(builtin_Math_sign));
  js_setprop(js, math_obj, js_mkstr(js, "sin", 3), js_mkfun(builtin_Math_sin));
  js_setprop(js, math_obj, js_mkstr(js, "sinh", 4), js_mkfun(builtin_Math_sinh));
  js_setprop(js, math_obj, js_mkstr(js, "sqrt", 4), js_mkfun(builtin_Math_sqrt));
  js_setprop(js, math_obj, js_mkstr(js, "tan", 3), js_mkfun(builtin_Math_tan));
  js_setprop(js, math_obj, js_mkstr(js, "tanh", 4), js_mkfun(builtin_Math_tanh));
  js_setprop(js, math_obj, js_mkstr(js, "trunc", 5), js_mkfun(builtin_Math_trunc));
  js_setprop(js, glob, js_mkstr(js, "Math", 4), math_obj);
  
  jsval_t import_obj = mkobj(js, 0);
  set_proto(js, import_obj, function_proto);
  
  set_slot(js, import_obj, SLOT_CFUNC, js_mkfun(builtin_import));
  js_setprop(js, glob, js_mkstr(js, "import", 6), js_obj_to_func(import_obj));
  
  js->module_ns = js_mkundef();
  js->import_meta = js_mkundef();
  
  js_setprop(js, object_proto, js_mkstr(js, "constructor", 11), obj_func);
  js_set_descriptor(js, object_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  js_setprop(js, function_proto, js_mkstr(js, "constructor", 11), func_ctor_func);
  js_set_descriptor(js, function_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  js_setprop(js, array_proto, js_mkstr(js, "constructor", 11), arr_ctor_func);
  js_set_descriptor(js, array_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  js_setprop(js, string_proto, js_mkstr(js, "constructor", 11), str_ctor_func);
  js_set_descriptor(js, string_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  js_setprop(js, number_proto, js_mkstr(js, "constructor", 11), number_ctor_func);
  js_set_descriptor(js, number_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  js_setprop(js, boolean_proto, js_mkstr(js, "constructor", 11), bool_ctor_func);
  js_set_descriptor(js, boolean_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  set_proto(js, glob, object_proto);
  
  js->object = object_proto;
  js->owns_mem = false;
  js->max_size = 0;
  
  return js;
}

ant_t *js_create_dynamic() {
  void *init_buf = ant_calloc(ANT_ARENA_MIN);
  if (init_buf == NULL) return NULL;
  
  ant_t *js = js_create(init_buf, ANT_ARENA_MIN);
  if (js == NULL) { free(init_buf); return NULL; }
  
  uint8_t *arena = (uint8_t *)ant_arena_reserve(ANT_ARENA_MAX);
  if (arena == NULL) { free(init_buf); return NULL; }
  
  if (ant_arena_commit(arena, 0, js->size) != 0) {
    ant_arena_free(arena, ANT_ARENA_MAX);
    free(init_buf); return NULL;
  }
  
  memcpy(arena, js->mem, js->brk);
  js->mem = arena;
  
  js->owns_mem = true;
  js->max_size = (jsoff_t) ANT_ARENA_MAX;
  
  ant_t *new_js = (ant_t *)malloc(sizeof(ant_t));
  if (new_js == NULL) {
    ant_arena_free(arena, ANT_ARENA_MAX);
    free(init_buf);
    return NULL;
  }
  
  memcpy(new_js, js, sizeof(ant_t));
  free(init_buf);

  new_js->vm = sv_vm_create(new_js, SV_VM_MAIN);

  return new_js;
}

void js_destroy(ant_t *js) {
  if (js == NULL) return;
  
  if (js->vm) {
    sv_vm_destroy(js->vm);
    js->vm = NULL;
  }
  
  js_esm_cleanup_module_cache();
  code_arena_reset();
  cleanup_buffer_module();
  cleanup_collections_module();
  cleanup_lmdb_module();
  
  if (js->owns_mem) {
    ant_arena_free(js->mem, js->max_size);
    free(js);
  }
  
  destroy_runtime(js);
}

inline double js_getnum(jsval_t value) { return tod(value); }
inline void js_setstackbase(ant_t *js, void *base) { js->cstk.base = base; js->cstk.main_base = base; }
inline void js_setstacklimit(ant_t *js, size_t max) { js->cstk.limit = max; }
inline void js_set_filename(ant_t *js, const char *filename) { js->filename = filename; }

inline jsval_t js_mkundef(void) { return mkval(T_UNDEF, 0); }
inline jsval_t js_mknull(void) { return mkval(T_NULL, 0); }
inline jsval_t js_mknum(double value) { return tov(value); }
inline jsval_t js_mkobj(ant_t *js) { return mkobj(js, 0); }
inline jsval_t js_glob(ant_t *js) { return js->global; }
inline jsval_t js_mkfun(jsval_t (*fn)(ant_t *, jsval_t *, int)) { return mkval(T_CFUNC, (size_t) (void *) fn); }

inline jsval_t js_getthis(ant_t *js) { return js->this_val; }
inline void js_setthis(ant_t *js, jsval_t val) { js->this_val = val; }
inline jsval_t js_getcurrentfunc(ant_t *js) { return js->current_func; }

jsval_t js_heavy_mkfun(ant_t *js, jsval_t (*fn)(ant_t *, jsval_t *, int), jsval_t data) {
  jsval_t cfunc = js_mkfun(fn);
  jsval_t fn_obj = mkobj(js, 0);
  
  set_slot(js, fn_obj, SLOT_CFUNC, cfunc);
  set_slot(js, fn_obj, SLOT_DATA, data);
  
  return js_obj_to_func(fn_obj);
}

void js_set(ant_t *js, jsval_t obj, const char *key, jsval_t val) {
  size_t key_len = strlen(key);
  
  if (vtype(obj) == T_OBJ) {
    jsoff_t existing = lkp(js, obj, key, key_len);
    if (existing > 0) {
      if (is_const_prop(js, existing)) {
        js_mkerr(js, "assignment to constant");
        return;
      }
      saveval(js, existing + sizeof(jsoff_t) * 2, val);
    } else {
      jsval_t key_str = js_mkstr(js, key, key_len);
      mkprop(js, obj, key_str, val, 0);
    }
  } else if (vtype(obj) == T_FUNC) {
    jsval_t func_obj = js_func_obj(obj);
    jsoff_t existing = lkp(js, func_obj, key, key_len);
    if (existing > 0) {
      if (is_const_prop(js, existing)) {
        js_mkerr(js, "assignment to constant");
        return;
      }
      saveval(js, existing + sizeof(jsoff_t) * 2, val);
    } else {
      jsval_t key_str = js_mkstr(js, key, key_len);
      mkprop(js, func_obj, key_str, val, 0);
    }
  }
}

void js_set_sym(ant_t *js, jsval_t obj, jsval_t sym, jsval_t val) {
  if (vtype(sym) != T_SYMBOL) return;
  jsoff_t sym_off = (jsoff_t)vdata(sym);
  
  if (vtype(obj) == T_FUNC) obj = js_func_obj(obj);
  if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR) return;
  
  jsoff_t existing = lkp_sym(js, obj, sym_off);
  if (existing > 0) {
    if (is_const_prop(js, existing)) return;
    saveval(js, existing + sizeof(jsoff_t) * 2, val);
  } else mkprop(js, obj, sym, val, 0);
}

jsval_t js_get_sym(ant_t *js, jsval_t obj, jsval_t sym) {
  if (vtype(sym) != T_SYMBOL) return js_mkundef();
  jsoff_t sym_off = (jsoff_t)vdata(sym);
  
  jsval_t receiver = obj;
  if (vtype(obj) == T_FUNC) obj = js_func_obj(obj);
  uint8_t ot = vtype(obj);
  if (!is_object_type(obj)) {
    if (ot == T_STR || ot == T_NUM || ot == T_BOOL || ot == T_BIGINT || ot == T_SYMBOL) {
      jsval_t proto = get_prototype_for_type(js, ot);
      if (!is_object_type(proto)) return js_mkundef();
      obj = js_as_obj(proto);
    } else {
      return js_mkundef();
    }
  } else {
    obj = js_as_obj(obj);
  }
  
  if (is_proxy(js, obj)) return proxy_get_val(js, obj, sym);
  
  jsval_t cur = obj;
  for (int i = 0; i < MAX_PROTO_CHAIN_DEPTH; i++) {
    jsoff_t cur_off = (jsoff_t)vdata(cur);
    descriptor_entry_t *sd = lookup_sym_descriptor(cur_off, sym_off);
    if (sd && sd->has_getter) {
      jsval_t g = sd->getter;
      if (vtype(g) == T_FUNC || vtype(g) == T_CFUNC)
        return sv_vm_call(js->vm, js, g, receiver, NULL, 0, NULL, false);
      return js_mkundef();
    }
    jsval_t proto = get_proto(js, cur);
    if (!is_object_type(proto)) break;
    cur = js_as_obj(proto);
  }
  
  jsoff_t off = lkp_sym_proto(js, obj, sym_off);
  if (off == 0) return js_mkundef();
  return resolveprop(js, mkval(T_PROP, off));
}

bool js_del(ant_t *js, jsval_t obj, const char *key) {
  size_t len = strlen(key);
  jsoff_t obj_off;
  
  if (vtype(obj) == T_OBJ) {
    obj_off = (jsoff_t)vdata(obj);
  } else if (vtype(obj) == T_ARR || vtype(obj) == T_FUNC) {
    obj_off = (jsoff_t)vdata(js_as_obj(obj));
    if (vtype(obj) == T_ARR) {
      unsigned long del_idx;
      jsoff_t arr_len = get_array_length(js, obj);
      if (parse_array_index(key, len, arr_len, &del_idx)) {
        arr_del(js, obj, (jsoff_t)del_idx);
        return true;
      }
    }
    obj = mkval(T_OBJ, obj_off);
  } else {
    return false;
  }
  
  jsoff_t prop_off = lkp(js, obj, key, len);
  if (prop_off == 0) return true;
  if (is_nonconfig_prop(js, prop_off)) return false;
  
  descriptor_entry_t *desc = lookup_descriptor(js, obj_off, key, len);
  if (desc && !desc->configurable) return false;
  
  jsoff_t first_prop = loadoff(js, obj_off) & ~(3U | FLAGMASK);
  jsoff_t tail = loadoff(js, obj_off + sizeof(jsoff_t) + sizeof(jsoff_t));
  
  if (first_prop == prop_off) {
    jsoff_t deleted_next = loadoff(js, prop_off) & ~FLAGMASK;
    jsoff_t current = loadoff(js, obj_off);
    
    saveoff(js, obj_off, (deleted_next & ~3ULL) | (current & (FLAGMASK | 3ULL)));
    if (tail == prop_off) saveoff(js, obj_off + sizeof(jsoff_t) + sizeof(jsoff_t), 0);
    
    invalidate_prop_cache(js, obj_off, prop_off);
    js->needs_gc = true;
    
    return true;
  }
  
  jsoff_t prev = first_prop;
  while (prev != 0 && prev < js->brk) {
    jsoff_t next = loadoff(js, prev) & ~(3U | FLAGMASK);
    if (next == prop_off) {
      jsoff_t deleted_next = loadoff(js, prop_off) & ~(3U | FLAGMASK);
      jsoff_t prev_flags = loadoff(js, prev) & FLAGMASK;
      
      saveoff(js, prev, deleted_next | prev_flags);
      if (tail == prop_off) saveoff(js, obj_off + sizeof(jsoff_t) + sizeof(jsoff_t), prev);
      
      invalidate_prop_cache(js, obj_off, prop_off);
      js->needs_gc = true;
      
      return true;
    }
    prev = next;
  }
  
  return false;
}

static bool js_try_get(ant_t *js, jsval_t obj, const char *key, jsval_t *out) {
  size_t key_len = strlen(key);
  
  if (vtype(obj) == T_FUNC) {
    if (sv_vm_is_strict(js->vm) &&
        ((key_len == 6 && memcmp(key, "caller", 6) == 0) ||
         (key_len == 9 && memcmp(key, "arguments", 9) == 0))) {
      *out = js_mkerr_typed(js, JS_ERR_TYPE,
                            "'%.*s' not allowed on functions in strict mode",
                            (int)key_len, key);
      return true;
    }

    jsval_t func_obj = js_func_obj(obj);
    if (key_len == 4 && memcmp(key, "meta", 4) == 0 && vtype(js->import_meta) != T_UNDEF) {
      jsval_t cfunc = js_get_slot(js, func_obj, SLOT_CFUNC);
      if (vtype(cfunc) == T_CFUNC && js_as_cfunc(cfunc) == builtin_import) {
        *out = js->import_meta;
        return true;
      }
    }
    jsoff_t off = lkp(js, func_obj, key, key_len);
    if (off == 0) {
      jsval_t accessor_result;
      if (try_accessor_getter(js, obj, key, key_len, &accessor_result)) {
        *out = accessor_result;
        return true;
      }
      return false;
    }

    descriptor_entry_t *desc = lookup_descriptor(js, (jsoff_t)vdata(func_obj), key, key_len);
    if (desc && desc->has_getter) {
      jsval_t accessor_result;
      if (try_accessor_getter(js, obj, key, key_len, &accessor_result)) {
        *out = accessor_result;
        return true;
      }
    }

    *out = resolveprop(js, mkval(T_PROP, off));
    return true;
  }
  
  if (vtype(obj) == T_ARR) {
    if (((key_len == 6 && memcmp(key, "callee", 6) == 0) ||
         (key_len == 6 && memcmp(key, "caller", 6) == 0)) &&
        vtype(get_slot(js, obj, SLOT_STRICT_ARGS)) != T_UNDEF) {
      *out = js_mkerr_typed(js, JS_ERR_TYPE,
                            "'%.*s' not allowed on strict arguments",
                            (int)key_len, key);
      return true;
    }

    if (key_len == 6 && memcmp(key, "length", 6) == 0) {
      *out = tov((double)get_array_length(js, obj));
      return true;
    }
    unsigned long idx;
    jsoff_t arr_len = get_array_length(js, obj);
    if (parse_array_index(key, key_len, arr_len, &idx)) {
      if (arr_has(js, obj, (jsoff_t)idx)) {
        *out = arr_get(js, obj, (jsoff_t)idx);
        return true;
      } return false;
    }
    
    jsval_t arr_obj = js_as_obj(obj);
    jsoff_t off = lkp(js, arr_obj, key, key_len);
    if (off == 0) {
      jsval_t accessor_result;
      if (try_accessor_getter(js, arr_obj, key, key_len, &accessor_result)) {
        *out = accessor_result; return true;
      } return false;
    }
    
    descriptor_entry_t *desc = lookup_descriptor(js, (jsoff_t)vdata(arr_obj), key, key_len);
    if (desc && desc->has_getter) {
      jsval_t accessor_result;
      if (try_accessor_getter(js, arr_obj, key, key_len, &accessor_result)) {
        *out = accessor_result; return true;
      }
    }
    
    *out = resolveprop(js, mkval(T_PROP, off));
    return true;
  }

  uint8_t t = vtype(obj);
  bool is_promise = (t == T_PROMISE);
  if (t == T_OBJ && is_proxy(js, obj)) {
    *out = proxy_get(js, obj, key, key_len);
    return true;
  }
  if (is_promise) obj = js_as_obj(obj);
  else if (t != T_OBJ) return false;
  jsoff_t off = lkp(js, obj, key, key_len);
  
  if (off == 0) {
    jsval_t result = try_dynamic_getter(js, obj, key, key_len);
    if (vtype(result) != T_UNDEF) { *out = result; return true; }
  }
  
  if (off == 0 && is_promise) {
    jsval_t promise_proto = get_ctor_proto(js, "Promise", 7);
    if (vtype(promise_proto) != T_UNDEF && vtype(promise_proto) != T_NULL) {
      off = lkp(js, promise_proto, key, key_len);
      if (off != 0) { *out = resolveprop(js, mkval(T_PROP, off)); return true; }
    }
  }
  
  if (off == 0) {
    jsval_t accessor_result;
    if (try_accessor_getter(js, obj, key, key_len, &accessor_result)) {
      *out = accessor_result; return true;
    }
    return false;
  }
  
  descriptor_entry_t *desc = lookup_descriptor(js, (jsoff_t)vdata(js_as_obj(obj)), key, key_len);
  if (desc && desc->has_getter) {
    jsval_t accessor_result;
    if (try_accessor_getter(js, obj, key, key_len, &accessor_result)) {
      *out = accessor_result; return true;
    }
  }
  
  *out = resolveprop(js, mkval(T_PROP, off));
  return true;
}

jsval_t js_get(ant_t *js, jsval_t obj, const char *key) {
  jsval_t val;
  if (js_try_get(js, obj, key, &val)) return val;
  return js_mkundef();
}

jsval_t js_getprop_proto(ant_t *js, jsval_t obj, const char *key) {
  size_t key_len = strlen(key);
  jsoff_t off = lkp_proto(js, obj, key, key_len);
  return off == 0 ? js_mkundef() : resolveprop(js, mkval(T_PROP, off));
}

jsval_t js_getprop_fallback(ant_t *js, jsval_t obj, const char *name) {
  jsval_t val;
  if (js_try_get(js, obj, name, &val)) return val;
  return js_getprop_proto(js, obj, name);
}

jsval_t js_getprop_super(ant_t *js, jsval_t super_obj, jsval_t receiver, const char *name) {
  if (!name) return js_mkundef();

  if (vtype(super_obj) == T_FUNC) super_obj = js_func_obj(super_obj);
  if (!is_object_type(super_obj)) return js_mkundef();

  size_t key_len = strlen(name);
  if (is_proxy(js, super_obj)) return proxy_get(js, super_obj, name, key_len);

  const char *key_intern = intern_string(name, key_len);
  if (!key_intern) return js_mkundef();

  jsval_t cur = js_as_obj(super_obj);
  for (int i = 0; i < MAX_PROTO_CHAIN_DEPTH; i++) {
    jsoff_t cur_off = (jsoff_t)vdata(cur);
    descriptor_entry_t *desc = lookup_descriptor(js, cur_off, name, key_len);
    if (desc) {
      if (desc->has_getter) {
        jsval_t getter = desc->getter;
        if (vtype(getter) == T_FUNC || vtype(getter) == T_CFUNC)
          return sv_vm_call(js->vm, js, getter, receiver, NULL, 0, NULL, false);
        return js_mkundef();
      }
      if (desc->has_setter) return js_mkundef();
    }

    jsoff_t prop_off = lkp_interned(js, cur, key_intern, key_len);
    if (prop_off != 0) return resolveprop(js, mkval(T_PROP, prop_off));

    jsval_t proto = get_proto(js, cur);
    if (!is_object_type(proto)) break;
    cur = js_as_obj(proto);
  }

  return js_mkundef();
}

typedef struct {
  bool (*callback)(ant_t *js, jsval_t value, void *udata);
  void *udata;
} js_iter_ctx_t;

static iter_action_t js_iter_cb(ant_t *js, jsval_t value, void *ctx, jsval_t *out) {
  js_iter_ctx_t *ictx = (js_iter_ctx_t *)ctx;
  return ictx->callback(js, value, ictx->udata) ? ITER_CONTINUE : ITER_BREAK;
}

bool js_iter(ant_t *js, jsval_t iterable, bool (*callback)(ant_t *js, jsval_t value, void *udata), void *udata) {
  js_iter_ctx_t ctx = { .callback = callback, .udata = udata };
  jsval_t result = iter_foreach(js, iterable, js_iter_cb, &ctx);
  return !is_err(result);
}

char *js_getstr(ant_t *js, jsval_t value, size_t *len) {
  if (vtype(value) != T_STR) return NULL;
  jsoff_t n, off = vstr(js, value, &n);
  if (len != NULL) *len = n;
  return (char *) &js->mem[off];
}

void js_merge_obj(ant_t *js, jsval_t dst, jsval_t src) {
  if (vtype(dst) != T_OBJ || vtype(src) != T_OBJ) return;
  jsoff_t next = loadoff(js, (jsoff_t) vdata(src)) & ~(3U | FLAGMASK);
  while (next < js->brk && next != 0) {
    jsoff_t header = loadoff(js, next);
    if (is_slot_prop(header)) { next = next_prop(header); continue; }
    
    jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
    jsval_t val = loadval(js, next + (jsoff_t) (sizeof(next) + sizeof(koff)));
    
    js_setprop(js, dst, mkval(T_STR, koff), val);
    next = next_prop(header);
  }
}

#define UTARRAY_EACH(arr, type, var) \
  if (arr) for (type *var = (type *)utarray_front(arr), *_end = var + utarray_len(arr); var < _end; var++)

#define REHASH_REGISTRY(registry, entry, tmp, new_reg, key_field, key_size, body) \
for (typeof(registry) new_reg = NULL, *_once = NULL; !_once; _once = (void*)1, registry = new_reg) \
  HASH_ITER(hh, registry, entry, tmp) { \
    HASH_DEL(registry, entry); \
    body; \
    HASH_ADD(hh, new_reg, key_field, key_size, entry); \
}

typedef jsoff_t (*gc_fwd_off_fn)(void *ctx, jsoff_t old);
typedef jsval_t (*gc_fwd_val_fn)(void *ctx, jsval_t old);
typedef void (*gc_off_op_t)(void *cb_ctx, jsoff_t *off);
typedef void (*gc_val_op_t)(void *cb_ctx, jsval_t *val);

typedef struct {
  gc_fwd_off_fn fwd_off;
  gc_fwd_val_fn fwd_val;
  gc_fwd_off_fn weak_off;
  void *ctx;
  ant_t *js;
} gc_cb_ctx_t;

static inline void gc_reserve_off_cb(void *cb_ctx, jsoff_t *off) {
  gc_cb_ctx_t *c = cb_ctx;
  if (*off) (void)c->fwd_off(c->ctx, *off);
}

static inline void gc_reserve_val_cb(void *cb_ctx, jsval_t *val) {
  gc_cb_ctx_t *c = cb_ctx;
  (void)c->fwd_val(c->ctx, *val);
}

static inline void gc_update_off_cb(void *cb_ctx, jsoff_t *off) {
  gc_cb_ctx_t *c = cb_ctx;
  if (*off) *off = c->fwd_off(c->ctx, *off);
}

static inline void gc_update_val_cb(void *cb_ctx, jsval_t *val) {
  gc_cb_ctx_t *c = cb_ctx;
  *val = c->fwd_val(c->ctx, *val);
}

static inline jsoff_t gc_weak_off_cb(void *cb_ctx, jsoff_t old) {
  gc_cb_ctx_t *c = cb_ctx;
  return c->weak_off(c->ctx, old);
}

void js_gc_visit_frame_funcs(ant_t *js, void (*visitor)(void *, sv_func_t *), void *ctx) {
  sv_vm_visit_frame_funcs(js->vm, visitor, ctx);
  for (coroutine_t *coro = pending_coroutines.head; coro; coro = coro->next) {
    if (coro->sv_vm) sv_vm_visit_frame_funcs(coro->sv_vm, visitor, ctx);
  }
}

static void gc_roots_common(gc_off_op_t op_off, gc_val_op_t op_val, gc_cb_ctx_t *c) {
  if (rt && rt->js == c->js)
    op_val(c, &rt->ant_obj);

  for (coroutine_t *coro = pending_coroutines.head; coro; coro = coro->next) {
    op_val(c, &coro->this_val);
    op_val(c, &coro->super_val);
    op_val(c, &coro->new_target);
    op_val(c, &coro->awaited_promise);
    op_val(c, &coro->result);
    op_val(c, &coro->async_func);
    op_val(c, &coro->yield_value);
    op_val(c, &coro->async_promise);
  }

  js_esm_gc_roots(op_val, c);
  op_val(c, &c->js->import_meta);
  timer_gc_update_roots(op_val, c);
  ffi_gc_update_roots(op_val, c);
  fetch_gc_update_roots(op_val, c);
  fs_gc_update_roots(op_val, c);
  child_process_gc_update_roots(op_val, c);
  readline_gc_update_roots(op_val, c);
  process_gc_update_roots(op_val, c);
  navigator_gc_update_roots(op_val, c);
  server_gc_update_roots(op_val, c);
  events_gc_update_roots(op_val, c);
  lmdb_gc_update_roots(op_val, c);
  symbol_gc_update_roots(op_val, c);

  op_val(c, &c->js->global);
  op_val(c, &c->js->object);
  op_val(c, &c->js->this_val);
  op_val(c, &c->js->new_target);
  op_val(c, &c->js->module_ns);
  op_val(c, &c->js->current_func);
  op_val(c, &c->js->thrown_value);
  op_val(c, &c->js->length_str);

  sv_vm_gc_roots(c->js->vm, op_val, c);
  sv_vm_gc_roots_pending(op_val, c);

  for (jshdl_t i = 0; i < c->js->gc_roots_len; i++) op_val(c, &c->js->gc_roots[i]);
}

void js_gc_reserve_roots(GC_RESERVE_ARGS) {
  #define RSV_OFF(x) ((x) ? (void)fwd_off(ctx, x) : (void)0)
  #define RSV_VAL(x) (void)fwd_val(ctx, x)
  
  gc_cb_ctx_t cb_ctx = { fwd_off, fwd_val, NULL, ctx, js };
  gc_roots_common(gc_reserve_off_cb, gc_reserve_val_cb, &cb_ctx);
  collections_gc_reserve_roots(gc_reserve_val_cb, &cb_ctx);
  
  promise_data_entry_t *pd, *pd_tmp;
  HASH_ITER(hh, promise_registry, pd, pd_tmp) {
    bool can_collect = (pd->state != 0) 
      && (utarray_len(pd->handlers) == 0) 
      && !pd->processing;
    
    if (can_collect) continue;
    RSV_OFF(pd->obj_offset);
    RSV_VAL(pd->value);
    
    UTARRAY_EACH(pd->handlers, promise_handler_t, h) {
      RSV_VAL(h->onFulfilled); 
      RSV_VAL(h->onRejected); 
      RSV_VAL(h->nextPromise);
    }
  }
  
  proxy_data_t *proxy, *proxy_tmp;
  HASH_ITER(hh, proxy_registry, proxy, proxy_tmp) {
    RSV_VAL(proxy->target);
    RSV_VAL(proxy->handler);
  }
  
  descriptor_entry_t *desc, *desc_tmp;
  HASH_ITER(hh, desc_registry, desc, desc_tmp) {
    if (desc->has_getter) RSV_VAL(desc->getter);
    if (desc->has_setter) RSV_VAL(desc->setter);
  }

  // accessor registry is a weak reference
  // objects only survive if reachable from other roots
  (void)accessor_registry;
  
  #undef RSV_OFF
  #undef RSV_VAL
}

void js_gc_update_roots(GC_UPDATE_ARGS) {
  #define FWD_OFF(x) ((x) ? ((x) = fwd_off(ctx, x)) : 0)
  #define FWD_VAL(x) ((x) = fwd_val(ctx, x))
  #define IS_UNREACHABLE(old, new) ((new) == (old) && (old) != 0 && (old) < js->brk)
  
  gc_cb_ctx_t cb_ctx = { fwd_off, fwd_val, weak_off, ctx, js };
  gc_roots_common(gc_update_off_cb, gc_update_val_cb, &cb_ctx);
  
  promise_data_entry_t *pd, *pd_tmp;
  promise_data_entry_t *new_unhandled = NULL;

  for (
    promise_data_entry_t *new_promise_registry = NULL, *_once = NULL; !_once; _once = (void*)1, 
    promise_registry = new_promise_registry, unhandled_rejections = new_unhandled
  )
    HASH_ITER(hh, promise_registry, pd, pd_tmp) {
      HASH_DEL(promise_registry, pd);
      promise_data_entry_t *in_unhandled = NULL;
      
      HASH_FIND(
        hh_unhandled, unhandled_rejections, 
        &pd->promise_id, sizeof(uint32_t), in_unhandled
      );
      
      if (in_unhandled) HASH_DELETE(hh_unhandled, unhandled_rejections, pd);
      jsoff_t new_off = weak_off(ctx, pd->obj_offset);
      
      if (new_off == (jsoff_t)~0 && !pd->processing) { 
        utarray_free(pd->handlers); 
        free(pd); continue; 
      }
      
      bool can_collect = (pd->state != 0) 
        && (utarray_len(pd->handlers) == 0) 
        && !pd->processing;
      
      if (can_collect) { 
        utarray_free(pd->handlers); 
        free(pd); continue; 
      }
      
      pd->obj_offset = new_off;
      FWD_VAL(pd->value);
      
      UTARRAY_EACH(pd->handlers, promise_handler_t, h) {
        FWD_VAL(h->onFulfilled);
        FWD_VAL(h->onRejected);
        FWD_VAL(h->nextPromise);
      }
      
      HASH_ADD(hh, new_promise_registry, promise_id, sizeof(uint32_t), pd);
      if (in_unhandled) HASH_ADD(hh_unhandled, new_unhandled, promise_id, sizeof(uint32_t), pd);
    }


  proxy_data_t *proxy, *proxy_tmp;
  for (proxy_data_t *new_proxy_registry = NULL, *_once = NULL; !_once; _once = (void*)1, proxy_registry = new_proxy_registry)
    HASH_ITER(hh, proxy_registry, proxy, proxy_tmp) {
      HASH_DEL(proxy_registry, proxy);
      jsoff_t new_off = weak_off(ctx, proxy->obj_offset);
      if (new_off == (jsoff_t)~0) { free(proxy); continue; }
      proxy->obj_offset = new_off;
      FWD_VAL(proxy->target); FWD_VAL(proxy->handler);
      HASH_ADD(hh, new_proxy_registry, obj_offset, sizeof(jsoff_t), proxy);
    }

  dynamic_accessors_t *acc, *acc_tmp;
  for (dynamic_accessors_t *new_acc_registry = NULL, *_once = NULL; !_once; _once = (void*)1, accessor_registry = new_acc_registry)
    HASH_ITER(hh, accessor_registry, acc, acc_tmp) {
      HASH_DEL(accessor_registry, acc);
      jsoff_t new_off = weak_off(ctx, acc->obj_offset);
      if (new_off == (jsoff_t)~0) { free(acc); continue; }
      acc->obj_offset = new_off;
      HASH_ADD(hh, new_acc_registry, obj_offset, sizeof(jsoff_t), acc);
    }

  descriptor_entry_t *desc, *desc_tmp;
  for (descriptor_entry_t *new_desc_registry = NULL, *_once = NULL; !_once; _once = (void*)1, desc_registry = new_desc_registry)
    HASH_ITER(hh, desc_registry, desc, desc_tmp) {
      HASH_DEL(desc_registry, desc);
      jsoff_t new_off = weak_off(ctx, (jsoff_t)(desc->key >> 32));
      if (new_off == (jsoff_t)~0) { free(desc); continue; }
      if (desc->has_getter) FWD_VAL(desc->getter);
      if (desc->has_setter) FWD_VAL(desc->setter);
      if (desc->prop_name) {
        desc->key = ((uint64_t)new_off << 32) | (uint32_t)(desc->key & 0xFFFFFFFF);
      } else {
        jsoff_t new_sym = fwd_off(ctx, desc->sym_off);
        desc->sym_off = new_sym;
        desc->key = make_sym_desc_key(new_off, new_sym);
      }
      desc->obj_off = new_off;
      HASH_ADD(hh, new_desc_registry, key, sizeof(uint64_t), desc);
    }

  intern_prop_cache_gen++;
  collections_gc_update_roots(gc_weak_off_cb, gc_update_val_cb, &cb_ctx);
  regex_gc_update_roots(gc_weak_off_cb, gc_update_val_cb, &cb_ctx);
  
  #undef FWD_OFF
  #undef FWD_VAL
}

#undef UTARRAY_EACH
#undef REHASH_REGISTRY

bool js_chkargs(jsval_t *args, int nargs, const char *spec) {
  int i = 0, ok = 1;
  for (; ok && i < nargs && spec[i]; i++) {
    uint8_t t = vtype(args[i]), c = (uint8_t) spec[i];
    ok = (c == 'b' && t == T_BOOL) || (c == 'd' && t == T_NUM) ||
         (c == 's' && t == T_STR) || (c == 'j');
  }
  if (spec[i] != '\0' || i != nargs) ok = 0;
  return ok;
}

static jsval_t js_eval_bytecode_mode(ant_t *js, const char *buf, size_t len, sv_compile_mode_t mode, bool parse_strict) {
  if (len == (size_t)~0U) len = strlen(buf);
  sv_ast_t *program = sv_parse(js, buf, (jsoff_t)len, parse_strict);

  if (!program) {
    if (js->thrown_exists) return mkval(T_ERR, 0);
    return js_mkerr_typed(js, JS_ERR_INTERNAL | JS_ERR_NO_STACK, "Unexpected parse error");
  }

  sv_func_t *func = sv_compile(js, program, mode, buf, (jsoff_t)len);
  if (!func) {
    if (js->thrown_exists) return mkval(T_ERR, 0);
    return js_mkerr_typed(js, JS_ERR_INTERNAL | JS_ERR_NO_STACK, "Unexpected compile error");
  }
  
  js_clear_error_site(js);   
  jsval_t result;

  if (sv_dump_bytecode_unlikely) sv_disasm(js, func, js->filename);
  if (func->is_tla) result = sv_execute_entry_tla(js, func, js->this_val);
  else result = sv_execute_entry(js->vm, func, js->this_val, NULL, 0);

  return result;
}

jsval_t js_eval_bytecode(ant_t *js, const char *buf, size_t len) {
  return js_eval_bytecode_mode(js, buf, len, SV_COMPILE_SCRIPT, false);
}

jsval_t js_eval_bytecode_module(ant_t *js, const char *buf, size_t len) {
  return js_eval_bytecode_mode(js, buf, len, SV_COMPILE_MODULE, false);
}

jsval_t js_eval_bytecode_eval(ant_t *js, const char *buf, size_t len) {
  return js_eval_bytecode_mode(js, buf, len, SV_COMPILE_EVAL, false);
}

jsval_t js_eval_bytecode_eval_with_strict(ant_t *js, const char *buf, size_t len, bool inherit_strict) {
  return js_eval_bytecode_mode(js, buf, len, SV_COMPILE_EVAL, inherit_strict);
}

jsval_t js_eval_bytecode_repl(ant_t *js, const char *buf, size_t len) {
  return js_eval_bytecode_mode(js, buf, len, SV_COMPILE_REPL, false);
}

jsval_t sv_call_native(
  ant_t *js, jsval_t func, jsval_t this_val,
  jsval_t *args, int nargs
) {
  if (vtype(func) == T_FFI)
    return ffi_call_by_index(js, (unsigned int)vdata(func), args, nargs);

  if (vtype(func) == T_CFUNC) {
    jsval_t saved_this = js->this_val;
    js->this_val = this_val;
    jsval_t (*fn)(ant_t *, jsval_t *, int) = (jsval_t(*)(ant_t *, jsval_t *, int))vdata(func);
    jsval_t res = fn(js, args, nargs);
    js->this_val = saved_this;
    return res;
  }

  if (vtype(func) == T_FUNC) {
    jsval_t func_obj = js_func_obj(func);

    jsval_t cfunc_slot = get_slot(js, func_obj, SLOT_CFUNC);
    if (vtype(cfunc_slot) == T_CFUNC) {
      jsval_t slot_bound_this = get_slot(js, func_obj, SLOT_BOUND_THIS);
      int final_nargs;
      jsval_t *combined = resolve_bound_args(js, func_obj, args, nargs, &final_nargs);
      jsval_t *final_args = combined ? combined : args;
      jsval_t saved_func = js->current_func;
      jsval_t saved_this = js->this_val;
      js->current_func = func;
      js->this_val = (vtype(slot_bound_this) != T_UNDEF) ? slot_bound_this : this_val;
      jsval_t (*fn)(ant_t *, jsval_t *, int) = (jsval_t(*)(ant_t *, jsval_t *, int))vdata(cfunc_slot);
      jsval_t res = fn(js, final_args, final_nargs);
      js->current_func = saved_func;
      js->this_val = saved_this;
      if (combined) free(combined);
      return res;
    }

    jsval_t builtin_slot = get_slot(js, func_obj, SLOT_BUILTIN);
    if (vtype(builtin_slot) == T_NUM && (int)tod(builtin_slot) == BUILTIN_OBJECT) {
      jsval_t saved_this = js->this_val;
      js->this_val = this_val;
      jsval_t res = builtin_Object(js, args, nargs);
      js->this_val = saved_this;
      return res;
    }
  }

  return js_mkerr_typed(js, JS_ERR_TYPE, "%s is not a function", typestr(vtype(func)));
}

ant_iter_t js_prop_iter_begin(ant_t *js, jsval_t obj) {
  ant_iter_t iter = {.ctx = js, .off = 0};
  
  uint8_t t = vtype(obj);
  if (t == T_OBJ || t == T_ARR || t == T_FUNC) {
    iter.off = (jsoff_t)vdata(js_as_obj(obj));
  }
  
  return iter;
}

bool js_prop_iter_next(ant_iter_t *iter, const char **key, size_t *key_len, jsval_t *value) {
  if (!iter || !iter->ctx) return false;
  
  ant_t *js = (ant_t *)iter->ctx;
  jsoff_t next = loadoff(js, iter->off) & ~(3U | FLAGMASK);
  
  while (next < js->brk && next != 0) {
    jsoff_t header = loadoff(js, next);
    if (!is_slot_prop(header) && !is_sym_key_prop(js, next)) break;
    next = next_prop(header);
  }
  
  if (next >= js->brk || next == 0) return false;
  iter->off = next;
  
  jsoff_t koff = loadoff(js, next + (jsoff_t)sizeof(jsoff_t));
  jsval_t val = loadval(js, next + (jsoff_t)(sizeof(jsoff_t) * 2));
  
  if (key) {
    jsoff_t klen = offtolen(loadoff(js, koff));
    *key = (const char *)&js->mem[koff + sizeof(jsoff_t)];
    if (key_len) *key_len = klen;
  }
  if (value) *value = val;
  
  return true;
}

void js_prop_iter_end(ant_iter_t *iter) {
  if (iter) { iter->off = 0; iter->ctx = NULL; }
}

jsval_t js_mkpromise(ant_t *js) { return mkpromise(js); }
void js_resolve_promise(ant_t *js, jsval_t promise, jsval_t value) { resolve_promise(js, promise, value); }
void js_reject_promise(ant_t *js, jsval_t promise, jsval_t value) { reject_promise(js, promise, value); }

void js_check_unhandled_rejections(ant_t *js) {
  promise_data_entry_t *pd, *tmp;
  
  HASH_ITER(hh_unhandled, unhandled_rejections, pd, tmp) {
    if (pd->has_rejection_handler) {
      HASH_DELETE(hh_unhandled, unhandled_rejections, pd); continue;
    }
    
    if (pd->trigger_pid != 0) {
      promise_data_entry_t *parent;
      HASH_FIND(hh, promise_registry, &pd->trigger_pid, sizeof(uint32_t), parent);
      if (parent && parent->has_rejection_handler) {
        HASH_DELETE(hh_unhandled, unhandled_rejections, pd); continue;
      }
    }
    
    if (js->fatal_error) {
      js->thrown_exists = true;
      js->thrown_value = pd->value;
      print_uncaught_throw(js);
      js_destroy(js); exit(1);
    }
    
    print_unhandled_promise_rejection(js, pd->value);
    pd->has_rejection_handler = true;
    HASH_DELETE(hh_unhandled, unhandled_rejections, pd);
  }
}

bool js_is_slot_prop(jsoff_t header) { return is_slot_prop(header); }
jsoff_t js_next_prop(jsoff_t header) { return next_prop(header); }
jsoff_t js_loadoff(ant_t *js, jsoff_t off) { return loadoff(js, off); }

void js_set_getter(ant_t *js, jsval_t obj, js_getter_fn getter) {
  if (!is_object_type(obj)) return;
  if (vtype(obj) != T_OBJ) obj = js_as_obj(obj);
  jsoff_t obj_off = (jsoff_t)vdata(obj);
  dynamic_accessors_t *entry = NULL;
  HASH_FIND(hh, accessor_registry, &obj_off, sizeof(jsoff_t), entry);
  if (!entry) {
    entry = (dynamic_accessors_t *)malloc(sizeof(dynamic_accessors_t));
    if (!entry) return;
    entry->obj_offset = obj_off;
    entry->getter = NULL; entry->setter = NULL;
    entry->deleter = NULL; entry->keys = NULL;
    HASH_ADD(hh, accessor_registry, obj_offset, sizeof(jsoff_t), entry);
  }
  entry->getter = getter;
}

void js_set_setter(ant_t *js, jsval_t obj, js_setter_fn setter) {
  if (!is_object_type(obj)) return;
  if (vtype(obj) != T_OBJ) obj = js_as_obj(obj);
  jsoff_t obj_off = (jsoff_t)vdata(obj);
  dynamic_accessors_t *entry = NULL;
  HASH_FIND(hh, accessor_registry, &obj_off, sizeof(jsoff_t), entry);
  if (!entry) {
    entry = (dynamic_accessors_t *)malloc(sizeof(dynamic_accessors_t));
    if (!entry) return;
    entry->obj_offset = obj_off;
    entry->getter = NULL; entry->setter = NULL;
    entry->deleter = NULL; entry->keys = NULL;
    HASH_ADD(hh, accessor_registry, obj_offset, sizeof(jsoff_t), entry);
  }
  entry->setter = setter;
}

void js_set_deleter(ant_t *js, jsval_t obj, js_deleter_fn deleter) {
  if (!is_object_type(obj)) return;
  if (vtype(obj) != T_OBJ) obj = js_as_obj(obj);
  jsoff_t obj_off = (jsoff_t)vdata(obj);
  dynamic_accessors_t *entry = NULL;
  HASH_FIND(hh, accessor_registry, &obj_off, sizeof(jsoff_t), entry);
  if (!entry) {
    entry = (dynamic_accessors_t *)malloc(sizeof(dynamic_accessors_t));
    if (!entry) return;
    entry->obj_offset = obj_off;
    entry->getter = NULL; entry->setter = NULL;
    entry->deleter = NULL; entry->keys = NULL;
    HASH_ADD(hh, accessor_registry, obj_offset, sizeof(jsoff_t), entry);
  }
  entry->deleter = deleter;
}

void js_set_keys(ant_t *js, jsval_t obj, js_keys_fn keys) {
  if (!is_object_type(obj)) return;
  if (vtype(obj) != T_OBJ) obj = js_as_obj(obj);
  jsoff_t obj_off = (jsoff_t)vdata(obj);
  dynamic_accessors_t *entry = NULL;
  HASH_FIND(hh, accessor_registry, &obj_off, sizeof(jsoff_t), entry);
  if (!entry) {
    entry = (dynamic_accessors_t *)malloc(sizeof(dynamic_accessors_t));
    if (!entry) return;
    entry->obj_offset = obj_off;
    entry->getter = NULL; entry->setter = NULL;
    entry->deleter = NULL; entry->keys = NULL;
    HASH_ADD(hh, accessor_registry, obj_offset, sizeof(jsoff_t), entry);
  }
  entry->keys = keys;
}
