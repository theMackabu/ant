#if defined(__GNUC__) && !defined(__clang__)
  #pragma GCC optimize("O3,inline")
#endif

#include <compat.h> // IWYU pragma: keep

#include "ant.h"
#include "tokens.h"
#include "common.h"
#include "arena.h"
#include "utils.h"
#include "runtime.h"
#include "internal.h"
#include "sugar.h"
#include "stack.h"
#include "errors.h"
#include "utf8.h"

#include <uv.h>
#include <oxc.h>
#include <assert.h>
#include <pcre2.h>
#include <stdarg.h>
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
#include "modules/json.h"
#include "modules/buffer.h"
#include "esm/remote.h"

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

typedef struct {
  jsval_t *stack;
  int depth;
  int capacity;
} this_stack_t;

static this_stack_t global_this_stack = {NULL, 0, 0};

const UT_icd jsoff_icd = {
  .sz = sizeof(jsoff_t),
  .init = NULL,
  .copy = NULL,
  .dtor = NULL,
};

const UT_icd jsval_icd = {
  .sz = sizeof(jsval_t),
  .init = NULL,
  .copy = NULL,
  .dtor = NULL,
};

UT_array *global_scope_stack = NULL;
UT_array *saved_scope_stack = NULL;

typedef struct {
  const char *name;
  jsoff_t name_len;
  bool is_loop;
  bool is_block;
} label_entry_t;

static const UT_icd label_entry_icd = {
  .sz = sizeof(label_entry_t),
  .init = NULL,
  .copy = NULL,
  .dtor = NULL,
};

static UT_array *label_stack = NULL;
static const char *break_target_label = NULL;
static jsoff_t break_target_label_len = 0;
static const char *continue_target_label = NULL;
static jsoff_t continue_target_label_len = 0;

#define F_BREAK_LABEL 1U
#define F_CONTINUE_LABEL 2U
static uint8_t label_flags = 0;

typedef struct esm_module {
  char *path;
  char *resolved_path;
  jsval_t namespace_obj;
  jsval_t default_export;
  bool is_loaded;
  bool is_loading;
  bool is_json;
  bool is_text;
  bool is_image;
  bool is_url;
  char *url_content;
  size_t url_content_len;
  struct esm_module *next;
  UT_hash_handle hh;
} esm_module_t;

typedef struct {
  esm_module_t *modules;
  int count;
} esm_module_cache_t;

typedef struct ant_library {
  char name[256];
  ant_library_init_fn init_fn;
  UT_hash_handle hh;
} ant_library_t;

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

static interned_string_t *intern_buckets[ANT_LIMIT_SIZE_CACHE];

typedef struct {
  jsoff_t obj_off;
  const char *intern_ptr;
  jsoff_t prop_off;
  jsoff_t tail;
} intern_prop_cache_entry_t;
static intern_prop_cache_entry_t intern_prop_cache[ANT_LIMIT_SIZE_CACHE];

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

typedef struct promise_data_entry {
  jsval_t value;
  UT_array *handlers;
  uint32_t promise_id;
  uint32_t trigger_pid;
  jsoff_t obj_offset;
  int state;
  bool has_rejection_handler;
  UT_hash_handle hh;
  UT_hash_handle hh_unhandled;
} promise_data_entry_t;

static promise_data_entry_t *promise_registry = NULL;
static promise_data_entry_t *unhandled_rejections = NULL;
static uint32_t next_promise_id = 1;

static promise_data_entry_t *get_promise_data(uint32_t promise_id, bool create);
static uint32_t get_promise_id(struct js *js, jsval_t p);
static bool js_try_grow_memory(struct js *js, size_t needed);

typedef struct map_entry {
  char *key;
  jsval_t value;
  UT_hash_handle hh;
} map_entry_t;

typedef struct set_entry {
  jsval_t value;
  char *key;
  UT_hash_handle hh;
} set_entry_t;

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
  UT_hash_handle hh;
} dynamic_accessors_t;

typedef struct descriptor_entry {
  uint64_t key;
  jsoff_t obj_off;
  char *prop_name;
  size_t prop_len;
  bool writable;
  bool enumerable;
  bool configurable;
  bool has_getter;
  bool has_setter;
  jsval_t getter;
  jsval_t setter;
  UT_hash_handle hh;
} descriptor_entry_t;

typedef struct {
  jsoff_t obj_off;
  jsoff_t key_off;
} propref_data_t;

static const UT_icd propref_icd = { sizeof(propref_data_t), NULL, NULL, NULL };
static UT_array *propref_stack = NULL;

typedef struct parsed_param {
  jsoff_t name_off;
  jsoff_t name_len;
  jsoff_t default_start;
  jsoff_t default_len;
  jsoff_t pattern_off;
  jsoff_t pattern_len;
  bool is_destruct;
} parsed_param_t;

typedef struct parsed_func {
  uint64_t code_hash;
  jsoff_t body_start;
  jsoff_t body_len;
  int param_count;
  bool has_rest;
  bool is_strict;
  jsoff_t rest_param_start;
  jsoff_t rest_param_len;
  UT_array *params;
  UT_hash_handle hh;
} parsed_func_t;

static const UT_icd parsed_param_icd = {
  .sz = sizeof(parsed_param_t),
  .init = NULL,
  .copy = NULL,
  .dtor = NULL,
};

static parsed_func_t *func_parse_cache = NULL;
static ant_library_t *library_registry = NULL;
static esm_module_cache_t global_module_cache = {NULL, 0};
static proxy_data_t *proxy_registry = NULL;
static dynamic_accessors_t *accessor_registry = NULL;
static descriptor_entry_t *desc_registry = NULL;

typedef struct map_registry_entry {
  map_entry_t **head;
  UT_hash_handle hh;
} map_registry_entry_t;

typedef struct set_registry_entry {
  set_entry_t **head;
  UT_hash_handle hh;
} set_registry_entry_t;

static map_registry_entry_t *map_registry = NULL;
static set_registry_entry_t *set_registry = NULL;

void ant_register_library(ant_library_init_fn init_fn, const char *name, ...) {
  va_list args;
  const char *alias = name;
  
  va_start(args, name);
  while (alias != NULL) {
    ant_library_t *lib = (ant_library_t *)ant_calloc(sizeof(ant_library_t));
    if (!lib) break;
    
    strncpy(lib->name, alias, sizeof(lib->name) - 1);
    lib->name[sizeof(lib->name) - 1] = '\0';
    lib->init_fn = init_fn;
    
    HASH_ADD_STR(library_registry, name, lib);
    alias = va_arg(args, const char *);
  }
  va_end(args);
}

static ant_library_t* find_library(const char *specifier, size_t spec_len) {
  ant_library_t *lib = NULL;
  
  char key[256];
  if (spec_len >= sizeof(key)) return NULL;
  memcpy(key, specifier, spec_len);
  key[spec_len] = '\0';
  
  HASH_FIND_STR(library_registry, key, lib);
  return lib;
}

static const char *typestr_raw(uint8_t t) {
  const char *names[] = { 
    "object", "prop", "string", "undefined", "null", "number",
    "boolean", "function", "coderef", "cfunc", "err", "array", 
    "promise", "typedarray", "bigint", "propref", "symbol", "generator", "ffi"
  };
  
  return (t < sizeof(names) / sizeof(names[0])) ? names[t] : "??";
}

jsval_t tov(double d) { 
  union { double d; jsval_t v; } u = {d}; return u.v; 
}

double tod(jsval_t v) { 
  union { jsval_t v; double d; } u = {v}; return u.d; 
}

static bool is_tagged(jsval_t v) {
  return (v >> 53) == NANBOX_PREFIX_CHK;
}

size_t vdata(jsval_t v) { 
  return (size_t)(v & NANBOX_DATA_MASK); 
}

#define PROPREF_STACK_FLAG  0x800000000000ULL
#define PROPREF_PRIM_FLAG   0x400000000000ULL
#define PROPREF_INDEX_MASK  0x3FFFFFFFFFFFULL
#define PROPREF_OFF_MASK    0x7FFFFFULL
#define PROPREF_PAYLOAD     0x7FFFFFULL
#define PROPREF_SAFE_MASK   0x7FFFFFULL
#define PROPREF_KEY_SHIFT   23U

typedef struct {
  jsval_t prim_val;
  jsoff_t key_off;
} prim_propref_data_t;

static UT_icd prim_propref_icd = { sizeof(prim_propref_data_t), NULL, NULL, NULL };
static UT_array *prim_propref_stack = NULL;

static jsoff_t coderefoff(jsval_t v) { return v & PROPREF_OFF_MASK; }
static jsoff_t codereflen(jsval_t v) { return (v >> PROPREF_KEY_SHIFT) & PROPREF_OFF_MASK; }

static jsval_t get_slot(struct js *js, jsval_t obj, internal_slot_t slot);
static void set_slot(struct js *js, jsval_t obj, internal_slot_t slot, jsval_t value);

static jsval_t get_proto(struct js *js, jsval_t obj);
static void set_proto(struct js *js, jsval_t obj, jsval_t proto);

static void clear_break_label(void) {
  break_target_label = NULL;
  break_target_label_len = 0;
  label_flags &= ~F_BREAK_LABEL;
}

static void clear_continue_label(void) {
  continue_target_label = NULL;
  continue_target_label_len = 0;
  label_flags &= ~F_CONTINUE_LABEL;
}

static inline propref_data_t *propref_get_entry(jsval_t v) {
  uint64_t data = v & NANBOX_DATA_MASK;
  if (!(data & PROPREF_STACK_FLAG)) return NULL;
  
  int idx = (int)(data & PROPREF_INDEX_MASK);
  if (!propref_stack || idx < 0 || idx >= (int)utarray_len(propref_stack)) return NULL;
  
  return (propref_data_t *)utarray_eltptr(propref_stack, (unsigned)idx);
}

static jsoff_t propref_obj(jsval_t v) {
  propref_data_t *entry = propref_get_entry(v);
  if (entry) return entry->obj_off;
  
  uint64_t data = v & NANBOX_DATA_MASK;
  return (data & PROPREF_STACK_FLAG) ? 0 : (data & PROPREF_OFF_MASK);
}

static jsoff_t propref_key(jsval_t v) {
  propref_data_t *entry = propref_get_entry(v);
  if (entry) return entry->key_off;
  
  uint64_t data = v & NANBOX_DATA_MASK;
  return (data & PROPREF_STACK_FLAG) ? 0 : ((data >> PROPREF_KEY_SHIFT) & PROPREF_OFF_MASK);
}

static inline jsoff_t offtolen(jsoff_t off) { return (off >> 3) - 1; }
static inline jsoff_t align64(jsoff_t v) { return (v + 7) & ~7ULL; }

static void saveoff(struct js *js, jsoff_t off, jsoff_t val) { 
  memcpy(&js->mem[off], &val, sizeof(val)); 
}

static void saveval(struct js *js, jsoff_t off, jsval_t val) { 
  memcpy(&js->mem[off], &val, sizeof(val)); 
}

static const char *typestr(uint8_t t) {
  if (t == T_CFUNC) return "function";
  if (t == T_ARR) return "object";
  if (t == T_NULL) return "object";
  return typestr_raw(t);
}

void js_set_needs_gc(struct js *js, bool needs) {
  js->needs_gc = needs;
}

void js_set_gc_suppress(struct js *js, bool suppress) {
  js->gc_suppress = suppress;
}

uint8_t vtype(jsval_t v) { 
  return is_tagged(v) ? ((v >> NANBOX_TYPE_SHIFT) & NANBOX_TYPE_MASK) : (uint8_t)T_NUM; 
}

jsval_t mkval(uint8_t type, uint64_t data) { 
  return NANBOX_PREFIX | ((jsval_t)(type & NANBOX_TYPE_MASK) << NANBOX_TYPE_SHIFT) | (data & NANBOX_DATA_MASK);
}

jsval_t js_obj_to_func(jsval_t obj) {
  return mkval(T_FUNC, vdata(obj));
}

jsval_t js_mktypedarray(void *data) {
  return mkval(T_TYPEDARRAY, (uintptr_t)data);
}

void *js_gettypedarray(jsval_t val) {
  if (vtype(val) != T_TYPEDARRAY) return NULL;
  return (void *)vdata(val);
}

jsval_t js_get_slot(struct js *js, jsval_t obj, internal_slot_t slot) { 
  return get_slot(js, obj, slot); 
}

void js_set_slot(struct js *js, jsval_t obj, internal_slot_t slot, jsval_t value) { 
  set_slot(js, obj, slot, value); 
}

jsval_t js_mkffi(unsigned int index) {
  return mkval(T_FFI, (uint64_t)index);
}

int js_getffi(jsval_t val) {
  if (vtype(val) != T_FFI) return -1;
  return (int)vdata(val);
}

static jsval_t mkcoderef(jsval_t off, jsoff_t len) { 
  return mkval(T_CODEREF, (off & PROPREF_OFF_MASK) | ((jsval_t)(len & PROPREF_OFF_MASK) << PROPREF_KEY_SHIFT));
}

static jsval_t mkpropref(jsoff_t obj_off, jsoff_t key_off) {
  if (obj_off <= PROPREF_SAFE_MASK && key_off <= PROPREF_SAFE_MASK) {
    return mkval(T_PROPREF, (obj_off & PROPREF_OFF_MASK) | ((jsval_t)(key_off & PROPREF_OFF_MASK) << PROPREF_KEY_SHIFT));
  }
  
  if (!propref_stack) utarray_new(propref_stack, &propref_icd);
  if (utarray_len(propref_stack) > 1024) utarray_clear(propref_stack);
  
  propref_data_t entry = { obj_off, key_off };
  utarray_push_back(propref_stack, &entry);
  int idx = (int)utarray_len(propref_stack) - 1;

  return mkval(T_PROPREF, PROPREF_STACK_FLAG | (uint64_t)idx);
}

static jsval_t mkprim_propref(jsval_t prim_val, jsoff_t key_off) {
  if (!prim_propref_stack) utarray_new(prim_propref_stack, &prim_propref_icd);
  if (utarray_len(prim_propref_stack) > 256) utarray_clear(prim_propref_stack);
  
  prim_propref_data_t entry = { prim_val, key_off };
  utarray_push_back(prim_propref_stack, &entry);
  int idx = (int)utarray_len(prim_propref_stack) - 1;
  
  return mkval(T_PROPREF, PROPREF_PRIM_FLAG | (uint64_t)idx);
}

static inline bool is_prim_propref(jsval_t v) {
  if (vtype(v) != T_PROPREF) return false;
  uint64_t data = v & NANBOX_DATA_MASK;
  return (data & PROPREF_PRIM_FLAG) != 0;
}

static inline prim_propref_data_t *prim_propref_get(jsval_t v) {
  uint64_t data = v & NANBOX_DATA_MASK;
  if (!(data & PROPREF_PRIM_FLAG)) return NULL;
  
  int idx = (int)(data & PROPREF_INDEX_MASK);
  if (!prim_propref_stack || idx < 0 || idx >= (int)utarray_len(prim_propref_stack)) return NULL;
  
  return (prim_propref_data_t *)utarray_eltptr(prim_propref_stack, (unsigned)idx);
}

inline size_t js_getbrk(struct js *js) { 
  return (size_t) js->brk;
}

static inline uint8_t unhex(uint8_t c) {
  return (c & 0xF) + (c >> 6) * 9;
}

static inline int is_body_end_tok(int tok) {
  return body_end_tok[tok];
}

static inline bool is_assign(uint8_t tok) {
  return tok >= TOK_ASSIGN && tok <= TOK_NULLISH_ASSIGN;
}

static inline bool is_identifier_like(uint8_t tok) {
  return tok >= TOK_IDENTIFIER && tok < TOK_IDENT_LIKE_END;
}

static inline bool is_keyword_propname(uint8_t tok) { 
  return (tok >= TOK_ASYNC && tok <= TOK_GLOBAL_THIS) || tok == TOK_TYPEOF; 
}

static inline bool is_contextual_keyword(uint8_t tok) {
  return tok == TOK_FROM || tok == TOK_OF || tok == TOK_AS || tok == TOK_ASYNC;
}

static inline bool js_stack_overflow(struct js *js) {
  volatile char marker;
  uintptr_t curr = (uintptr_t)&marker;
  
  mco_coro *coro = mco_running();
  if (coro != NULL) {
    uintptr_t stack_top = (uintptr_t)coro->stack_base + coro->stack_size;
    size_t limit = coro->stack_size / 2;
    size_t used = (stack_top > curr) ? (stack_top - curr) : (curr - stack_top);
    return used > limit;
  }
  
  if (js->stack_limit == 0 || js->cstk == NULL) return false;
  uintptr_t base = (uintptr_t)js->cstk;
  size_t used = (base > curr) ? (base - curr) : (curr - base);
  return used > js->stack_limit;
}

static inline bool is_valid_param_name(uint8_t tok) {
  return tok == TOK_IDENTIFIER || is_contextual_keyword(tok);
}

static bool is_valid_arrow_param_tok(uint8_t tok) {
  static const uint64_t bits[4] = {
    0x000C000000000FCCull,
    0x041CC0100BC00808ull,
    0x0000000000800100ull,
    0x0000000000000000ull
  };
  return (bits[tok >> 6] >> (tok & 63)) & 1;
}

static inline bool is_block_tok(uint8_t tok) {
  static const uint64_t bits[4] = {
    0x2108000000000110ull, 
    0x0000000000124075ull, 0, 0
  };
  return (bits[tok >> 6] >> (tok & 63)) & 1;
}

static inline bool is_asi_ok_tok(uint8_t tok) {
  static const uint64_t bits[4] = {
    0x0000000000000212ull, 0, 0, 0
  };
  return (bits[tok >> 6] >> (tok & 63)) & 1;
}

static inline bool is_unboxed_obj(struct js *js, jsval_t val, jsval_t expected_proto) {
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

typedef struct {
  const char *code;
  jsoff_t clen, pos;
  uint8_t tok, consumed;
} js_parse_state_t;

#define JS_SAVE_STATE(js, state) do { \
  (state).code = (js)->code; \
  (state).clen = (js)->clen; \
  (state).pos = (js)->pos; \
  (state).tok = (js)->tok; \
  (state).consumed = (js)->consumed; \
} while(0)

#define JS_RESTORE_STATE(js, state) do { \
  (js)->code = (state).code; \
  (js)->clen = (state).clen; \
  (js)->pos = (state).pos; \
  (js)->tok = (state).tok; \
  (js)->consumed = (state).consumed; \
} while(0)

static size_t strstring(struct js *js, jsval_t value, char *buf, size_t len);
static size_t strkey(struct js *js, jsval_t value, char *buf, size_t len);

static inline jsoff_t loadoff(struct js *js, jsoff_t off) {
  assert(off + sizeof(jsoff_t) <= js->brk); jsoff_t val;
  memcpy(&val, &js->mem[off], sizeof(val)); return val;
}

static bool is_arr_off(struct js *js, jsoff_t off) { 
  return (loadoff(js, off) & ARRMASK) != 0; 
}

static jsoff_t vstrlen(struct js *js, jsval_t v) { 
  return offtolen(loadoff(js, (jsoff_t) vdata(v))); 
}

static inline jsval_t loadval(struct js *js, jsoff_t off) { 
  return *(jsval_t *)(&js->mem[off]);
}

static jsval_t upper(struct js *js, jsval_t scope) { 
  return mkval(T_OBJ, loadoff(js, (jsoff_t) (vdata(scope) + sizeof(jsoff_t)))); 
}

#define EXPECT(_tok, ...)                                    \
  if (next(js) != _tok) {                                    \
    __VA_ARGS__;                                             \
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "parse error"); \
  } else js->consumed = 1

#define EXPECT_IDENT(...) \
  if (!is_valid_param_name(next(js))) {                              \
    __VA_ARGS__;                                                     \
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "identifier expected"); \
  } else js->consumed = 1

static bool is_digit(int c);
static bool is_proxy(struct js *js, jsval_t obj);
static bool bigint_is_zero(struct js *js, jsval_t v);

static bool streq(const char *buf, size_t len, const char *p, size_t n);
static bool is_this_loop_continue_target(int depth_at_entry);
static bool code_has_function_decl(const char *code, size_t len);
static bool parse_func_params(struct js *js, uint8_t *flags, int *out_count);
static bool try_dynamic_setter(struct js *js, jsval_t obj, const char *key, size_t key_len, jsval_t value);

static size_t strbigint(struct js *js, jsval_t value, char *buf, size_t len);
static size_t tostr(struct js *js, jsval_t value, char *buf, size_t len);
static size_t strpromise(struct js *js, jsval_t value, char *buf, size_t len);
static size_t js_to_pcre2_pattern(const char *src, size_t src_len, char *dst, size_t dst_size);

static jsval_t js_stmt_impl(struct js *js);
static jsval_t js_expr(struct js *js);
static jsval_t js_call_valueOf(struct js *js, jsval_t value);
static jsval_t js_call_toString(struct js *js, jsval_t value);
static jsval_t js_eval_slice(struct js *js, jsoff_t off, jsoff_t len);
static jsval_t js_eval_str(struct js *js, const char *code, jsoff_t len);
static jsval_t js_stmt(struct js *js);
static jsval_t js_assignment(struct js *js);
static jsval_t js_arrow_func(struct js *js, jsoff_t params_start, jsoff_t params_end, bool is_async);
static jsval_t js_async_arrow_paren(struct js *js);
static jsval_t js_var_decl(struct js *js);

static jsval_t do_op(struct js *, uint8_t op, jsval_t l, jsval_t r);
static jsval_t do_instanceof(struct js *js, jsval_t l, jsval_t r);
static jsval_t do_in(struct js *js, jsval_t l, jsval_t r);

static inline bool is_slot_prop(jsoff_t header);
static inline jsoff_t next_prop(jsoff_t header);

static jsval_t js_import_stmt(struct js *js);
static jsval_t js_export_stmt(struct js *js);
static jsval_t builtin_Object(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_promise_then(struct js *js, jsval_t *args, int nargs);

static jsval_t proxy_get(struct js *js, jsval_t proxy, const char *key, size_t key_len);
static jsval_t proxy_set(struct js *js, jsval_t proxy, const char *key, size_t key_len, jsval_t value);
static jsval_t proxy_has(struct js *js, jsval_t proxy, const char *key, size_t key_len);
static jsval_t proxy_delete(struct js *js, jsval_t proxy, const char *key, size_t key_len);

static inline bool push_this(jsval_t this_value);
static inline jsval_t pop_this(void);

static jsval_t get_prototype_for_type(struct js *js, uint8_t type);
static jsval_t get_ctor_proto(struct js *js, const char *name, size_t len);

static jsval_t setprop(struct js *js, jsval_t obj, jsval_t k, jsval_t v);
static jsoff_t lkp_interned(struct js *js, jsval_t obj, const char *search_intern, size_t len);

static descriptor_entry_t *lookup_descriptor(jsoff_t obj_off, const char *key, size_t klen);
static const char *bigint_digits(struct js *js, jsval_t v, size_t *len);

typedef struct { jsval_t handle; bool is_new; } ctor_t;

static ctor_t get_constructor(struct js *js, const char *name, size_t len) {
  ctor_t ctor;
  
  ctor.handle = get_ctor_proto(js, name, len);
  ctor.is_new = (vtype(js->new_target) != T_UNDEF);
  
  return ctor;
}

static jsval_t unwrap_primitive(struct js *js, jsval_t val) {
  if (vtype(val) != T_OBJ) return val;
  jsval_t prim = get_slot(js, val, SLOT_PRIMITIVE);
  if (vtype(prim) == T_UNDEF) return val;
  return prim;
}

static jsval_t to_string_val(struct js *js, jsval_t val) {
  uint8_t t = vtype(val);
  if (t == T_STR) return val;
  if (t == T_OBJ) {
    jsval_t prim = get_slot(js, val, SLOT_PRIMITIVE);
    if (vtype(prim) == T_STR) return prim;
  }
  return js_call_toString(js, val);
}

bool js_truthy(struct js *js, jsval_t v) {
  static const void *dispatch[] = {
    [T_OBJ]    = &&l_true,
    [T_FUNC]   = &&l_true,
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

static inline size_t uint_to_str(char *buf, size_t bufsize, uint64_t val) {
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

#define MAX_STRINGIFY_DEPTH 64
#define MAX_MULTIREF_OBJS 128

static jsval_t stringify_stack[MAX_STRINGIFY_DEPTH];
static int stringify_depth = 0;
static int stringify_indent = 0;

static jsval_t multiref_objs[MAX_MULTIREF_OBJS];
static int multiref_ids[MAX_MULTIREF_OBJS];
static int multiref_count = 0;
static int multiref_next_id = 0;

static void scan_refs(struct js *js, jsval_t value);

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

static void scan_obj_refs(struct js *js, jsval_t obj) {
  if (is_on_stack(obj)) {
    mark_multiref(obj);
    return;
  }
  
  if (stringify_depth >= MAX_STRINGIFY_DEPTH) return;
  stringify_stack[stringify_depth++] = obj;
  
  jsoff_t next = loadoff(js, (jsoff_t) vdata(obj)) & ~(3U | FLAGMASK);
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

static void scan_arr_refs(struct js *js, jsval_t obj) {
  if (is_on_stack(obj)) {
    mark_multiref(obj);
    return;
  }
  
  if (stringify_depth >= MAX_STRINGIFY_DEPTH) return;
  stringify_stack[stringify_depth++] = obj;
  
  jsoff_t next = loadoff(js, (jsoff_t) vdata(obj)) & ~(3U | FLAGMASK);
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

static void scan_func_refs(struct js *js, jsval_t value) {
  jsval_t func_obj = mkval(T_OBJ, vdata(value));
  
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

static void scan_refs(struct js *js, jsval_t value) {
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

static inline jsoff_t get_prop_koff(struct js *js, jsoff_t prop) {
  return loadoff(js, prop + (jsoff_t) sizeof(prop));
}

static void get_prop_key(struct js *js, jsoff_t prop, const char **key, jsoff_t *klen) {
  jsoff_t koff = get_prop_koff(js, prop);
  *klen = offtolen(loadoff(js, koff));
  *key = (char *) &js->mem[koff + sizeof(koff)];
}

static jsval_t get_prop_val(struct js *js, jsoff_t prop) {
  jsoff_t koff = get_prop_koff(js, prop);
  return loadval(js, prop + (jsoff_t) (sizeof(prop) + sizeof(koff)));
}

const char *get_str_prop(struct js *js, jsval_t obj, const char *key, jsoff_t klen, jsoff_t *out_len) {
  jsoff_t off = lkp(js, obj, key, klen);
  if (off <= 0) return NULL;
  jsval_t v = resolveprop(js, mkval(T_PROP, off));
  if (vtype(v) != T_STR) return NULL;
  return (const char *)&js->mem[vstr(js, v, out_len)];
}

static bool is_small_array(struct js *js, jsval_t obj, int *elem_count) {
  int count = 0;
  bool has_nested = false;
  jsoff_t next = loadoff(js, (jsoff_t) vdata(obj)) & ~(3U | FLAGMASK);
  jsoff_t length = 0;
  jsoff_t scan = next;
  
  while (scan < js->brk && scan != 0) {
    const char *key; jsoff_t klen;
    get_prop_key(js, scan, &key, &klen);
    if (streq(key, klen, "length", 6)) {
      jsval_t val = get_prop_val(js, scan);
      if (vtype(val) == T_NUM) length = (jsoff_t) tod(val);
      break;
    }
    scan = loadoff(js, scan) & ~(3U | FLAGMASK);
  }
  
  for (jsoff_t i = 0; i < length; i++) {
    char idx[16];
    snprintf(idx, sizeof(idx), "%u", (unsigned) i);
    jsoff_t idxlen = (jsoff_t) strlen(idx);
    jsoff_t prop = next;
    jsval_t val = js_mkundef();
    bool found = false;
    
    while (prop < js->brk && prop != 0) {
      const char *key; jsoff_t klen;
      get_prop_key(js, prop, &key, &klen);
      if (streq(key, klen, idx, idxlen)) {
        val = get_prop_val(js, prop);
        found = true;
        break;
      }
      prop = loadoff(js, prop) & ~(3U | FLAGMASK);
    }
    
    if (found) {
      uint8_t t = vtype(val);
      if (t == T_OBJ || t == T_ARR || t == T_FUNC) has_nested = true;
      count++;
    } else count++;
  }
  
  if (elem_count) *elem_count = count;
  return count <= 4 && !has_nested;
}

static bool is_array_index(const char *key, jsoff_t klen) {
  if (klen == 0) return false;
  for (jsoff_t i = 0; i < klen; i++) {
    if (key[i] < '0' || key[i] > '9') return false;
  }
  return true;
}

static jsoff_t get_array_length(struct js *js, jsval_t arr) {
  jsoff_t off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  if (!off) return 0;
  jsval_t val = resolveprop(js, mkval(T_PROP, off));
  return vtype(val) == T_NUM ? (jsoff_t) tod(val) : 0;
}

static jsval_t get_obj_ctor(struct js *js, jsval_t obj) {
  jsval_t ctor = get_slot(js, obj, SLOT_CTOR);
  if (vtype(ctor) == T_FUNC) return ctor;
  jsval_t proto = get_slot(js, obj, SLOT_PROTO);
  if (vtype(proto) != T_OBJ) return js_mkundef();
  jsoff_t off = lkp_interned(js, proto, INTERN_CONSTRUCTOR, 11);
  return off ? resolveprop(js, mkval(T_PROP, off)) : js_mkundef();
}

static const char *get_func_name(struct js *js, jsval_t func, jsoff_t *out_len) {
  if (vtype(func) != T_FUNC) return NULL;
  jsoff_t off = lkp(js, mkval(T_OBJ, vdata(func)), "name", 4);
  if (!off) return NULL;
  jsval_t name = resolveprop(js, mkval(T_PROP, off));
  if (vtype(name) != T_STR) return NULL;
  jsoff_t str_off = vstr(js, name, out_len);
  return (const char *) &js->mem[str_off];
}

static const char *get_class_name(struct js *js, jsval_t obj, jsoff_t *out_len, const char *skip) {
  const char *name = get_func_name(js, get_obj_ctor(js, obj), out_len);
  if (!name) return NULL;
  if (skip && *out_len == (jsoff_t)strlen(skip) && memcmp(name, skip, *out_len) == 0) return NULL;
  return name;
}

static size_t strarr(struct js *js, jsval_t obj, char *buf, size_t len) {
  int ref = get_circular_ref(obj);
  if (ref) return ref > 0 ? (size_t) snprintf(buf, len, "[Circular *%d]", ref) : cpy(buf, len, "[Circular]", 10);
  
  push_stringify(obj);
  jsoff_t first = loadoff(js, (jsoff_t) vdata(obj)) & ~(3U | FLAGMASK);
  jsoff_t length = get_array_length(js, obj);
  
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
  
  for (jsoff_t i = 0; i < length; i++) {
    if (i > 0) n += cpy(buf + n, REMAIN(n, len), inline_mode ? ", " : ",\n", 2);
    if (!inline_mode) n += add_indent(buf + n, REMAIN(n, len), stringify_indent);
    
    char idx[16];
    snprintf(idx, sizeof(idx), "%u", (unsigned) i);
    jsoff_t idxlen = (jsoff_t) strlen(idx);
    
    bool found = false;
    jsval_t val = js_mkundef();
    for (jsoff_t p = first; p < js->brk && p != 0; p = next_prop(loadoff(js, p))) {
      const char *key; jsoff_t klen;
      get_prop_key(js, p, &key, &klen);
      if (streq(key, klen, idx, idxlen)) {
        val = get_prop_val(js, p);
        found = true; break;
      }
    }
    n += found ? tostr(js, val, buf + n, REMAIN(n, len)) : cpy(buf + n, REMAIN(n, len), "undefined", 9);
  }
  
  for (jsoff_t p = first; p < js->brk && p != 0; p = next_prop(loadoff(js, p))) {
    jsoff_t header = loadoff(js, p);
    if (is_slot_prop(header)) continue;
    
    const char *key; jsoff_t klen;
    get_prop_key(js, p, &key, &klen);
    if (streq(key, klen, "length", 6) || is_array_index(key, klen)) continue;
    
    n += cpy(buf + n, REMAIN(n, len), inline_mode ? ", " : ",\n", 2);
    if (!inline_mode) n += add_indent(buf + n, REMAIN(n, len), stringify_indent);
    n += cpy(buf + n, REMAIN(n, len), key, klen);
    n += cpy(buf + n, REMAIN(n, len), ": ", 2);
    n += tostr(js, get_prop_val(js, p), buf + n, REMAIN(n, len));
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

static size_t array_to_string(struct js *js, jsval_t obj, char *buf, size_t len) {
  if (is_circular(obj)) return cpy(buf, len, "", 0);
  
  push_stringify(obj);
  size_t n = 0;
  jsoff_t next = loadoff(js, (jsoff_t) vdata(obj)) & ~(3U | FLAGMASK);
  jsoff_t length = 0;
  jsoff_t scan = next;
  
  while (scan < js->brk && scan != 0) {
    const char *key; jsoff_t klen;
    get_prop_key(js, scan, &key, &klen);
    if (streq(key, klen, "length", 6)) {
      jsval_t val = get_prop_val(js, scan);
      if (vtype(val) == T_NUM) length = (jsoff_t) tod(val);
      break;
    }
    scan = loadoff(js, scan) & ~(3U | FLAGMASK);
  }
  
  for (jsoff_t i = 0; i < length; i++) {
    if (i > 0) n += cpy(buf + n, REMAIN(n, len), ",", 1);
    char idx[16];
    snprintf(idx, sizeof(idx), "%u", (unsigned) i);
    jsoff_t idxlen = (jsoff_t) strlen(idx);
    jsoff_t prop = next;
    jsval_t val = js_mkundef();
    bool found = false;
    
    while (prop < js->brk && prop != 0) {
      const char *key; jsoff_t klen;
      get_prop_key(js, prop, &key, &klen);
      if (streq(key, klen, idx, idxlen)) {
        val = get_prop_val(js, prop);
        found = true;
        break;
      }
      prop = loadoff(js, prop) & ~(3U | FLAGMASK);
    }
    
    if (found) {
      uint8_t vt = vtype(val);
      if (vt == T_STR) {
        jsoff_t slen, soff = vstr(js, val, &slen);
        n += cpy(buf + n, REMAIN(n, len), (const char *)&js->mem[soff], slen);
      } else if (vt != T_UNDEF && vt != T_NULL) n += tostr(js, val, buf + n, REMAIN(n, len));
    }
  }
  
  pop_stringify();
  return n;
}

static size_t strdate(struct js *js, jsval_t obj, char *buf, size_t len) {
  jsval_t time_val = js_get_slot(js, obj, SLOT_DATA);
  if (vtype(time_val) != T_NUM) return cpy(buf, len, "Invalid Date", 12);
  
  double timestamp_ms = tod(time_val);
  time_t timestamp_sec = (time_t)(timestamp_ms / 1000.0);
  struct tm *tm_local = localtime(&timestamp_sec);
  
  if (!tm_local) return cpy(buf, len, "Invalid Date", 12);
  
  char date_part[64];
  strftime(date_part, sizeof(date_part), "%a %b %d %Y %H:%M:%S", tm_local);
  
  time_t now = timestamp_sec;
  struct tm *gm = gmtime(&now);
  struct tm local_copy = *tm_local;
  time_t local_time = mktime(&local_copy);
  time_t gmt_time = mktime(gm);
  long offset_sec = (long)difftime(local_time, gmt_time);
  int offset_hours = (int)(offset_sec / 3600);
  int offset_mins = (int)(labs(offset_sec) % 3600) / 60;
  
  char tz_name[64];
  strftime(tz_name, sizeof(tz_name), "%Z", tm_local);
  
  return (size_t) snprintf(buf, len, "%s GMT%+03d%02d (%s)", date_part, offset_hours, offset_mins, tz_name);
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

static size_t strkey(struct js *js, jsval_t value, char *buf, size_t len) {
  jsoff_t slen, off = vstr(js, value, &slen);
  const char *str = (const char *) &js->mem[off];
  
  const char *sym_desc = get_symbol_description_from_key(str, slen);
  if (sym_desc) {
    size_t n = 0;
    n += cpy(buf + n, REMAIN(n, len), "[", 1);
    n += cpy(buf + n, REMAIN(n, len), sym_desc, strlen(sym_desc));
    n += cpy(buf + n, REMAIN(n, len), "]", 1);
    return n;
  }
  
  if (is_valid_identifier(str, slen)) {
    return cpy(buf, len, str, slen);
  }
  return strstring(js, value, buf, len);
}

static bool is_small_object(struct js *js, jsval_t obj, int *prop_count) {
  int count = 0;
  bool has_nested = false;
  
  jsoff_t obj_off = (jsoff_t)vdata(obj);
  jsoff_t next = loadoff(js, obj_off) & ~(3U | FLAGMASK);
  
  while (next < js->brk && next != 0) {
    jsoff_t header = loadoff(js, next);
    if (is_slot_prop(header)) { next = next_prop(header); continue; }
    
    const char *key; jsoff_t klen;
    get_prop_key(js, next, &key, &klen);
    const char *tag_sym_key = get_toStringTag_sym_key();
    bool should_hide = streq(key, klen, STR_PROTO, STR_PROTO_LEN) || streq(key, klen, tag_sym_key, strlen(tag_sym_key));
    
    if (!should_hide) {
      descriptor_entry_t *desc = lookup_descriptor(obj_off, key, klen);
      if (desc && !desc->enumerable) should_hide = true;
    }
    
    if (!should_hide) {
      jsval_t val = get_prop_val(js, next);
      uint8_t t = vtype(val);
      if (t == T_OBJ || t == T_ARR || t == T_FUNC) has_nested = true;
      count++;
    }
    
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
static size_t strobj(struct js *js, jsval_t obj, char *buf, size_t len) {
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
  
  const char *tostring_tag_key = get_toStringTag_sym_key();
  jsoff_t tag_off = lkp_proto(js, obj, tostring_tag_key, strlen(tostring_tag_key));
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
  jsoff_t next = loadoff(js, (jsoff_t) vdata(obj)) & ~(3U | FLAGMASK);
  bool first = true;
  
  jsoff_t obj_off = (jsoff_t)vdata(obj);
  int prop_capacity = 64;
  
  jsoff_t *prop_offsets = malloc(prop_capacity * sizeof(jsoff_t));
  int num_props = 0;
  
  while (next < js->brk && next != 0) {
    jsoff_t header = loadoff(js, next);
    if (is_slot_prop(header)) { next = next_prop(header); continue; }
    
    jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
    jsoff_t klen = offtolen(loadoff(js, koff));
    
    const char *key = (char *) &js->mem[koff + sizeof(koff)];
    const char *tag_sym_key = get_toStringTag_sym_key();
    bool should_hide = streq(key, klen, STR_PROTO, STR_PROTO_LEN) || streq(key, klen, tag_sym_key, strlen(tag_sym_key));
    
    if (!should_hide) {
      descriptor_entry_t *desc = lookup_descriptor(obj_off, key, klen);
      if (desc && !desc->enumerable) should_hide = true;
    }
    
    if (!should_hide) {
      if (num_props >= prop_capacity) {
        prop_capacity *= 2;
        prop_offsets = realloc(prop_offsets, prop_capacity * sizeof(jsoff_t));
      }
      prop_offsets[num_props++] = next;
    }
    next = next_prop(header);
  }
  
  for (int i = num_props - 1; i >= 0; i--) {
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

jsoff_t vstr(struct js *js, jsval_t value, jsoff_t *len) {
  jsoff_t off = (jsoff_t) vdata(value);
  if (len) *len = offtolen(loadoff(js, off));
  return (jsoff_t) (off + sizeof(off));
}

static size_t strstring(struct js *js, jsval_t value, char *buf, size_t len) {
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
  
  interned_string_t *entry = (interned_string_t *)ant_calloc(sizeof(interned_string_t) + len + 1);
  if (!entry) return NULL;
  
  entry->str = (char *)(entry + 1);
  memcpy(entry->str, str, len);
  entry->str[len] = '\0';
  entry->len = len;
  entry->hash = h;
  entry->next = intern_buckets[bucket];
  intern_buckets[bucket] = entry;
  
  return entry->str;
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

static size_t strfunc(struct js *js, jsval_t value, char *buf, size_t len) {
  jsoff_t name_len = 0;
  const char *name = get_func_name(js, value, &name_len);
  
  jsval_t func_obj = mkval(T_OBJ, vdata(value));
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

static size_t tostr(struct js *js, jsval_t value, char *buf, size_t len) {
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
    
    case T_SYMBOL: {
      const char *desc = js_sym_desc(js, value);
      if (desc) return (size_t) snprintf(buf, len, "Symbol(%s)", desc);
      return ANT_COPY(buf, len, "Symbol()");
    }
    
    case T_PROP:    return (size_t) snprintf(buf, len, "PROP@%lu", (unsigned long) vdata(value)); 
    default:        return (size_t) snprintf(buf, len, "VTYPE%d", vtype(value));
  }
}

static char *tostr_alloc(struct js *js, jsval_t value) {
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

jsval_t js_tostring_val(struct js *js, jsval_t value) {
  uint8_t t = vtype(value);
  char *buf; size_t len, buflen;
  
  static const void *jump_table[] = {
    [T_OBJ] = &&L_OBJ, [T_PROP] = &&L_DEFAULT, [T_STR] = &&L_STR,
    [T_UNDEF] = &&L_UNDEF, [T_NULL] = &&L_NULL, [T_NUM] = &&L_NUM,
    [T_BOOL] = &&L_BOOL, [T_FUNC] = &&L_OBJ, [T_CODEREF] = &&L_DEFAULT,
    [T_CFUNC] = &&L_DEFAULT, [T_ERR] = &&L_DEFAULT, [T_ARR] = &&L_OBJ,
    [T_PROMISE] = &&L_DEFAULT, [T_TYPEDARRAY] = &&L_DEFAULT,
    [T_BIGINT] = &&L_BIGINT, [T_PROPREF] = &&L_DEFAULT,
    [T_SYMBOL] = &&L_DEFAULT, [T_GENERATOR] = &&L_DEFAULT, [T_FFI] = &&L_DEFAULT
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

const char *js_str(struct js *js, jsval_t value) {
  if (is_err(value)) return js->errmsg;
  
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

static bool js_try_grow_memory(struct js *js, size_t needed) {
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

static inline bool js_has_space(struct js *js, size_t size) {
  return js->brk + size <= js->size;
}

static bool js_ensure_space(struct js *js, size_t size) {
  if (js_has_space(js, size)) return true;
  if (js_try_grow_memory(js, size) && js_has_space(js, size)) return true;

  js->needs_gc = true;

  if (js_has_space(js, size)) return true;
  if (js_try_grow_memory(js, size) && js_has_space(js, size)) return true;

  return false;
}

static void js_track_allocation(struct js *js, size_t size) {
  js->brk += (jsoff_t) size;
  js->gc_alloc_since += (jsoff_t) size;
  
  jsoff_t threshold = js->brk / 2;
  if (threshold < 4 * 1024 * 1024) threshold = 4 * 1024 * 1024;
  if (js->gc_alloc_since > threshold) js->needs_gc = true;
}

static inline jsoff_t js_alloc(struct js *js, size_t size) {
  size = align64((jsoff_t) size);
  if (!js_ensure_space(js, size)) return ~(jsoff_t) 0;

  jsoff_t ofs = js->brk;
  js_track_allocation(js, size);
  
  return ofs;
}

static jsval_t mkentity(struct js *js, jsoff_t b, const void *buf, size_t len) {
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

jsval_t js_mkstr(struct js *js, const void *ptr, size_t len) {
  jsoff_t n = (jsoff_t) (len + 1);
  return mkentity(js, (jsoff_t) ((n << 3) | T_STR), ptr, n);
}

jsval_t js_mkbigint(struct js *js, const char *digits, size_t len, bool negative) {
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

static bool bigint_IsNegative(struct js *js, jsval_t v) {
  jsoff_t ofs = (jsoff_t) vdata(v);
  return js->mem[ofs + sizeof(jsoff_t)] == 1;
}

static const char *bigint_digits(struct js *js, jsval_t v, size_t *len) {
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

static jsval_t bigint_add(struct js *js, jsval_t a, jsval_t b) {
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

static jsval_t bigint_sub(struct js *js, jsval_t a, jsval_t b) {
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

static jsval_t bigint_mul(struct js *js, jsval_t a, jsval_t b) {
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

static jsval_t bigint_div(struct js *js, jsval_t a, jsval_t b) {
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

static jsval_t bigint_mod(struct js *js, jsval_t a, jsval_t b) {
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

static jsval_t bigint_neg(struct js *js, jsval_t a) {
  size_t len;
  const char *digits = bigint_digits(js, a, &len);
  bool neg = bigint_IsNegative(js, a);
  if (len == 1 && digits[0] == '0') return js_mkbigint(js, digits, len, false);
  return js_mkbigint(js, digits, len, !neg);
}

static jsval_t bigint_exp(struct js *js, jsval_t base, jsval_t exp) {
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

static int bigint_compare(struct js *js, jsval_t a, jsval_t b) {
  bool aneg = bigint_IsNegative(js, a), bneg = bigint_IsNegative(js, b);
  size_t alen, blen;
  const char *ad = bigint_digits(js, a, &alen), *bd = bigint_digits(js, b, &blen);
  if (aneg && !bneg) return -1;
  if (!aneg && bneg) return 1;
  int cmp = bigint_cmp_abs(ad, alen, bd, blen);
  return aneg ? -cmp : cmp;
}

static bool bigint_is_zero(struct js *js, jsval_t v) {
  size_t len;
  const char *digits = bigint_digits(js, v, &len);
  return len == 1 && digits[0] == '0';
}

static size_t strbigint(struct js *js, jsval_t value, char *buf, size_t len) {
  bool neg = bigint_IsNegative(js, value);
  size_t dlen;
  const char *digits = bigint_digits(js, value, &dlen);
  size_t n = 0;
  if (neg) n += cpy(buf + n, REMAIN(n, len), "-", 1);
  n += cpy(buf + n, REMAIN(n, len), digits, dlen);
  return n;
}

static jsval_t builtin_BigInt(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t builtin_BigInt_asIntN(struct js *js, jsval_t *args, int nargs) {
  (void)js; (void)args; (void)nargs;
  return js_mkerr(js, "BigInt.asIntN not implemented");
}

static jsval_t builtin_BigInt_asUintN(struct js *js, jsval_t *args, int nargs) {
  (void)js; (void)args; (void)nargs;
  return js_mkerr(js, "BigInt.asUintN not implemented");
}

static jsval_t builtin_bigint_toString(struct js *js, jsval_t *args, int nargs) {
  jsval_t val = js->this_val;
  if (vtype(val) != T_BIGINT) return js_mkerr(js, "toString called on non-BigInt");
  
  int radix = 10;
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    radix = (int)tod(args[0]);
    if (radix < 2 || radix > 36) {
      return js_mkerr(js, "radix must be between 2 and 36");
    }
  }
  
  bool neg = bigint_IsNegative(js, val);
  size_t dlen;
  const char *digits = bigint_digits(js, val, &dlen);
  
  if (radix == 10) {
    size_t buflen = dlen + 2;
    char *buf = (char *)ant_calloc(buflen);
    if (!buf) return js_mkerr(js, "oom");
    size_t n = 0;
    if (neg) buf[n++] = '-';
    memcpy(buf + n, digits, dlen);
    n += dlen;
    jsval_t ret = js_mkstr(js, buf, n);
    free(buf);
    return ret;
  }
  
  size_t result_cap = dlen * 4 + 16;
  char *result = (char *)ant_calloc(result_cap);
  if (!result) return js_mkerr(js, "oom");
  size_t rpos = result_cap - 1;
  result[rpos] = '\0';
  
  char *num = (char *)ant_calloc(dlen + 1);
  if (!num) { free(result); return js_mkerr(js, "oom"); }
  memcpy(num, digits, dlen);
  num[dlen] = '\0';
  size_t numlen = dlen;
  
  while (numlen > 0 && !(numlen == 1 && num[0] == '0')) {
    int remainder = 0;
    for (size_t i = 0; i < numlen; i++) {
      int d = remainder * 10 + (num[i] - '0');
      num[i] = (char)('0' + (d / radix));
      remainder = d % radix;
    }
    size_t start = 0;
    while (start < numlen - 1 && num[start] == '0') start++;
    memmove(num, num + start, numlen - start + 1);
    numlen -= start;
    if (numlen == 1 && num[0] == '0') numlen = 0;
    if (rpos == 0) {
      size_t new_cap = result_cap * 2;
      char *new_result = (char *)ant_calloc(new_cap);
      if (!new_result) { free(num); free(result); return js_mkerr(js, "oom"); }
      size_t used = result_cap - rpos;
      memcpy(new_result + new_cap - used, result + rpos, used);
      free(result);
      result = new_result;
      rpos = new_cap - used;
      result_cap = new_cap;
    }
    rpos--;
    result[rpos] = (char)(remainder < 10 ? '0' + remainder : 'a' + (remainder - 10));
  }
  
  free(num);
  
  if (rpos == result_cap - 1) {
    result[--rpos] = '0';
  }
  
  if (neg) result[--rpos] = '-';
  
  jsval_t ret = js_mkstr(js, result + rpos, result_cap - 1 - rpos);
  free(result);
  return ret;
}

static jsval_t mkobj(struct js *js, jsoff_t parent) {
  jsoff_t buf[2] = { parent, 0 };
  return mkentity(js, 0 | T_OBJ, buf, sizeof(buf));
}

static jsval_t mkarr(struct js *js) {
  jsval_t arr = mkobj(js, 0);
  jsoff_t off = (jsoff_t) vdata(arr);
  jsoff_t header = loadoff(js, off);
  
  saveoff(js, off, header | ARRMASK);
  jsval_t array_proto = get_ctor_proto(js, "Array", 5);
  if (vtype(array_proto) == T_OBJ) set_proto(js, arr, array_proto);
  
  jsval_t arr_val = mkval(T_ARR, vdata(arr));
  js_set_descriptor(js, arr_val, "length", 6, JS_DESC_W);
  
  return arr_val;
}

jsval_t js_mkarr(struct js *js) { 
  return mkarr(js); 
}

jsval_t js_newobj(struct js *js) {
  jsval_t obj = mkobj(js, 0);
  jsval_t proto = get_ctor_proto(js, "Object", 6);
  if (vtype(proto) == T_OBJ) set_proto(js, obj, proto);
  return obj;
}

jsoff_t js_arr_len(struct js *js, jsval_t arr) {
  if (vtype(arr) != T_ARR) return 0;
  jsoff_t max_idx = 0;
  bool found_length_prop = false;
  jsoff_t length_prop_val = 0;
  jsoff_t scan = loadoff(js, (jsoff_t) vdata(arr)) & ~(3U | FLAGMASK);
  while (scan < js->brk && scan != 0) {
    const char *key; jsoff_t klen;
    get_prop_key(js, scan, &key, &klen);
    if (streq(key, klen, "length", 6)) {
      jsval_t val = get_prop_val(js, scan);
      if (vtype(val) == T_NUM) {
        found_length_prop = true;
        length_prop_val = (jsoff_t) tod(val);
      }
    } else if (klen > 0 && key[0] >= '0' && key[0] <= '9') {
      char *endptr;
      unsigned long idx = strtoul(key, &endptr, 10);
      if (endptr == key + klen && idx + 1 > max_idx) max_idx = (jsoff_t)(idx + 1);
    }
    scan = loadoff(js, scan) & ~(3U | FLAGMASK);
  }
  if (found_length_prop) return length_prop_val;
  return max_idx;
}

jsval_t js_arr_get(struct js *js, jsval_t arr, jsoff_t idx) {
  if (vtype(arr) != T_ARR) return js_mkundef();
  char idxstr[16];
  size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
  jsoff_t prop = loadoff(js, (jsoff_t) vdata(arr)) & ~(3U | FLAGMASK);
  while (prop < js->brk && prop != 0) {
    const char *key; jsoff_t klen;
    get_prop_key(js, prop, &key, &klen);
    if (streq(key, klen, idxstr, idxlen)) return get_prop_val(js, prop);
    prop = loadoff(js, prop) & ~(3U | FLAGMASK);
  }
  return js_mkundef();
}

static inline bool is_const_prop(struct js *js, jsoff_t propoff) {
  jsoff_t v = loadoff(js, propoff);
  return (v & CONSTMASK) != 0;
}

static inline bool is_nonconfig_prop(struct js *js, jsoff_t propoff) {
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

static void invalidate_prop_cache(struct js *js, jsoff_t obj_off, jsoff_t prop_off) {
  jsoff_t koff = loadoff(js, prop_off + sizeof(jsoff_t));
  jsoff_t klen = (loadoff(js, koff) >> 3) - 1;
  
  const char *key = (char *)&js->mem[koff + sizeof(jsoff_t)];
  const char *interned = intern_string(key, klen);
  if (!interned) return;
  
  uint32_t cache_slot = (((uintptr_t)interned >> 3) ^ obj_off) & (ANT_LIMIT_SIZE_CACHE - 1);
  intern_prop_cache_entry_t *ce = &intern_prop_cache[cache_slot];
  
  if (ce->obj_off == obj_off && ce->intern_ptr == interned) {
    ce->obj_off = 0; ce->intern_ptr = NULL;
    ce->prop_off = 0; ce->tail = 0;
  }
}

static jsval_t mkprop(struct js *js, jsval_t obj, jsval_t k, jsval_t v, jsoff_t flags) {
  jsoff_t koff = (jsoff_t) vdata(k);
  jsoff_t head = (jsoff_t) vdata(obj);
  char buf[sizeof(koff) + sizeof(v)];
  
  jsoff_t header = loadoff(js, head);
  jsoff_t first_prop = header & ~(3U | FLAGMASK);
  jsoff_t tail = loadoff(js, head + sizeof(jsoff_t) + sizeof(jsoff_t));
  
  memcpy(buf, &koff, sizeof(koff));
  memcpy(buf + sizeof(koff), &v, sizeof(v));
  
  jsoff_t klen = (loadoff(js, koff) >> 3) - 1;
  const char *p = (char *) &js->mem[koff + sizeof(koff)];
  (void)intern_string(p, klen);
  
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

static inline jsval_t mkprop_fast(struct js *js, jsval_t obj, jsval_t k, jsval_t v, jsoff_t flags) {
  jsoff_t koff = (jsoff_t) vdata(k);
  jsoff_t head = (jsoff_t) vdata(obj);
  char buf[sizeof(koff) + sizeof(v)];
  
  jsoff_t header = loadoff(js, head);
  jsoff_t first_prop = header & ~(3U | FLAGMASK);
  jsoff_t tail = loadoff(js, head + sizeof(jsoff_t) + sizeof(jsoff_t));
  
  memcpy(buf, &koff, sizeof(koff));
  memcpy(buf + sizeof(koff), &v, sizeof(v));
  
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

jsval_t js_mkprop_fast(struct js *js, jsval_t obj, const char *key, size_t len, jsval_t v) {
  jsval_t k = js_mkstr(js, key, len);
  if (is_err(k)) return k;
  return mkprop_fast(js, obj, k, v, 0);
}

jsoff_t js_mkprop_fast_off(struct js *js, jsval_t obj, const char *key, size_t len, jsval_t v) {
  jsval_t k = js_mkstr(js, key, len);
  if (is_err(k)) return 0;
  jsoff_t prop_off = js->brk;
  mkprop_fast(js, obj, k, v, 0);
  return prop_off + sizeof(jsoff_t) * 2;
}

void js_saveval(struct js *js, jsoff_t off, jsval_t v) { saveval(js, off, v); }

static jsval_t mkslot(struct js *js, jsval_t obj, internal_slot_t slot, jsval_t v) {
  jsoff_t head = (jsoff_t) vdata(obj);
  char buf[sizeof(jsoff_t) + sizeof(v)];
  
  jsoff_t header = loadoff(js, head);
  jsoff_t first_prop = header & ~(3U | FLAGMASK);
  jsoff_t tail = loadoff(js, head + sizeof(jsoff_t) + sizeof(jsoff_t));
  
  jsoff_t slot_key = (jsoff_t)slot;
  memcpy(buf, &slot_key, sizeof(slot_key));
  memcpy(buf + sizeof(slot_key), &v, sizeof(v));
  
  jsoff_t new_prop_off = js->brk;
  jsval_t prop = mkentity(js, 0 | T_PROP | SLOTMASK, buf, sizeof(buf));
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

static jsoff_t search_slot(struct js *js, jsval_t obj, internal_slot_t slot) {
  jsoff_t off = (jsoff_t) vdata(obj);
  if (off >= js->brk) return 0;
  jsoff_t next = loadoff(js, off) & ~(3U | FLAGMASK);
  jsoff_t header, koff;

check:
  if (next == 0 || next >= js->brk) return 0;
  header = loadoff(js, next);
  if ((header & SLOTMASK) == 0) goto advance;
  koff = loadoff(js, next + sizeof(jsoff_t));
  if (koff == (jsoff_t)slot) return next;
advance:
  next = header & ~(3U | FLAGMASK);
  goto check;
}

static void set_slot(struct js *js, jsval_t obj, internal_slot_t slot, jsval_t val) {
  jsoff_t existing = search_slot(js, obj, slot);
  if (existing > 0) {
    saveval(js, existing + sizeof(jsoff_t) * 2, val);
  } else mkslot(js, obj, slot, val);
}

static jsval_t get_slot(struct js *js, jsval_t obj, internal_slot_t slot) {
  jsoff_t off = search_slot(js, obj, slot);
  if (off == 0) return js_mkundef();
  return loadval(js, off + sizeof(jsoff_t) * 2);
}

static void set_func_code_ptr(struct js *js, jsval_t func_obj, const char *code, size_t len) {
  set_slot(js, func_obj, SLOT_CODE, mkval(T_CFUNC, (size_t)code));
  set_slot(js, func_obj, SLOT_CODE_LEN, tov((double)len));
}

static void set_func_code(struct js *js, jsval_t func_obj, const char *code, size_t len) {
  const char *arena_code = code_arena_alloc(code, len);
  if (!arena_code) return;
  set_func_code_ptr(js, func_obj, arena_code, len);
  
  if (!code_has_function_decl(code, len)) set_slot(js, func_obj, SLOT_NO_FUNC_DECLS, js_true);
  if (!memmem(code, len, "var", 3)) return;
  
  size_t vars_buf_len;
  char *vars = OXC_get_func_hoisted_vars(code, len, &vars_buf_len);
  
  if (vars) {
    set_slot(js, func_obj, SLOT_HOISTED_VARS, mkval(T_CFUNC, (size_t)vars));
    set_slot(js, func_obj, SLOT_HOISTED_VARS_LEN, tov((double)vars_buf_len));
  }
}

static const char *get_func_code(struct js *js, jsval_t func_obj, jsoff_t *len) {
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

static double js_to_number(struct js *js, jsval_t arg) {
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

static jsval_t setup_func_prototype(struct js *js, jsval_t func) {
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
  
  res = setprop(js, func, prototype_key, proto_obj);
  if (is_err(res)) return res;
  js_set_descriptor(js, func, "prototype", 9, JS_DESC_W);
  
  return js_mkundef();
}

static void infer_func_name(struct js *js, jsval_t func, const char *name, size_t len) {
  jsval_t func_obj = mkval(T_OBJ, vdata(func));
  if (vtype(get_slot(js, func_obj, SLOT_NAME)) != T_UNDEF) return;
  jsval_t name_val = js_mkstr(js, name, len);
  set_slot(js, func_obj, SLOT_NAME, name_val);
  setprop(js, func_obj, js_mkstr(js, "name", 4), name_val);
}

static jsval_t validate_array_length(struct js *js, jsval_t v) {
  if (vtype(v) != T_NUM) {
    return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid array length");
  }
  double d = tod(v);
  if (d < 0 || d != (uint32_t)d || d >= 4294967296.0) {
    return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid array length");
  }
  return js_mkundef();
}

jsval_t js_setprop(struct js *js, jsval_t obj, jsval_t k, jsval_t v) {
  jsoff_t koff = (jsoff_t) vdata(k);
  jsoff_t klen = offtolen(loadoff(js, koff));
  const char *key = (char *) &js->mem[koff + sizeof(jsoff_t)];
  
  if (vtype(obj) == T_ARR && streq(key, klen, "length", 6)) {
    jsval_t err = validate_array_length(js, v);
    if (is_err(err)) return err;
  }
  
  if (is_proxy(js, obj)) {
    jsval_t result = proxy_set(js, obj, key, klen, v);
    if (is_err(result)) return result;
    return v;
  }
  
  if (try_dynamic_setter(js, obj, key, klen, v)) {
    return v;
  }
  
  jsoff_t existing = lkp(js, obj, key, klen);
  
  {
    jsoff_t obj_off = (jsoff_t)vdata(obj);
    descriptor_entry_t *desc = lookup_descriptor(obj_off, key, klen);
    
    if (!desc) goto no_descriptor;
    
    if (desc->has_setter) {
      jsval_t setter = desc->setter;
      uint8_t setter_type = vtype(setter);
      if (setter_type == T_FUNC || setter_type == T_CFUNC) {
        js_parse_state_t saved;
        JS_SAVE_STATE(js, saved);
        uint8_t saved_flags = js->flags;
        jsoff_t saved_toff = js->toff;
        jsoff_t saved_tlen = js->tlen;
        
        jsval_t saved_this = js->this_val;
        js->this_val = obj;
        push_this(obj);
        jsval_t result = call_js_with_args(js, setter, &v, 1);
        pop_this();
        js->this_val = saved_this;
        
        JS_RESTORE_STATE(js, saved);
        js->flags = saved_flags;
        js->toff = saved_toff;
        js->tlen = saved_tlen;
        
        if (is_err(result)) return result;
        return v;
      }
    }
    
    if (desc->has_getter && !desc->has_setter) {
      if (js->flags & F_STRICT) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot set property which has only a getter");
      return v;
    }
    
    if (existing <= 0) goto no_descriptor;
    
    if (!desc->writable) {
      if (js->flags & F_STRICT) return js_mkerr(js, "assignment to read-only property");
      return mkval(T_PROP, existing);
    }
  }
  
no_descriptor:
  if (existing <= 0) goto create_new;
  
  if (is_const_prop(js, existing)) {
    if (js->flags & F_STRICT) return js_mkerr(js, "assignment to constant");
    return mkval(T_PROP, existing);
  }

  saveval(js, existing + sizeof(jsoff_t) * 2, v);
  if (vtype(obj) != T_ARR || klen == 0 || key[0] < '0' || key[0] > '9') goto done_update;
  
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
  
  jsval_t len_key = js_mkstr(js, "length", 6);
  jsval_t new_len = tov((double)(update_idx + 1));
  if (len_off != 0) {
    saveval(js, len_off + sizeof(jsoff_t) * 2, new_len);
  } else mkprop(js, obj, len_key, new_len, 0);

done_update:
  return mkval(T_PROP, existing);

create_new:
  if (js_truthy(js, get_slot(js, obj, SLOT_FROZEN))) {
    if (js->flags & F_STRICT) return js_mkerr(js, "cannot add property to frozen object");
    return js_mkundef();
  }
  
  if (js_truthy(js, get_slot(js, obj, SLOT_SEALED))) {
    if (js->flags & F_STRICT) return js_mkerr(js, "cannot add property to sealed object");
    return js_mkundef();
  }
  
  jsval_t ext_slot = get_slot(js, obj, SLOT_EXTENSIBLE);
  if (vtype(ext_slot) != T_UNDEF && !js_truthy(js, ext_slot)) {
    if (js->flags & F_STRICT) return js_mkerr(js, "cannot add property to non-extensible object");
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
    jsval_t inner_len_key = js_mkstr(js, "length", 6);
    jsval_t inner_new_len = tov((double)(idx + 1));
    if (inner_len_off != 0) {
      saveval(js, inner_len_off + sizeof(jsoff_t) * 2, inner_new_len);
    } else mkprop(js, obj, inner_len_key, inner_new_len, 0);
  }
  
  return result;
}

static inline jsval_t setprop(struct js *js, jsval_t obj, jsval_t k, jsval_t v) {
  return js_setprop(js, obj, k, v);
}

static inline void esm_export_binding(struct js *js, const char *exported, size_t exported_len, jsval_t value) {
  jsval_t export_key = js_mkstr(js, exported, exported_len);
  setprop(js, js->module_ns, export_key, value);
  if (exported_len == 7 && strncmp(exported, "default", 7) == 0) js_set_slot(js, js->module_ns, SLOT_DEFAULT, value);
}

static inline jsval_t setprop_cstr(struct js *js, jsval_t obj, const char *key, size_t len, jsval_t v) {
  jsval_t k = js_mkstr(js, key, len);
  if (is_err(k)) return k;
  return mkprop(js, obj, k, v, 0);
}

static jsval_t setprop_interned(struct js *js, jsval_t obj, const char *key, size_t len, jsval_t v) {
  jsval_t k = js_mkstr(js, key, len);
  if (is_err(k)) return k;
  return js_setprop(js, obj, k, v);
}

jsval_t js_setprop_nonconfigurable(struct js *js, jsval_t obj, const char *key, size_t keylen, jsval_t v) {
  jsval_t k = js_mkstr(js, key, keylen);
  if (is_err(k)) return k;
  jsval_t result = setprop(js, obj, k, v);
  if (is_err(result)) return result;
  
  js_set_descriptor(js, obj, key, keylen, JS_DESC_W);
  return result;
}

jsval_t js_mksym(struct js *js, const char *desc) {
  uint64_t id = ++js->sym_counter;
  jsoff_t desc_off = 0;
  if (desc && *desc) {
    jsval_t desc_str = js_mkstr(js, desc, strlen(desc));
    desc_off = (jsoff_t)vdata(desc_str);
  }
  uint64_t payload = ((id & PROPREF_PAYLOAD) << PROPREF_KEY_SHIFT) | (desc_off & PROPREF_PAYLOAD);
  return mkval(T_SYMBOL, payload);
}

static inline uint64_t sym_get_id(jsval_t v) {
  return (vdata(v) >> PROPREF_KEY_SHIFT) & PROPREF_PAYLOAD;
}

static inline jsoff_t sym_get_desc_off(jsval_t v) {
  return vdata(v) & PROPREF_PAYLOAD;
}

static const char *sym_get_desc(struct js *js, jsval_t v) {
  jsoff_t off = sym_get_desc_off(v);
  if (off == 0) return NULL;
  return (const char *)&js->mem[off + sizeof(jsoff_t)];
}

uint64_t inline js_sym_id(jsval_t sym) {
  return sym_get_id(sym);
}

jsval_t js_mksym_for(struct js *js, const char *key) {
  (void)js;
  const char *interned = intern_string(key, strlen(key));
  uint64_t id = (uint64_t)(uintptr_t)interned;
  return mkval(T_SYMBOL, id | (1ULL << 47));
}

const char *js_sym_key(jsval_t sym) {
  if (vtype(sym) != T_SYMBOL) return NULL;
  uint64_t data = vdata(sym);
  if (!(data & (1ULL << 47))) return NULL;
  return (const char *)(uintptr_t)(data & ~(1ULL << 47));
}

const inline char *js_sym_desc(struct js *js, jsval_t sym) {
  return sym_get_desc(js, sym);
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

static int is_unicode_space(const unsigned char *p, jsoff_t remaining, bool *is_line_term) {
  if (is_line_term) *is_line_term = false;
  if (p[0] < 0x80) return 0;
  if (remaining >= 2 && p[0] == 0xC2 && p[1] == 0xA0) return 2;
  if (remaining >= 3 && p[0] == 0xE2 && p[1] == 0x80) {
    if (p[2] >= 0x80 && p[2] <= 0x8A) return 3;
    if (p[2] == 0xAF) return 3;
    if (p[2] == 0xA8) { if (is_line_term) *is_line_term = true; return 3; }
    if (p[2] == 0xA9) { if (is_line_term) *is_line_term = true; return 3; }
  }
  if (remaining >= 3 && p[0] == 0xE1 && p[1] == 0x9A && p[2] == 0x80) return 3;
  if (remaining >= 3 && p[0] == 0xE2 && p[1] == 0x81 && p[2] == 0x9F) return 3;
  if (remaining >= 3 && p[0] == 0xE3 && p[1] == 0x80 && p[2] == 0x80) return 3;
  if (remaining >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) return 3;
  return 0;
}

enum { C_0 = 0, C_SPC, C_NL, C_SL, C_HI };

static const uint8_t cc[128] = {
  0,0,0,0,0,0,0,0,0,C_SPC,C_NL,C_SPC,C_SPC,C_SPC,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  C_SPC,0,0,0,0,0,0,0,0,0,0,0,0,0,0,C_SL,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

static jsoff_t skiptonext(const char *code, jsoff_t len, jsoff_t n, bool *nl) {
  static const void *D[] = { &&L0, &&LS, &&LN, &&LSL, &&LH };
  bool saw_nl = false;
  unsigned char c;

  const char *p = code + n;
  const char *end = code + len;

  if (__builtin_expect(p == code && end - p >= 2 && p[0] == '#' && p[1] == '!', 0)) {
    for (p += 2; p < end && *p != '\n'; p++);
    if (p < end) { saw_nl = true; p++; }
  }

  if (__builtin_expect(p >= end, 0)) goto L0;
  c = (unsigned char)*p;
  goto *D[c & 0x80 ? C_HI : cc[c]];

LS:
  p++;
  if (__builtin_expect(p >= end, 0)) goto L0;
  c = (unsigned char)*p;
  goto *D[c & 0x80 ? C_HI : cc[c]];

LN:
  saw_nl = true;
  p++;
  if (__builtin_expect(p >= end, 0)) goto L0;
  c = (unsigned char)*p;
  goto *D[c & 0x80 ? C_HI : cc[c]];

LSL:
  if (p + 1 >= end) goto L0;
  if (p[1] == '/') {
    for (p += 2; p < end && *p != '\n'; p++);
    if (p < end) { saw_nl = true; p++; }
    if (__builtin_expect(p >= end, 0)) goto L0;
    c = (unsigned char)*p;
    goto *D[c & 0x80 ? C_HI : cc[c]];
  }
  if (p[1] == '*') {
    for (p += 2; p + 1 < end; p++) {
      if (*p == '*' && p[1] == '/') { p += 2; break; }
      if (*p == '\n') saw_nl = true;
    }
    if (__builtin_expect(p >= end, 0)) goto L0;
    c = (unsigned char)*p;
    goto *D[c & 0x80 ? C_HI : cc[c]];
  }
  goto L0;

LH: {
  bool lt;
  int u = is_unicode_space((const unsigned char *)p, (jsoff_t)(end - p), &lt);
  if (u > 0) {
    if (lt) saw_nl = true;
    p += u;
    if (__builtin_expect(p >= end, 0)) goto L0;
    c = (unsigned char)*p;
    goto *D[c & 0x80 ? C_HI : cc[c]];
  }
}

L0:
  if (nl) *nl = saw_nl;
  return (jsoff_t)(p - code);
}

#define K(s, t) if (len == sizeof(s)-1 && !memcmp(buf, s, sizeof(s)-1)) return t
#define M(s)   (len == sizeof(s)-1 && !memcmp(buf, s, sizeof(s)-1))

static uint8_t parsekeyword(const char *buf, size_t len) {
  switch (buf[0]) {
    case 'a':
      K("as", TOK_AS);
      K("async", TOK_ASYNC);
      K("await", TOK_AWAIT);
      break;
    case 'b':
      K("break", TOK_BREAK);
      break;
    case 'c':
      K("case", TOK_CASE);
      K("catch", TOK_CATCH);
      K("class", TOK_CLASS);
      K("const", TOK_CONST);
      K("continue", TOK_CONTINUE);
      break;
    case 'd':
      K("do", TOK_DO);
      K("default", TOK_DEFAULT);
      K("delete", TOK_DELETE);
      K("debugger", TOK_DEBUGGER);
      break;
    case 'e':
      K("else", TOK_ELSE);
      K("export", TOK_EXPORT);
      break;
    case 'f':
      K("for", TOK_FOR);
      K("from", TOK_FROM);
      K("false", TOK_FALSE);
      K("finally", TOK_FINALLY);
      K("function", TOK_FUNC);
      break;
    case 'g':
      K("globalThis", TOK_GLOBAL_THIS);
      break;
    case 'i':
      K("if", TOK_IF);
      K("in", TOK_IN);
      K("import", TOK_IMPORT);
      K("instanceof", TOK_INSTANCEOF);
      break;
    case 'l':
      K("let", TOK_LET);
      break;
    case 'n':
      K("new", TOK_NEW);
      K("null", TOK_NULL);
      break;
    case 'o':
      K("of", TOK_OF);
      break;
    case 'r':
      K("return", TOK_RETURN);
      break;
    case 's':
      K("super", TOK_SUPER);
      K("static", TOK_STATIC);
      K("switch", TOK_SWITCH);
      break;
    case 't':
      K("try", TOK_TRY);
      K("this", TOK_THIS);
      K("true", TOK_TRUE);
      K("throw", TOK_THROW);
      K("typeof", TOK_TYPEOF);
      break;
    case 'u':
      K("undefined", TOK_UNDEF);
      break;
    case 'v':
      K("var", TOK_VAR);
      K("void", TOK_VOID);
      break;
    case 'w':
      K("while", TOK_WHILE);
      K("with", TOK_WITH);
      K("window", TOK_WINDOW);
      break;
    case 'y':
      K("yield", TOK_YIELD);
      break;
  }
  return TOK_IDENTIFIER;
}

static bool is_strict_reserved(const char *buf, size_t len) {
  switch (buf[0]) {
    case 'i':
      if M("interface") return true;
      if M("implements") return true;
      break;
    case 'l':
      if M("let") return true;
      break;
    case 'p':
      if M("private") return true;
      if M("package") return true;
      if M("public") return true;
      if M("protected") return true;
      break;
    case 's':
      if M("static") return true;
      break;
    case 'y':
      if M("yield") return true;
      break;
  }
  return false;
}

#undef K
#undef M

static inline bool streq(const char *buf, size_t len, const char *s, size_t n) {
  return len == n && !memcmp(buf, s, n);
}

static inline bool is_strict_restricted(const char *buf, size_t len) {
  if (len == 4) return streq(buf, len, "eval", 4);
  if (len == 9) return streq(buf, len, "arguments", 9);
  return false;
}

#define CHAR_DIGIT  0x01
#define CHAR_XDIGIT 0x02
#define CHAR_ALPHA  0x04
#define CHAR_IDENT  0x08
#define CHAR_IDENT1 0x10
#define CHAR_WS     0x20
#define CHAR_OCTAL  0x40

static const uint8_t char_type[256] = {
  ['\t'] = CHAR_WS, ['\n'] = CHAR_WS, ['\r'] = CHAR_WS, [' '] = CHAR_WS,
  ['0'] = CHAR_DIGIT | CHAR_XDIGIT | CHAR_IDENT | CHAR_OCTAL,
  ['1'] = CHAR_DIGIT | CHAR_XDIGIT | CHAR_IDENT | CHAR_OCTAL,
  ['2'] = CHAR_DIGIT | CHAR_XDIGIT | CHAR_IDENT | CHAR_OCTAL,
  ['3'] = CHAR_DIGIT | CHAR_XDIGIT | CHAR_IDENT | CHAR_OCTAL,
  ['4'] = CHAR_DIGIT | CHAR_XDIGIT | CHAR_IDENT | CHAR_OCTAL,
  ['5'] = CHAR_DIGIT | CHAR_XDIGIT | CHAR_IDENT | CHAR_OCTAL,
  ['6'] = CHAR_DIGIT | CHAR_XDIGIT | CHAR_IDENT | CHAR_OCTAL,
  ['7'] = CHAR_DIGIT | CHAR_XDIGIT | CHAR_IDENT | CHAR_OCTAL,
  ['8'] = CHAR_DIGIT | CHAR_XDIGIT | CHAR_IDENT,
  ['9'] = CHAR_DIGIT | CHAR_XDIGIT | CHAR_IDENT,
  ['A'] = CHAR_XDIGIT | CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['B'] = CHAR_XDIGIT | CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['C'] = CHAR_XDIGIT | CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['D'] = CHAR_XDIGIT | CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['E'] = CHAR_XDIGIT | CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['F'] = CHAR_XDIGIT | CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['a'] = CHAR_XDIGIT | CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['b'] = CHAR_XDIGIT | CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['c'] = CHAR_XDIGIT | CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['d'] = CHAR_XDIGIT | CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['e'] = CHAR_XDIGIT | CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['f'] = CHAR_XDIGIT | CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['G'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['H'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['I'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['J'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['K'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['L'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['M'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['N'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['O'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['P'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['Q'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['R'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['S'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['T'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['U'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['V'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['W'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['X'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['Y'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['Z'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['g'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['h'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['i'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['j'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['k'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['l'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['m'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['n'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['o'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['p'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['q'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['r'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['s'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['t'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['u'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['v'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['w'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['x'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['y'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['z'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['_'] = CHAR_IDENT | CHAR_IDENT1,
  ['$'] = CHAR_IDENT | CHAR_IDENT1,
};

#define IS_DIGIT(c)  (char_type[(uint8_t)(c)] & CHAR_DIGIT)
#define IS_XDIGIT(c) (char_type[(uint8_t)(c)] & CHAR_XDIGIT)
#define IS_IDENT(c)  (char_type[(uint8_t)(c)] & CHAR_IDENT)
#define IS_IDENT1(c) (char_type[(uint8_t)(c)] & CHAR_IDENT1)
#define IS_OCTAL(c)  (char_type[(uint8_t)(c)] & CHAR_OCTAL)

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

static inline bool is_function_keyword(const char *code, jsoff_t pos, jsoff_t end) {
  if (pos + 8 > end) return false;
  uint64_t word;
  memcpy(&word, code + pos, 8);
  
  if (word != 0x6e6f6974636e7566ULL) return false;
  return (pos + 8 >= end) || !IS_IDENT(code[pos + 8]);
}

static inline bool is_async_function(const char *code, jsoff_t pos, jsoff_t end) {
  if (pos + 5 > end) return false;
  
  uint32_t word4;
  memcpy(&word4, code + pos, 4);
  
  if (word4 != 0x6e797361U || code[pos + 4] != 'c') return false;
  if (pos + 5 < end && IS_IDENT(code[pos + 5])) return false;
  
  jsoff_t scan = pos + 5;
  while (scan < end && (
    code[scan] == ' ' || 
    code[scan] == '\t' || 
    code[scan] == '\n' || 
    code[scan] == '\r'
  )) scan++;
  
  return is_function_keyword(code, scan, end);
}

static bool code_has_function_decl(const char *code, size_t len) {
  if (!memmem(code, len, "function", 8)) return false;
  
  size_t pos = 0;
  
  int target_depth = 0;
  if (len > 0 && code[0] == '(') target_depth = 1;
  int depth = 0;
  
  while (pos < len) {
    uint8_t c = (uint8_t)code[pos];
    
    if (c == '"' || c == '\'' || c == '`') {
      uint8_t quote = c;
      pos++;
      while (pos < len && (uint8_t)code[pos] != quote) {
        if (code[pos] == '\\' && pos + 1 < len) pos++;
        pos++;
      }
      if (pos < len) pos++;
      continue;
    }
    
    if (c == '/' && pos + 1 < len) {
      if (code[pos + 1] == '/') {
        pos += 2;
        while (pos < len && code[pos] != '\n') pos++;
        continue;
      }
      if (code[pos + 1] == '*') {
        pos += 2;
        while (pos + 1 < len && !(code[pos] == '*' && code[pos + 1] == '/')) pos++;
        if (pos + 1 < len) pos += 2;
        continue;
      }
    }
    
    if (c == '{') { depth++; pos++; continue; }
    if (c == '}') {
      if (depth <= target_depth) break;
      depth--; pos++; continue;
    }
    
    if (depth == target_depth) {
      if (c == 'f' && is_function_keyword(code, (jsoff_t)pos, (jsoff_t)len)) return true;
      if (c == 'a' && is_async_function(code, (jsoff_t)pos, (jsoff_t)len)) return true;
    }
    
    pos++;
  }
  
  return false;
}

static const uint8_t single_char_tok[128] = {
  ['('] = TOK_LPAREN,
  [')'] = TOK_RPAREN,
  ['{'] = TOK_LBRACE,
  ['}'] = TOK_RBRACE,
  ['['] = TOK_LBRACKET,
  [']'] = TOK_RBRACKET,
  [';'] = TOK_SEMICOLON,
  [','] = TOK_COMMA,
  [':'] = TOK_COLON,
  ['~'] = TOK_TILDA,
  ['#'] = TOK_HASH,
};

static bool is_space(int c) { 
  if (c < 0 || c >= 256) return false;
  return (char_type[(uint8_t)c] & CHAR_WS) != 0;
}

static bool is_digit(int c) { 
  if (c < 0 || c >= 256) return false;
  return (char_type[(uint8_t)c] & CHAR_DIGIT) != 0;
}

static bool is_xdigit(int c) { 
  if (c < 0 || c >= 256) return false;
  return (char_type[(uint8_t)c] & CHAR_XDIGIT) != 0;
}

static bool is_alpha(int c) { 
  if (c < 0 || c >= 256) return false;
  return (char_type[(uint8_t)c] & CHAR_ALPHA) != 0;
}

static bool is_ident_begin(int c) { 
  if (c < 0) return false;
  if (c < 128) return (char_type[(uint8_t)c] & CHAR_IDENT1) != 0;
  return (c & 0x80) != 0;
}

static bool is_ident_continue(int c) { 
  if (c < 0) return false;
  if (c < 128) return (char_type[(uint8_t)c] & (CHAR_IDENT | CHAR_IDENT1)) != 0;
  return (c & 0x80) != 0;
}

static int parse_unicode_escape(const char *buf, jsoff_t len, jsoff_t pos, uint32_t *codepoint) {
  if (pos + 5 >= len) return 0;
  if (buf[pos] != '\\' || buf[pos + 1] != 'u') return 0;
  
  uint32_t cp = 0;
  for (int i = 0; i < 4; i++) {
    int c = (unsigned char)buf[pos + 2 + i];
    if (!is_xdigit(c)) return 0;
    cp <<= 4;
    cp |= (c <= '9') ? (c - '0') : ((c | 0x20) - 'a' + 10);
  }
  *codepoint = cp;
  return 6;
}

static bool is_unicode_ident_begin(uint32_t cp) {
  if (cp < 128) return (char_type[(uint8_t)cp] & CHAR_IDENT1) != 0;
  return true;
}

static bool is_unicode_ident_continue(uint32_t cp) {
  if (cp < 128) return (char_type[(uint8_t)cp] & (CHAR_IDENT | CHAR_IDENT1)) != 0;
  return true;
}

static size_t decode_ident_escapes(const char *src, size_t srclen, char *dst, size_t dstlen) {
  size_t si = 0, di = 0;
  while (si < srclen && di + 4 < dstlen) {
    uint32_t cp;
    int el = parse_unicode_escape(src, (jsoff_t)srclen, (jsoff_t)si, &cp);
    if (el > 0) {
      di += utf8_encode(cp, dst + di);
      si += el;
    } else dst[di++] = src[si++];
  }
  dst[di] = '\0';
  return di;
}

static bool has_unicode_escape(const char *src, size_t len) {
  if (len < 6) return false;
  const char *end = src + len - 5;
  const char *p = src;
  while ((p = memchr(p, '\\', end - p)) != NULL) {
    if (p[1] == 'u') return true;
    p++;
  }
  return false;
}

static jsval_t js_mkstr_ident(struct js *js, const char *src, size_t srclen) {
  if (!has_unicode_escape(src, srclen)) {
    return js_mkstr(js, src, srclen);
  }
  char decoded[256];
  size_t decoded_len = decode_ident_escapes(src, srclen, decoded, sizeof(decoded));
  return js_mkstr(js, decoded, decoded_len);
}


static uint8_t parseident(const char *buf, jsoff_t len, jsoff_t *tlen) {
  if (len == 0) return TOK_ERR;
  
  unsigned char c = (unsigned char)buf[0];
  jsoff_t i = 0;
  
  if (c < 128 && c != '\\' && is_ident_begin(c)) {
    i = 1;
    while (i < len) {
      c = (unsigned char)buf[i];
      if (c >= 128 || c == '\\') goto slow_path_continue;
      if (!is_ident_continue(c)) break;
      i++;
    }
    *tlen = i;
    return parsekeyword(buf, i);
  }
  
  if (c == '\\') {
    uint32_t first_cp;
    int esc_len = parse_unicode_escape(buf, len, 0, &first_cp);
    if (esc_len <= 0 || !is_unicode_ident_begin(first_cp)) return TOK_ERR;
    *tlen = esc_len;
    goto slow_path_loop;
  }
  
  if (c >= 128) {
    if ((c & 0xC0) == 0x80) return TOK_ERR;
    int ws_len = is_unicode_space((const unsigned char *)buf, len, NULL);
    if (ws_len > 0) return TOK_ERR;
    i = 1;
    while (i < len && ((unsigned char)buf[i] & 0xC0) == 0x80) i++;
    *tlen = i;
    goto slow_path_loop;
  }
  
  return TOK_ERR;

slow_path_continue:
  *tlen = i;
  
slow_path_loop:;
  int has_escapes = (buf[0] == '\\');
  
  while (*tlen < len) {
    c = (unsigned char)buf[*tlen];
    
    if (c == '\\') {
      uint32_t cp;
      int el = parse_unicode_escape(buf, len, *tlen, &cp);
      if (el <= 0 || !is_unicode_ident_continue(cp)) break;
      *tlen += el;
      has_escapes = 1;
    } else if (c < 128) {
      if (!is_ident_continue(c)) break;
      (*tlen)++;
    } else {
      if ((c & 0xC0) == 0x80) break;
      int ws_len = is_unicode_space((const unsigned char *)&buf[*tlen], len - *tlen, NULL);
      if (ws_len > 0) break;
      (*tlen)++;
      while (*tlen < len && ((unsigned char)buf[*tlen] & 0xC0) == 0x80) (*tlen)++;
    }
  }
  
  if (has_escapes) {
    char decoded[256];
    size_t decoded_len = decode_ident_escapes(buf, *tlen, decoded, sizeof(decoded));
    return parsekeyword(decoded, decoded_len);
  }
  
  return parsekeyword(buf, *tlen);
}

static inline jsoff_t parse_decimal(const char *buf, jsoff_t maxlen, double *out) {
  uint64_t int_part = 0, frac_part = 0;
  int frac_digits = 0;
  jsoff_t i = 0;

  while (i < maxlen && (IS_DIGIT(buf[i]) || buf[i] == '_')) {
    if (buf[i] != '_') int_part = int_part * 10 + (buf[i] - '0');
    i++;
  }

  if (i < maxlen && buf[i] == '.') {
    i++;
    while (i < maxlen && (IS_DIGIT(buf[i]) || buf[i] == '_')) {
      if (buf[i] != '_') { frac_part = frac_part * 10 + (buf[i] - '0'); frac_digits++; }
      i++;
    }
  }

  static const double neg_pow10[] = {
    1e0,1e-1,1e-2,1e-3,1e-4,1e-5,1e-6,1e-7,1e-8,1e-9,1e-10,
    1e-11,1e-12,1e-13,1e-14,1e-15,1e-16,1e-17,1e-18,1e-19,1e-20
  };
  
  static const double pos_pow10[] = {
    1e0,1e1,1e2,1e3,1e4,1e5,1e6,1e7,1e8,1e9,1e10,
    1e11,1e12,1e13,1e14,1e15,1e16,1e17,1e18,1e19,1e20
  };

  double val = (double)int_part;
  if (frac_digits > 0) {
    val += (frac_digits <= 20) 
      ? (double)frac_part * neg_pow10[frac_digits] 
      : (double)frac_part * pow(10.0, -frac_digits);
  }

  if (i < maxlen && (buf[i] == 'e' || buf[i] == 'E')) {
    i++;
    int exp_sign = 1, exp_val = 0;
    if (i < maxlen && (buf[i] == '+' || buf[i] == '-')) {
      exp_sign = (buf[i] == '-') ? -1 : 1;
      i++;
    }
    while (i < maxlen && (IS_DIGIT(buf[i]) || buf[i] == '_')) {
      if (buf[i] != '_') exp_val = exp_val * 10 + (buf[i] - '0');
      i++;
    }
    if (exp_val <= 20) {
      val = (exp_sign > 0) ? val * pos_pow10[exp_val] : val * neg_pow10[exp_val];
    } else val *= pow(10.0, exp_sign * exp_val);
  }

  *out = val;
  return i;
}

static inline jsoff_t parse_binary(const char *buf, jsoff_t maxlen, double *out) {
  double val = 0;
  jsoff_t i = 2;
  while (i < maxlen && (buf[i] == '0' || buf[i] == '1' || buf[i] == '_')) {
    if (buf[i] != '_') val = val * 2 + (buf[i] - '0');
    i++;
  }
  *out = val;
  return i;
}

static inline jsoff_t parse_octal(const char *buf, jsoff_t maxlen, double *out) {
  double val = 0;
  jsoff_t i = 2;
  while (i < maxlen && (IS_OCTAL(buf[i]) || buf[i] == '_')) {
    if (buf[i] != '_') val = val * 8 + (buf[i] - '0');
    i++;
  }
  *out = val;
  return i;
}

static inline jsoff_t parse_legacy_octal(const char *buf, jsoff_t maxlen, double *out) {
  double val = 0;
  jsoff_t i = 1;
  while (i < maxlen && IS_OCTAL(buf[i])) {
      val = val * 8 + (buf[i] - '0');
      i++;
  }
  *out = val;
  return i;
}

static inline jsoff_t parse_hex(const char *buf, jsoff_t maxlen, double *out) {
  double val = 0;
  jsoff_t i = 2;
  while (i < maxlen && (IS_XDIGIT(buf[i]) || buf[i] == '_')) {
    if (buf[i] != '_') {
      int d = 
        (buf[i] >= 'a') ? (buf[i] - 'a' + 10) :
        (buf[i] >= 'A') ? (buf[i] - 'A' + 10) : (buf[i] - '0');
      val = val * 16 + d;
    } i++;
  }
  *out = val;
  return i;
}

static inline uint8_t parse_number(struct js *js, const char *buf, jsoff_t remaining) {
  double value = 0;
  jsoff_t numlen = 0;
  
  if (buf[0] == '0' && remaining > 1) {
    char c1 = buf[1] | 0x20; 
    if (c1 == 'b') {
      numlen = parse_binary(buf, remaining, &value);
    } else if (c1 == 'o') {
      numlen = parse_octal(buf, remaining, &value);
    } else if (c1 == 'x') {
      numlen = parse_hex(buf, remaining, &value);
    } else if (IS_OCTAL(buf[1])) {
        if (js->flags & F_STRICT) {
          js->tok = TOK_ERR;
          js->tlen = 1;
          return TOK_ERR;
        }
        numlen = parse_legacy_octal(buf, remaining, &value);
    } else numlen = parse_decimal(buf, remaining, &value);
  } else numlen = parse_decimal(buf, remaining, &value);
  
  js->tval = tov(value);
  if (numlen < remaining && buf[numlen] == 'n') {
    js->tok = TOK_BIGINT;
    js->tlen = numlen + 1;
  } else {
    js->tok = TOK_NUMBER;
    js->tlen = numlen;
  }
  
  return js->tok;
}

static inline uint8_t scan_string(struct js *js, const char *buf, jsoff_t rem, char quote) {
  jsoff_t i = 1;

  while (i < rem) {
    const char *p = buf + i;
    jsoff_t search_len = rem - i;

    const char *q = memchr(p, quote, search_len);
    const char *b = memchr(p, '\\', search_len);

    if (q == NULL) {
      js->tok = TOK_ERR;
      js->tlen = rem;
      return TOK_ERR;
    }

    if (b == NULL || q < b) {
      i = (jsoff_t)((q - buf) + 1);
      js->tok = TOK_STRING;
      js->tlen = i;
      return TOK_STRING;
    }

    jsoff_t esc_pos = (jsoff_t)(b - buf);
    if (esc_pos + 1 >= rem) {
      js->tok = TOK_ERR;
      js->tlen = rem;
      return TOK_ERR;
    }

    char esc_char = buf[esc_pos + 1];
    jsoff_t skip = 2;

    if (esc_char == 'x') { skip = 4; } else if (esc_char == 'u') {
      skip = (esc_pos + 2 < rem && buf[esc_pos + 2] == '{') ? 0 : 6;
      if (skip == 0) {
        jsoff_t j = esc_pos + 3;
        while (j < rem && buf[j] != '}') j++;
        skip = (j < rem) ? (j - esc_pos + 1) : (rem - esc_pos);
      }
    }

    if (esc_pos + skip > rem) {
      js->tok = TOK_ERR;
      js->tlen = rem;
      return TOK_ERR;
    }

    i = esc_pos + skip;
  }

  js->tok = TOK_ERR;
  js->tlen = rem;
  return TOK_ERR;
}

static inline jsoff_t skip_string_literal(const char *buf, jsoff_t rem, jsoff_t start, char quote) {
  jsoff_t i = start + 1;
  while (i < rem) {
    if (buf[i] == '\\') { i += 2; continue; }
    if (buf[i] == quote) { return i + 1; } i++;
  }
  return rem;
}

static inline jsoff_t skip_line_comment(const char *buf, jsoff_t rem, jsoff_t start) {
  jsoff_t i = start + 2;
  while (i < rem && buf[i] != '\n') i++;
  return i;
}

static inline jsoff_t skip_block_comment(const char *buf, jsoff_t rem, jsoff_t start) {
  jsoff_t i = start + 2;
  while (i + 1 < rem && !(buf[i] == '*' && buf[i + 1] == '/')) i++;
  return (i + 1 < rem) ? (i + 2) : rem;
}

static jsoff_t skip_template_literal(const char *buf, jsoff_t rem, jsoff_t start) {
  jsoff_t i = start + 1;
  int expr_depth = 0;

  while (i < rem) {
    char c = buf[i];

    if (c == '\\') {
      i += 2;
      continue;
    }

    if (expr_depth == 0) {
      if (c == '`') return i + 1;
      if (c == '$' && i + 1 < rem && buf[i + 1] == '{') {
        expr_depth = 1;
        i += 2;
        continue;
      } i++; continue;
    }

    if (c == '\'' || c == '"') {
      i = skip_string_literal(buf, rem, i, c);
      continue;
    }

    if (c == '`') {
      jsoff_t next = skip_template_literal(buf, rem, i);
      if (next <= i) return rem;
      i = next; continue;
    }

    if (c == '/' && i + 1 < rem) {
      if (buf[i + 1] == '/') { i = skip_line_comment(buf, rem, i); continue; }
      if (buf[i + 1] == '*') { i = skip_block_comment(buf, rem, i); continue; }
    }

    if (c == '{') { expr_depth++; i++; continue; }
    if (c == '}') { expr_depth--; i++; continue; }

    i++;
  }

  return rem;
}

static inline uint8_t scan_template(struct js *js, const char *buf, jsoff_t rem) {
  jsoff_t end = skip_template_literal(buf, rem, 0);
  if (end <= 1 || end > rem) {
    js->tok = TOK_ERR;
    js->tlen = rem;
    return TOK_ERR;
  }

  js->tok = TOK_TEMPLATE;
  js->tlen = end;
  return TOK_TEMPLATE;
}

static inline uint8_t parse_operator(struct js *js, const char *buf, jsoff_t rem) {
  #define MATCH2(c1,c2)       (rem >= 2 && buf[1] == (c2))
  #define MATCH3(c1,c2,c3)    (rem >= 3 && buf[1] == (c2) && buf[2] == (c3))
  #define MATCH4(c1,c2,c3,c4) (rem >= 4 && buf[1]==(c2) && buf[2]==(c3) && buf[3]==(c4))

  switch (buf[0]) {
  case '?':
    if (MATCH3('?','?','=')) { js->tok = TOK_NULLISH_ASSIGN; js->tlen = 3; }
    else if (MATCH2('?','?')) { js->tok = TOK_NULLISH; js->tlen = 2; }
    else if (MATCH2('?','.')) { js->tok = TOK_OPTIONAL_CHAIN; js->tlen = 2; }
    else { js->tok = TOK_Q; js->tlen = 1; }
    break;

  case '!':
    if (MATCH3('!','=','=')) { js->tok = TOK_SNE; js->tlen = 3; }
    else if (MATCH2('!','=')) { js->tok = TOK_NE; js->tlen = 2; }
    else { js->tok = TOK_NOT; js->tlen = 1; }
    break;

  case '=':
    if (MATCH3('=','=','=')) { js->tok = TOK_SEQ; js->tlen = 3; }
    else if (MATCH2('=','=')) { js->tok = TOK_EQ; js->tlen = 2; }
    else if (MATCH2('=','>')) { js->tok = TOK_ARROW; js->tlen = 2; }
    else { js->tok = TOK_ASSIGN; js->tlen = 1; }
    break;

  case '<':
    if (MATCH3('<','<','=')) { js->tok = TOK_SHL_ASSIGN; js->tlen = 3; }
    else if (MATCH2('<','<')) { js->tok = TOK_SHL; js->tlen = 2; }
    else if (MATCH2('<','=')) { js->tok = TOK_LE; js->tlen = 2; }
    else { js->tok = TOK_LT; js->tlen = 1; }
    break;

  case '>':
    if (MATCH4('>','>','>','=')) { js->tok = TOK_ZSHR_ASSIGN; js->tlen = 4; }
    else if (MATCH3('>','>','>')) { js->tok = TOK_ZSHR; js->tlen = 3; }
    else if (MATCH3('>','>','=')) { js->tok = TOK_SHR_ASSIGN; js->tlen = 3; }
    else if (MATCH2('>','>')) { js->tok = TOK_SHR; js->tlen = 2; }
    else if (MATCH2('>','=')) { js->tok = TOK_GE; js->tlen = 2; }
    else { js->tok = TOK_GT; js->tlen = 1; }
    break;

  case '&':
    if (MATCH3('&','&','=')) { js->tok = TOK_LAND_ASSIGN; js->tlen = 3; }
    else if (MATCH2('&','&')) { js->tok = TOK_LAND; js->tlen = 2; }
    else if (MATCH2('&','=')) { js->tok = TOK_AND_ASSIGN; js->tlen = 2; }
    else { js->tok = TOK_AND; js->tlen = 1; }
    break;

  case '|':
    if (MATCH3('|','|','=')) { js->tok = TOK_LOR_ASSIGN; js->tlen = 3; }
    else if (MATCH2('|','|')) { js->tok = TOK_LOR; js->tlen = 2; }
    else if (MATCH2('|','=')) { js->tok = TOK_OR_ASSIGN; js->tlen = 2; }
    else { js->tok = TOK_OR; js->tlen = 1; }
    break;

  case '+':
    if (MATCH2('+','+')) { js->tok = TOK_POSTINC; js->tlen = 2; }
    else if (MATCH2('+','=')) { js->tok = TOK_PLUS_ASSIGN; js->tlen = 2; }
    else { js->tok = TOK_PLUS; js->tlen = 1; }
    break;

  case '-':
    if (MATCH2('-','-')) { js->tok = TOK_POSTDEC; js->tlen = 2; }
    else if (MATCH2('-','=')) { js->tok = TOK_MINUS_ASSIGN; js->tlen = 2; }
    else { js->tok = TOK_MINUS; js->tlen = 1; }
    break;

  case '*':
    if (MATCH2('*','*')) { js->tok = TOK_EXP; js->tlen = 2; }
    else if (MATCH2('*','=')) { js->tok = TOK_MUL_ASSIGN; js->tlen = 2; }
    else { js->tok = TOK_MUL; js->tlen = 1; }
    break;

  case '/':
    if (MATCH2('/','=')) { js->tok = TOK_DIV_ASSIGN; js->tlen = 2; }
    else { js->tok = TOK_DIV; js->tlen = 1; }
    break;

  case '%':
    if (MATCH2('%','=')) { js->tok = TOK_REM_ASSIGN; js->tlen = 2; }
    else { js->tok = TOK_REM; js->tlen = 1; }
    break;

  case '^':
    if (MATCH2('^','=')) { js->tok = TOK_XOR_ASSIGN; js->tlen = 2; }
    else { js->tok = TOK_XOR; js->tlen = 1; }
    break;

  case '.':
    if (MATCH3('.','.', '.')) { js->tok = TOK_REST; js->tlen = 3; }
    else if (rem > 1 && IS_DIGIT(buf[1])) {
      double val;
      js->tlen = parse_decimal(buf, rem, &val);
      js->tval = tov(val);
      js->tok = TOK_NUMBER;
    }
    else { js->tok = TOK_DOT; js->tlen = 1; }
    break;

  default:
    return 0;
  }

  #undef MATCH2
  #undef MATCH3
  #undef MATCH4

  return js->tok;
}

static uint8_t next(struct js *js) {
  if (likely(js->consumed == 0)) return js->tok;

  js->consumed = 0;
  js->tok = TOK_ERR;
  js->toff = js->pos = skiptonext(js->code, js->clen, js->pos, &js->had_newline);
  js->tlen = 0;

  if (unlikely(js->toff >= js->clen)) {
    js->tok = TOK_EOF;
    return TOK_EOF;
  }

  const char *buf = js->code + js->toff;
  jsoff_t rem = js->clen - js->toff;
  uint8_t c = (uint8_t)buf[0];

  if (likely(c < 128)) {
    uint8_t simple_tok = single_char_tok[c];
    if (simple_tok != 0) {
      js->tok = simple_tok;
      js->tlen = 1;
      js->pos = js->toff + 1;
      return simple_tok;
    }
  }

  if (likely(IS_IDENT1(c))) {
    js->tok = parseident(buf, rem, &js->tlen);
    js->pos = js->toff + js->tlen;
    return js->tok;
  }

  if (IS_DIGIT(c)) {
    parse_number(js, buf, rem);
    if (js->tlen == 0) js->tlen = 1;
    js->pos = js->toff + js->tlen;
    return js->tok;
  }

  if (c == '"' || c == '\'') {
    scan_string(js, buf, rem, c);
    if (js->tlen == 0) js->tlen = 1;
    js->pos = js->toff + js->tlen;
    return js->tok;
  }

  if (c == '`') {
    scan_template(js, buf, rem);
    if (js->tlen == 0) js->tlen = 1;
    js->pos = js->toff + js->tlen;
    return js->tok;
  }

  if (parse_operator(js, buf, rem)) {
    if (js->tlen == 0) js->tlen = 1;
    js->pos = js->toff + js->tlen;
    return js->tok;
  }

  js->tok = parseident(buf, rem, &js->tlen);
  if (js->tlen == 0) js->tlen = 1;
  js->pos = js->toff + js->tlen;
  
  return js->tok;
}

static inline uint8_t lookahead(struct js *js) {
  uint8_t old = js->tok, tok = 0;
  uint8_t old_consumed = js->consumed;
  jsoff_t pos = js->pos;
  
  js->consumed = 1;
  tok = next(js);
  js->pos = pos;
  js->tok = old;
  js->consumed = old_consumed;
  
  return tok;
}

static bool is_typeof_bare_ident(struct js *js) {
  jsoff_t pos = js->pos, toff = js->toff, tlen = js->tlen;
  uint8_t tok = js->tok, consumed = js->consumed;
  bool had_newline = js->had_newline;
  
  int depth = 0;
  uint8_t t = next(js);
  while (t == TOK_LPAREN) { js->consumed = 1; t = next(js); depth++; }
  
  bool bare = (t == TOK_IDENTIFIER);
  if (bare) {
    js->consumed = 1;
    t = next(js);
    while (depth > 0 && t == TOK_RPAREN) { js->consumed = 1; t = next(js); depth--; }
    if (depth != 0 || t == TOK_DOT || t == TOK_LBRACKET || t == TOK_LPAREN || t == TOK_OPTIONAL_CHAIN) bare = false;
  }
  
  js->pos = pos; js->toff = toff; js->tlen = tlen;
  js->tok = tok; js->consumed = consumed; js->had_newline = had_newline;
  
  return bare;
}

jsval_t js_mkscope(struct js *js) {
  assert((js->flags & F_NOEXEC) == 0);
  if (global_scope_stack == NULL) utarray_new(global_scope_stack, &jsoff_icd);
  jsoff_t prev = (jsoff_t) vdata(js->scope);
  utarray_push_back(global_scope_stack, &prev);
  js->scope = mkobj(js, prev);
  return js->scope;
}

void js_delscope(struct js *js) {
  if (global_scope_stack && utarray_len(global_scope_stack) > 0) {
    jsoff_t *prev = (jsoff_t *)utarray_back(global_scope_stack);
    js->scope = mkval(T_OBJ, *prev);
    utarray_pop_back(global_scope_stack);
  } else js->scope = upper(js, js->scope);
}

static void mkscope(struct js *js) { (void)js_mkscope(js); }
static void delscope(struct js *js) { (void)js_delscope(js); }

static void for_let_push(struct js *js, const char *var_name, jsoff_t var_len, jsoff_t prop_off, jsval_t body_scope) {
  if (js->for_let_stack_len >= js->for_let_stack_cap) {
    int new_cap = js->for_let_stack_cap ? js->for_let_stack_cap * 2 : 4;
    js->for_let_stack = realloc(js->for_let_stack, new_cap * sizeof(struct for_let_ctx));
    js->for_let_stack_cap = new_cap;
  }
  js->for_let_stack[js->for_let_stack_len++] = (struct for_let_ctx){var_name, var_len, prop_off, body_scope};
}

static inline void for_let_set_body_scope(struct js *js, jsval_t body_scope) {
  if (js->for_let_stack_len > 0) js->for_let_stack[js->for_let_stack_len - 1].body_scope = body_scope;
}

static inline void for_let_pop(struct js *js) {
  if (js->for_let_stack_len > 0) js->for_let_stack_len--;
}

static inline struct for_let_ctx *for_let_current(struct js *js) {
  return js->for_let_stack_len > 0 ? &js->for_let_stack[js->for_let_stack_len - 1] : NULL;
}

static void copy_body_scope_props(struct js *js, jsval_t body_scope, jsval_t closure_scope, const char *skip_var, jsoff_t skip_len) {
  if (vtype(body_scope) != T_OBJ) return;
  jsoff_t prop_off = loadoff(js, (jsoff_t)vdata(body_scope)) & ~(3U | FLAGMASK);
  
  while (prop_off < js->brk && prop_off != 0) {
    jsoff_t header = loadoff(js, prop_off);
    if (is_slot_prop(header)) { prop_off = next_prop(header); continue; }
    
    jsoff_t koff = loadoff(js, prop_off + (jsoff_t)sizeof(prop_off));
    jsoff_t klen = offtolen(loadoff(js, koff));
    const char *key = (char *)&js->mem[koff + sizeof(koff)];
    jsval_t val = loadval(js, prop_off + (jsoff_t)(sizeof(prop_off) + sizeof(koff)));
    
    prop_off = next_prop(header);
    if (is_internal_prop(key, klen)) continue;
    if (skip_var && klen == skip_len && memcmp(key, skip_var, klen) == 0) continue;
    
    jsval_t key_str = js_mkstr(js, key, klen);
    mkprop(js, closure_scope, key_str, val, 0);
  }
}

static jsval_t for_let_capture_scope(struct js *js) {
  struct for_let_ctx *flc = for_let_current(js);
  if (!flc || flc->prop_off == 0) return js->scope;
  
  jsval_t loop_var_val = resolveprop(js, mkval(T_PROP, flc->prop_off));
  jsval_t closure_scope = js_mkscope(js);
  if (is_err(closure_scope)) return closure_scope;
  
  jsval_t var_key = js_mkstr(js, flc->var_name, flc->var_len);
  mkprop(js, closure_scope, var_key, loop_var_val, 0);
  
  if (vtype(flc->body_scope) == T_OBJ) {
    copy_body_scope_props(js, flc->body_scope, closure_scope, flc->var_name, flc->var_len);
  }
  delscope(js);
  return closure_scope;
}

static void scope_clear_props(struct js *js, jsval_t scope) {
  jsoff_t off = (jsoff_t)vdata(scope);
  jsoff_t header = loadoff(js, off);
  jsoff_t parent = loadoff(js, off + sizeof(jsoff_t));
  
  saveoff(js, off, (header & FLAGMASK) | T_OBJ);
  saveoff(js, off + sizeof(jsoff_t), parent);
  saveoff(js, off + sizeof(jsoff_t) + sizeof(jsoff_t), 0);
}

static bool block_needs_scope(struct js *js) {
  jsoff_t pos = js->pos, toff = js->toff, tlen = js->tlen;
  uint8_t tok = js->tok, consumed = js->consumed;
  bool had_newline = js->had_newline;
  
  bool needs = false;
  int depth = 1;
  js->consumed = 1;
  
  while (depth > 0) {
    uint8_t t = next(js);
    if (t == TOK_EOF) break;
    if (t == TOK_LBRACE) { depth++; js->consumed = 1; continue; }
    if (t == TOK_RBRACE) { depth--; js->consumed = 1; continue; }
    
    if (depth == 1 && (
      t == TOK_LET ||
      t == TOK_CONST ||
      t == TOK_CLASS ||
      t == TOK_FUNC
    )) { needs = true; break; }
    
    js->consumed = 1;
  }
  
  js->pos = pos; js->toff = toff; js->tlen = tlen;
  js->tok = tok; js->consumed = consumed; js->had_newline = had_newline;
  
  return needs;
}

static inline bool push_this(jsval_t this_value) {
  if (global_this_stack.depth >= global_this_stack.capacity) {
    int new_capacity = global_this_stack.capacity == 0 ? 16 : global_this_stack.capacity * 2;
    jsval_t *new_stack = (jsval_t *) realloc(global_this_stack.stack, new_capacity * sizeof(jsval_t));
    if (!new_stack) return false;
    global_this_stack.stack = new_stack;
    global_this_stack.capacity = new_capacity;
  }
  global_this_stack.stack[global_this_stack.depth++] = this_value;
  return true;
}

static inline jsval_t pop_this() {
  if (global_this_stack.depth > 0) {
    return global_this_stack.stack[--global_this_stack.depth];
  }
  return js_mkundef();
}

static inline jsval_t peek_this() {
  if (global_this_stack.depth > 0) {
    return global_this_stack.stack[global_this_stack.depth - 1];
  }
  return js_mkundef();
}

static jsval_t js_func_decl(struct js *js);
static jsval_t js_func_decl_async(struct js *js);

static void hoist_function_declarations(struct js *js) {
  if (js->flags & F_NOEXEC) return;
  if (js->is_hoisting) return;
  
  if (js->skip_func_hoist) return;
  if (!code_has_function_decl(js->code + js->pos, js->clen - js->pos)) return;
  
  js->is_hoisting = true;
  js_parse_state_t saved;
  JS_SAVE_STATE(js, saved);
  jsval_t saved_scope = js->scope;
  
  int depth = 0;
  uint8_t tok, prev_tok = TOK_EOF;
  
  while ((tok = next(js)) != TOK_EOF && !(tok == TOK_RBRACE && depth == 0)) {
    if (tok == TOK_LBRACE) { depth++; prev_tok = tok; js->consumed = 1; continue; }
    if (tok == TOK_RBRACE) { depth--; prev_tok = tok; js->consumed = 1; continue; }
    if (depth > 0) { prev_tok = tok; js->consumed = 1; continue; }
    
    if (tok == TOK_EXPORT) {
      js->consumed = 1;
      uint8_t next_tok = next(js);
      if (next_tok != TOK_FUNC && next_tok != TOK_ASYNC && next_tok != TOK_DEFAULT)
        goto skip_export;
      
      int brace_depth = 0;
      while (next(js) != TOK_EOF) {
        if (js->tok == TOK_LBRACE) brace_depth++;
        else if (js->tok == TOK_RBRACE && --brace_depth <= 0) break;
        js->consumed = 1;
      }
      
      skip_export: {
        prev_tok = tok;
        continue;
      }
    }
    
    if (depth == 0 && tok == TOK_FUNC) {
      if (expr_context_tok[prev_tok]) {
        prev_tok = tok;
        js->consumed = 1;
        continue;
      }
      
      jsoff_t after_func = js->pos;
      js->consumed = 1;
      
      if (next(js) == TOK_IDENTIFIER) {
        js->pos = after_func;
        js->tok = TOK_FUNC;
        js->consumed = 1;
        js_func_decl(js);
      }
      
      prev_tok = tok;
      continue;
    }
    
    if (depth == 0 && tok == TOK_ASYNC) {
      if (expr_context_tok[prev_tok]) {
        prev_tok = tok;
        js->consumed = 1;
        continue;
      }
      
      js->consumed = 1;
      if (next(js) != TOK_FUNC) goto skip_async;
      jsoff_t func_pos = js->pos;
      js->consumed = 1;
      if (next(js) != TOK_IDENTIFIER) goto skip_async;
      js->pos = func_pos;
      js->tok = TOK_FUNC;
      js->consumed = 0;
      js_func_decl_async(js);
      
      skip_async: {
        prev_tok = tok;
        continue;
      }
    }
    
    prev_tok = tok;
    js->consumed = 1;
  }
  
  js->is_hoisting = false;
  JS_RESTORE_STATE(js, saved);
  js->scope = saved_scope;
}

static void declare_hoisted_vars(struct js *js, jsval_t var_scope, const char *var_names) {
  const char *ptr = var_names;
  while (*ptr) {
    size_t len = strlen(ptr);
    jsoff_t existing = lkp(js, var_scope, ptr, len);
    if (existing == 0) mkprop(js, var_scope, js_mkstr(js, ptr, len), js_mkundef(), 0);
    ptr += len + 1;
  }
}

static void hoist_var_declarations_from_slot(struct js *js, jsval_t var_scope, jsval_t func_obj) {
  jsval_t vars_val = get_slot(js, func_obj, SLOT_HOISTED_VARS);
  if (vtype(vars_val) != T_CFUNC) return;
  const char *var_names = (const char *)vdata(vars_val);
  if (!var_names) return;
  declare_hoisted_vars(js, var_scope, var_names);
}

static void hoist_var_declarations(struct js *js, jsval_t var_scope) {
  if (js->flags & F_NOEXEC) return;
  if (js->clen == 0) return;
  if (!memmem(js->code, js->clen, "var", 3)) return;
  
  size_t buf_len;
  char *var_names = OXC_get_hoisted_vars(js->code, (size_t)js->clen, &buf_len);
  if (!var_names) return;
  
  declare_hoisted_vars(js, var_scope, var_names);
  OXC_free_hoisted_vars(var_names, buf_len);
}

static jsval_t js_block(struct js *js, bool create_scope) {
  jsval_t res = js_mkundef();
  bool scope_created = false;
  
  if (create_scope && lookahead(js) != TOK_RBRACE && block_needs_scope(js)) {
    mkscope(js);
    scope_created = true;
  }
  
  js->consumed = 1;
  hoist_function_declarations(js);
  
  uint8_t peek;
  while ((peek = next(js)) != TOK_EOF && peek != TOK_RBRACE && !is_err(res)) {
    uint8_t t = js->tok;
    res = js_stmt(js);
    if (!is_err(res) && !is_block_tok(t) && !(js->had_newline || is_asi_ok_tok(js->tok))) {
      res = js_mkerr_typed(js, JS_ERR_SYNTAX, "; expected"); break;
    }
    if (js->flags & (F_RETURN | F_THROW)) break;
  }
  
  if (js->tok == TOK_RBRACE) js->consumed = 1;
  if (scope_created) delscope(js);
  
  return res;
}

static inline jsoff_t lkp_interned(struct js *js, jsval_t obj, const char *search_intern, size_t len) {
  jsoff_t obj_off = (jsoff_t)vdata(obj);
  jsoff_t first_prop = loadoff(js, obj_off) & ~(3U | FLAGMASK);
  jsoff_t tail = loadoff(js, obj_off + sizeof(jsoff_t) * 2);
  
  uint32_t slot = (((uintptr_t)search_intern >> 3) ^ obj_off) & (ANT_LIMIT_SIZE_CACHE - 1);
  intern_prop_cache_entry_t *ce = &intern_prop_cache[slot];
  if (ce->obj_off == obj_off && ce->intern_ptr == search_intern && ce->tail == tail) return ce->prop_off;
  
  jsoff_t off = first_prop;
  jsoff_t result = 0;
  
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
  
  ce->obj_off = obj_off;
  ce->intern_ptr = search_intern;
  ce->prop_off = result;
  ce->tail = tail;
  
  return result;
}

inline jsoff_t lkp(struct js *js, jsval_t obj, const char *buf, size_t len) {
  const char *search_intern = intern_string(buf, len);
  if (!search_intern) return 0;
  return lkp_interned(js, obj, search_intern, len);
}

static jsval_t *resolve_bound_args(struct js *js, jsval_t func_obj, jsval_t *args, int nargs, int *out_nargs) {
  *out_nargs = nargs;
  
  jsval_t bound_arr = get_slot(js, func_obj, SLOT_BOUND_ARGS);
  int bound_argc = 0;
  
  if (vtype(bound_arr) == T_ARR) {
    jsoff_t len_off = lkp_interned(js, bound_arr, INTERN_LENGTH, 6);
    if (len_off != 0) {
      jsval_t len_val = resolveprop(js, mkval(T_PROP, len_off));
      if (vtype(len_val) == T_NUM) bound_argc = (int) tod(len_val);
    }
  }
  
  if (bound_argc <= 0) return NULL;
  
  *out_nargs = bound_argc + nargs;
  jsval_t *combined = (jsval_t *)ant_calloc(sizeof(jsval_t) * (*out_nargs));
  if (!combined) return NULL;
  
  for (int i = 0; i < bound_argc; i++) {
    char idx[16];
    snprintf(idx, sizeof(idx), "%d", i);
    jsoff_t prop_off = lkp(js, bound_arr, idx, strlen(idx));
    combined[i] = (prop_off != 0) ? resolveprop(js, mkval(T_PROP, prop_off)) : js_mkundef();
  }
  for (int i = 0; i < nargs; i++) combined[bound_argc + i] = args[i];
  
  return combined;
}

static jsoff_t lkp_scope(struct js *js, jsval_t scope, const char *buf, size_t len) {
  const char *search_intern = intern_string(buf, len);
  if (!search_intern) return 0;

  jsoff_t scope_off = (jsoff_t)vdata(scope);
  jsoff_t off = loadoff(js, scope_off) & ~(3U | FLAGMASK);

  while (off < js->brk && off != 0) {
    jsoff_t header = loadoff(js, off);
    if (is_slot_prop(header)) { off = next_prop(header); continue; }

    jsoff_t koff = loadoff(js, (jsoff_t)(off + sizeof(off)));
    jsoff_t klen = (loadoff(js, koff) >> 3) - 1;
    if (klen == len) {
      const char *p = (char *)&js->mem[koff + sizeof(koff)];
      if (intern_string(p, klen) == search_intern) return off;
    }
    off = next_prop(header);
  }
  return 0;
}

static jsoff_t lkp_with_getter(struct js *js, jsval_t obj, const char *buf, size_t len, jsval_t *getter_out, bool *has_getter_out) {
  *has_getter_out = false;
  *getter_out = js_mkundef();
  
  jsval_t current = obj;
  while (vtype(current) == T_OBJ || vtype(current) == T_FUNC) {
    jsoff_t current_off = (jsoff_t)vdata(current);
    descriptor_entry_t *desc = lookup_descriptor(current_off, buf, len);
    
    if (desc && desc->has_getter) {
      *getter_out = desc->getter;
      *has_getter_out = true;
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

static jsoff_t lkp_with_setter(struct js *js, jsval_t obj, const char *buf, size_t len, jsval_t *setter_out, bool *has_setter_out) {
  *has_setter_out = false;
  *setter_out = js_mkundef();
  
  jsval_t current = obj;
  while (vtype(current) == T_OBJ || vtype(current) == T_FUNC) {
    jsoff_t current_off = (jsoff_t)vdata(current);
    descriptor_entry_t *desc = lookup_descriptor(current_off, buf, len);
    
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

static jsval_t call_proto_accessor(struct js *js, jsval_t prim, jsval_t accessor, bool has_accessor, jsval_t *arg, int arg_count, bool is_setter) {
  if (!has_accessor || (vtype(accessor) != T_FUNC && vtype(accessor) != T_CFUNC)) return js_mkundef();
  
  js_parse_state_t saved;
  JS_SAVE_STATE(js, saved);
  uint8_t saved_flags = js->flags;
  jsoff_t saved_toff = js->toff;
  jsoff_t saved_tlen = js->tlen;
  
  jsval_t saved_this = js->this_val;
  js->this_val = prim;
  push_this(prim);
  jsval_t result = call_js_with_args(js, accessor, arg, arg_count);
  pop_this();
  js->this_val = saved_this;
  
  JS_RESTORE_STATE(js, saved);
  js->flags = saved_flags;
  js->toff = saved_toff;
  js->tlen = saved_tlen;
  
  if (is_setter) return is_err(result) ? result : (arg ? *arg : js_mkundef());
  return result;
}

jsval_t js_get_proto(struct js *js, jsval_t obj) {
  uint8_t t = vtype(obj);

  if (t != T_OBJ && t != T_ARR && t != T_FUNC && t != T_PROMISE) return js_mknull();
  jsval_t as_obj = (t == T_OBJ) ? obj : mkval(T_OBJ, vdata(obj));
  
  jsval_t proto = get_slot(js, as_obj, SLOT_PROTO);
  uint8_t pt = vtype(proto);
  if (pt == T_OBJ || pt == T_ARR || pt == T_FUNC) return proto;
  
  if (t == T_FUNC || t == T_ARR || t == T_PROMISE) return get_prototype_for_type(js, t);
  return js_mknull();
}

static jsval_t get_proto(struct js *js, jsval_t obj) {
  return js_get_proto(js, obj);
}

void js_set_proto(struct js *js, jsval_t obj, jsval_t proto) {
  uint8_t t = vtype(obj);
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return;
  
  jsval_t as_obj = (t == T_OBJ) ? obj : mkval(T_OBJ, vdata(obj));
  set_slot(js, as_obj, SLOT_PROTO, proto);
}

static void set_proto(struct js *js, jsval_t obj, jsval_t proto) {
  js_set_proto(js, obj, proto);
}

jsval_t js_get_ctor_proto(struct js *js, const char *name, size_t len) {
  jsoff_t ctor_off = lkp_scope(js, js->scope, name, len);
  
  if (ctor_off == 0 && global_scope_stack) {
    unsigned int stack_len = utarray_len(global_scope_stack);
    for (int i = (int)stack_len - 1; i >= 0 && ctor_off == 0; i--) {
      jsoff_t *scope_off = (jsoff_t *)utarray_eltptr(global_scope_stack, (unsigned int)i);
      jsval_t scope = mkval(T_OBJ, *scope_off);
      ctor_off = lkp_scope(js, scope, name, len);
    }
  } else if (ctor_off == 0) {
    for (jsval_t scope = upper(js, js->scope); vdata(scope) != 0 && ctor_off == 0; scope = upper(js, scope)) {
      ctor_off = lkp_scope(js, scope, name, len);
    }
  }
  
  if (ctor_off == 0) return js_mknull();
  jsval_t ctor = resolveprop(js, mkval(T_PROP, ctor_off));
  if (vtype(ctor) != T_FUNC) return js_mknull();
  jsval_t ctor_obj = mkval(T_OBJ, vdata(ctor));
  jsoff_t proto_off = lkp_interned(js, ctor_obj, INTERN_PROTOTYPE, 9);
  
  if (proto_off == 0) return js_mknull();
  return resolveprop(js, mkval(T_PROP, proto_off));
}

static inline jsval_t get_ctor_proto(struct js *js, const char *name, size_t len) {
  return js_get_ctor_proto(js, name, len);
}

static jsval_t get_prototype_for_type(struct js *js, uint8_t type) {
  switch (type) {
    case T_STR:     return get_ctor_proto(js, "String", 6);
    case T_NUM:     return get_ctor_proto(js, "Number", 6);
    case T_BOOL:    return get_ctor_proto(js, "Boolean", 7);
    case T_ARR:     return get_ctor_proto(js, "Array", 5);
    case T_FUNC:    return get_ctor_proto(js, "Function", 8);
    case T_PROMISE: return get_ctor_proto(js, "Promise", 7);
    case T_OBJ:     return get_ctor_proto(js, "Object", 6);
    case T_BIGINT:  return get_ctor_proto(js, "BigInt", 6);
    default:        return js_mknull();
  }
}

jsoff_t lkp_proto(struct js *js, jsval_t obj, const char *key, size_t len) {
  uint8_t t = vtype(obj);
  const char *key_intern = intern_string(key, len);
  
  if (!key_intern) return 0;
  if (len == STR_PROTO_LEN && memcmp(key, STR_PROTO, STR_PROTO_LEN) == 0) return 0;
  
  jsval_t cur = obj;
  int depth = 0;
  
  while (depth < 32) {
    if (t == T_OBJ || t == T_ARR || t == T_FUNC || t == T_PROMISE) {
      jsval_t as_obj = mkval(T_OBJ, vdata(cur));
      jsoff_t off = lkp_interned(js, as_obj, key_intern, len);
      if (off != 0) return off;
      
      jsval_t proto = get_slot(js, as_obj, SLOT_PROTO);
      uint8_t pt = vtype(proto);
      if (pt == T_NULL) break;
      if (pt != T_OBJ && pt != T_ARR && pt != T_FUNC) {
        if (TYPE_FLAG(t) & T_NEEDS_PROTO_FALLBACK) {
          cur = get_prototype_for_type(js, t);
          t = vtype(cur);
          if (t == T_NULL || t == T_UNDEF) break;
          depth++; continue;
        }
        break;
      }
      cur = proto;
      t = vtype(cur);
      if (t == T_NULL || t == T_UNDEF) break;
      depth++;
    } else if (t == T_STR || t == T_NUM || t == T_BOOL || t == T_BIGINT) {
      cur = get_prototype_for_type(js, t);
      t = vtype(cur);
      if (t == T_NULL || t == T_UNDEF) break;
      depth++;
    } else if (t == T_CFUNC) {
      jsval_t func_proto = get_ctor_proto(js, "Function", 8);
      uint8_t ft = vtype(func_proto);
      if (ft == T_OBJ || ft == T_ARR || ft == T_FUNC) {
        jsoff_t off = lkp(js, mkval(T_OBJ, vdata(func_proto)), key, len);
        if (off != 0) return off;
      }
      break;
    } else {
      break;
    }
  }
  
  return 0;
}

static jsval_t try_dynamic_getter(struct js *js, jsval_t obj, const char *key, size_t key_len) {
  jsoff_t obj_off = (jsoff_t)vdata(obj);
  dynamic_accessors_t *entry = NULL;
  HASH_FIND(hh, accessor_registry, &obj_off, sizeof(jsoff_t), entry);
  if (!entry || !entry->getter) return js_mkundef();
  return entry->getter(js, obj, key, key_len);
}

static bool try_dynamic_setter(struct js *js, jsval_t obj, const char *key, size_t key_len, jsval_t value) {
  jsoff_t obj_off = (jsoff_t)vdata(obj);
  dynamic_accessors_t *entry = NULL;
  HASH_FIND(hh, accessor_registry, &obj_off, sizeof(jsoff_t), entry);
  if (!entry || !entry->setter) return false;
  return entry->setter(js, obj, key, key_len, value);
}

static jsval_t lookup(struct js *js, const char *buf, size_t len) {
  if (js->flags & F_NOEXEC) return 0;
  
  char decoded[256];
  const char *key_str = buf;
  size_t key_len = len;
  
  if (has_unicode_escape(buf, len)) {
    key_len = decode_ident_escapes(buf, len, decoded, sizeof(decoded));
    key_str = decoded;
  }
  
  if (key_len == STR_PROTO_LEN && memcmp(key_str, STR_PROTO, STR_PROTO_LEN) == 0) {
    jsval_t proto = get_slot(js, js->scope, SLOT_PROTO);
    if (vtype(proto) != T_UNDEF) return proto;
    return get_prototype_for_type(js, vtype(js->scope));
  }
  
  const char *key_intern = intern_string(key_str, key_len);
  
  jsval_t parent_scope = upper(js, js->scope);
  
  jsoff_t off = lkp_interned(js, js->scope, key_intern, key_len);
  if (off != 0) {
    return mkval(T_PROP, off);
  }
  
  jsval_t with_slot = get_slot(js, js->scope, SLOT_WITH);
  if (vtype(with_slot) != T_UNDEF) {
    jsval_t with_obj = (
      vtype(with_slot) == T_OBJ ||
      vtype(with_slot) == T_ARR || 
      vtype(with_slot) == T_FUNC) ? 
      with_slot : mkval(T_OBJ, vdata(with_slot)
    );
    
    jsoff_t prop_off = lkp_interned(js, with_obj, key_intern, key_len);
    if (prop_off != 0) {
      jsval_t key = js_mkstr(js, key_str, key_len);
      if (is_err(key)) return key;
      return mkpropref((jsoff_t)vdata(with_obj), (jsoff_t)vdata(key));
    }
  }
  
  uint8_t depth = 1;
  for (jsval_t scope = parent_scope; depth < 255; depth++) {
    off = lkp_interned(js, scope, key_intern, key_len);
    if (off != 0) {
      return mkval(T_PROP, off);
    }
    
    jsval_t scope_with_slot = get_slot(js, scope, SLOT_WITH);
    if (vtype(scope_with_slot) != T_UNDEF) {
      jsval_t with_obj = (
        vtype(scope_with_slot) == T_OBJ ||
        vtype(scope_with_slot) == T_ARR || 
        vtype(scope_with_slot) == T_FUNC) ? 
        scope_with_slot : mkval(T_OBJ, vdata(scope_with_slot)
      );
      
      jsoff_t prop_off = lkp_interned(js, with_obj, key_intern, key_len);
      if (prop_off != 0) {
        jsval_t key = js_mkstr(js, key_str, key_len);
        if (is_err(key)) return key;
        return mkpropref((jsoff_t)vdata(with_obj), (jsoff_t)vdata(key));
      }
    }
    
    if (vdata(scope) == 0) break;
    scope = upper(js, scope);
  }
  
  if (global_scope_stack && utarray_len(global_scope_stack) > 0) {
    jsoff_t *root_off = (jsoff_t *)utarray_eltptr(global_scope_stack, 0);
    if (root_off && *root_off != 0) {
      jsval_t root_scope = mkval(T_OBJ, *root_off);
      jsoff_t root_lkp_off = lkp(js, root_scope, key_str, key_len);
      if (root_lkp_off != 0) return mkval(T_PROP, root_lkp_off);
    }
  }
  
  return js_mkerr_typed(js, JS_ERR_REFERENCE, "'%.*s' is not defined", (int) key_len, key_str);
}

static bool try_accessor_getter(struct js *js, jsval_t obj, const char *key, size_t key_len, jsval_t *out) {
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

jsval_t resolveprop(struct js *js, jsval_t v) {
  if (vtype(v) == T_PROPREF) {
    if (is_prim_propref(v)) {
      prim_propref_data_t *prim_data = prim_propref_get(v);
      if (!prim_data) return js_mkundef();
      
      jsval_t prim = prim_data->prim_val;
      jsval_t key = mkval(T_STR, prim_data->key_off);
      jsoff_t key_len;
      const char *key_str = (const char *)&js->mem[vstr(js, key, &key_len)];
      
       jsval_t proto = get_prototype_for_type(js, vtype(prim));
       if (vtype(proto) == T_OBJ) {
         jsval_t getter = js_mkundef();
         bool has_getter = false;
         lkp_with_getter(js, proto, key_str, key_len, &getter, &has_getter);
         jsval_t result = call_proto_accessor(js, prim, getter, has_getter, NULL, 0, false);
         if (vtype(result) != T_UNDEF) return result;

         jsoff_t off = lkp_proto(js, prim, key_str, key_len);
         if (off != 0) return resolveprop(js, mkval(T_PROP, off));
       }
      
      return js_mkundef();
    }
    
    jsoff_t obj_off = propref_obj(v);
    jsoff_t key_off = propref_key(v);
    jsval_t key = mkval(T_STR, key_off);
    jsoff_t len;
    const char *key_str = (const char *)&js->mem[vstr(js, key, &len)];
    
    if (is_arr_off(js, obj_off) && streq(key_str, len, "length", 6)) {
      jsval_t arr = mkval(T_ARR, obj_off);
      return tov(D(js_arr_len(js, arr)));
    }
    
    jsval_t obj = mkval(T_OBJ, obj_off);
    if (is_proxy(js, obj)) return proxy_get(js, obj, key_str, len);
    
    if (len == STR_PROTO_LEN && memcmp(key_str, STR_PROTO, STR_PROTO_LEN) == 0) {
      jsval_t proto = get_slot(js, obj, SLOT_PROTO);
      if (vtype(proto) != T_UNDEF) return proto;
      return get_prototype_for_type(js, vtype(obj));
    }
    
    jsoff_t prop_off = lkp(js, obj, key_str, len);
    if (prop_off != 0) return resolveprop(js, mkval(T_PROP, prop_off));
    
    jsoff_t proto_off = lkp_proto(js, obj, key_str, len);
    if (proto_off != 0) return resolveprop(js, mkval(T_PROP, proto_off));
    
    jsval_t accessor_result;
    if (try_accessor_getter(js, obj, key_str, len, &accessor_result)) {
      return accessor_result;
    }
    
    jsval_t dyn_result = try_dynamic_getter(js, obj, key_str, len);
    if (vtype(dyn_result) != T_UNDEF) return dyn_result;
    
    return js_mkundef();
  }
  if (vtype(v) != T_PROP) return v;
  return resolveprop(js, loadval(js, (jsoff_t) (vdata(v) + sizeof(jsoff_t) * 2)));
}

static bool try_accessor_setter(struct js *js, jsval_t obj, const char *key, size_t key_len, jsval_t val, jsval_t *out) {
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

static jsval_t assign(struct js *js, jsval_t lhs, jsval_t val) {
  if (js->flags & F_NOEXEC) return val;
  if (vtype(lhs) == T_PROPREF) {
    if (is_prim_propref(lhs)) {
      prim_propref_data_t *prim_data = prim_propref_get(lhs);
      if (!prim_data) {
        if (js->flags & F_STRICT) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot create property on primitive value");
        return val;
      }
      
      jsval_t prim = prim_data->prim_val;
      jsval_t key = mkval(T_STR, prim_data->key_off);
      jsoff_t key_len;
      const char *key_str = (const char *)&js->mem[vstr(js, key, &key_len)];
      
       jsval_t proto = get_prototype_for_type(js, vtype(prim));
       if (vtype(proto) == T_OBJ) {
         jsval_t setter = js_mkundef();
         bool has_setter = false;
         lkp_with_setter(js, proto, key_str, key_len, &setter, &has_setter);
         jsval_t result = call_proto_accessor(js, prim, setter, has_setter, &val, 1, true);
         if (vtype(result) != T_UNDEF) return result;
       }
      
      if (js->flags & F_STRICT) {
        return js_mkerr_typed(
          js, JS_ERR_TYPE, "Cannot create property '%.*s' on %s", 
          (int)key_len, key_str, typestr(vtype(prim))
        );
      }
      return val;
    }
    
    jsoff_t obj_off = propref_obj(lhs);
    jsoff_t key_off = propref_key(lhs);
    jsval_t obj = mkval(is_arr_off(js, obj_off) ? T_ARR : T_OBJ, obj_off);
    jsval_t key = mkval(T_STR, key_off);
    
    jsoff_t key_len;
    const char *key_str = (const char *)&js->mem[vstr(js, key, &key_len)];
    
    jsval_t setter_result;
    if (try_accessor_setter(js, obj, key_str, key_len, val, &setter_result)) {
      return setter_result;
    }
    
    return setprop(js, obj, key, val);
  }
  
  if (vtype(lhs) != T_PROP) {
    if (js->flags & F_STRICT) {
      return js_mkerr_typed(js, JS_ERR_SYNTAX, "Invalid left-hand side in assignment");
    }
    return val;
  }
  
  jsoff_t propoff = (jsoff_t) vdata(lhs);
  
  jsoff_t koff = loadoff(js, propoff + sizeof(jsoff_t));
  jsoff_t klen = offtolen(loadoff(js, koff));
  const char *key = (char *)&js->mem[koff + sizeof(jsoff_t)];
  
  if (is_const_prop(js, propoff)) {
    if (js->flags & F_STRICT) return js_mkerr(js, "assignment to constant");
    return mkval(T_PROP, propoff);
  }
  
  if ((klen == 9 && memcmp(key, "undefined", 9) == 0) ||
      (klen == 3 && memcmp(key, "NaN", 3) == 0) ||
      (klen == 8 && memcmp(key, "Infinity", 8) == 0)) {
    if (js->flags & F_STRICT) return js_mkerr(js, "Cannot assign to read only property");
    return lhs;
  }
  
  saveval(js, (jsoff_t) ((vdata(lhs) & ~3ULL) + sizeof(jsoff_t) * 2), val);
  return lhs;
}

static jsval_t do_assign_op(struct js *js, uint8_t op, jsval_t l, jsval_t r) {
  uint8_t m[] = {
    TOK_PLUS, TOK_MINUS, TOK_MUL, TOK_DIV, TOK_REM, TOK_SHL,
    TOK_SHR,  TOK_ZSHR,  TOK_AND, TOK_XOR, TOK_OR
  };
  
  jsval_t res = do_op(js, m[op - TOK_PLUS_ASSIGN], resolveprop(js, l), r);
  return assign(js, l, res);
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

static jsval_t string_builder_finalize(struct js *js, string_builder_t *sb) {
  jsval_t result = js_mkstr(js, sb->buffer, sb->size);
  if (sb->is_dynamic && sb->buffer) free(sb->buffer);
  return result;
}

static jsval_t do_string_op(struct js *js, uint8_t op, jsval_t l, jsval_t r) {
  jsoff_t n1, off1 = vstr(js, l, &n1);
  jsoff_t n2, off2 = vstr(js, r, &n2);
  
  if (op == TOK_PLUS) {
    string_builder_t sb;
    char static_buffer[512];
    string_builder_init(&sb, static_buffer, sizeof(static_buffer));
    
    if (!string_builder_append(&sb, (char *)&js->mem[off1], n1) ||
        !string_builder_append(&sb, (char *)&js->mem[off2], n2)) {
      return js_mkerr(js, "string concatenation failed");
    }
    
    return string_builder_finalize(js, &sb);
  } else if (op == TOK_EQ) {
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
      } else {
        cmp = (n1 < n2) ? -1 : 1;
      }
    }
    
    switch (op) {
      case TOK_LT: return mkval(T_BOOL, cmp < 0 ? 1 : 0);
      case TOK_LE: return mkval(T_BOOL, cmp <= 0 ? 1 : 0);
      case TOK_GT: return mkval(T_BOOL, cmp > 0 ? 1 : 0);
      case TOK_GE: return mkval(T_BOOL, cmp >= 0 ? 1 : 0);
      default:     return js_mkerr(js, "bad str op");
    }
  } else {
    return js_mkerr(js, "bad str op");
  }
}

static jsval_t do_bracket_op(struct js *js, jsval_t l, jsval_t r) {
  jsval_t obj = resolveprop(js, l);
  jsval_t key_val = resolveprop(js, r);
  char keybuf[64];
  const char *keystr;
  size_t keylen;
  if (vtype(key_val) == T_NUM) {
    double dv = tod(key_val);
    if (dv >= 0 && dv <= 0xFFFFFFFF && dv == (double)(uint32_t)dv) {
      keylen = uint_to_str(keybuf, sizeof(keybuf), (uint32_t)dv);
    } else {
      keylen = strnum(key_val, keybuf, sizeof(keybuf));
    }
    keystr = keybuf;
  } else if (vtype(key_val) == T_STR) {
    jsoff_t slen;
    jsoff_t off = vstr(js, key_val, &slen);
    keystr = (char *) &js->mem[off];
    keylen = slen;
  } else if (vtype(key_val) == T_SYMBOL) {
    snprintf(keybuf, sizeof(keybuf), "__sym_%llu__", (unsigned long long)sym_get_id(key_val));
    keystr = keybuf;
    keylen = strlen(keybuf);
  } else {
    jsval_t str_val = js_tostring_val(js, key_val);
    if (is_err(str_val)) return str_val;
    jsoff_t slen;
    jsoff_t off = vstr(js, str_val, &slen);
    keystr = (char *) &js->mem[off];
    keylen = slen;
  }
  if (streq(keystr, keylen, "length", 6)) {
    if (vtype(obj) == T_STR) {
      jsoff_t byte_len;
      jsoff_t str_off = vstr(js, obj, &byte_len);
      const char *str_data = (const char *)&js->mem[str_off];
      return tov(D(utf16_strlen(str_data, byte_len)));
    }
    if (vtype(obj) == T_ARR) {
      jsoff_t len_off = lkp(js, obj, "length", 6);
      if (len_off != 0) {
        return mkval(T_PROP, len_off);
      }
      jsval_t key = js_mkstr(js, "length", 6);
      jsval_t len_val = tov(D(js_arr_len(js, obj)));
      jsval_t prop = setprop(js, obj, key, len_val);
      return prop;
    }
  }
  if (vtype(obj) == T_STR) {
    double idx_d = JS_NAN;
    if (vtype(key_val) == T_NUM) {
      idx_d = tod(key_val);
    } else {
      char *endptr;
      char temp[64];
      size_t copy_len = keylen < sizeof(temp) - 1 ? keylen : sizeof(temp) - 1;
      memcpy(temp, keystr, copy_len);
      temp[copy_len] = '\0';
      idx_d = strtod(temp, &endptr);
      if (endptr == temp || *endptr != '\0') idx_d = JS_NAN;
    }
    if (!isnan(idx_d) && idx_d >= 0 && idx_d == (double)(long)idx_d) {
      jsoff_t idx = (jsoff_t) idx_d;
      jsoff_t byte_len = offtolen(loadoff(js, (jsoff_t) vdata(obj)));
      jsoff_t str_off = (jsoff_t) vdata(obj) + sizeof(jsoff_t);
      const char *str_data = (const char *)&js->mem[str_off];
      size_t char_bytes;
      int byte_offset = utf16_index_to_byte_offset(str_data, byte_len, idx, &char_bytes);
      if (byte_offset >= 0) {
        return js_mkstr(js, str_data + byte_offset, char_bytes);
      }
    }
    jsoff_t off = lkp_proto(js, obj, keystr, keylen);
    if (off != 0) return resolveprop(js, mkval(T_PROP, off));
    return js_mkundef();
  }
  if (vtype(obj) == T_FUNC) {
    if ((js->flags & F_STRICT) && (streq(keystr, keylen, "caller", 6) || streq(keystr, keylen, "arguments", 9))) {
      return js_mkerr_typed(js, JS_ERR_TYPE, "'%.*s' not allowed on functions in strict mode", (int)keylen, keystr);
    }
    jsval_t func_obj = mkval(T_OBJ, vdata(obj));
    jsoff_t off = lkp_proto(js, obj, keystr, keylen);
    if (off != 0) {
      jsoff_t obj_off = (jsoff_t)vdata(obj);
      descriptor_entry_t *desc = lookup_descriptor(obj_off, keystr, keylen);
      if (desc) {
        jsval_t key = js_mkstr(js, keystr, keylen);
        return mkpropref(obj_off, (jsoff_t)vdata(key));
      }
      return mkval(T_PROP, off);
    }
    if (streq(keystr, keylen, "name", 4)) return js_mkstr(js, "", 0);
    jsval_t key = js_mkstr(js, keystr, keylen);
    jsval_t prop = setprop(js, func_obj, key, js_mkundef());
    return prop;
  }
  if (vtype(obj) == T_CFUNC) {
    if ((js->flags & F_STRICT) && (streq(keystr, keylen, "caller", 6) || streq(keystr, keylen, "arguments", 9))) {
      return js_mkerr_typed(js, JS_ERR_TYPE, "'%.*s' not allowed on functions in strict mode", (int)keylen, keystr);
    }
    jsoff_t off = lkp_proto(js, obj, keystr, keylen);
    if (off != 0) return resolveprop(js, mkval(T_PROP, off));
    if (streq(keystr, keylen, "name", 4)) return js_mkstr(js, "", 0);
    return js_mkundef();
  }
  if (vtype(obj) == T_NUM || vtype(obj) == T_BOOL || vtype(obj) == T_BIGINT) {
    jsval_t key = js_mkstr(js, keystr, keylen);
    return mkprim_propref(obj, (jsoff_t)vdata(key));
  }
  if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR) {
    return js_mkundef();
  }
  if ((streq(keystr, keylen, "callee", 6) || streq(keystr, keylen, "caller", 6)) &&
      vtype(get_slot(js, obj, SLOT_STRICT_ARGS)) != T_UNDEF) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "'%.*s' not allowed on strict arguments", (int)keylen, keystr);
  }
  
  jsval_t getter = js_mkundef();
  bool has_getter = false;
  jsoff_t prop_off = lkp_with_getter(js, obj, keystr, keylen, &getter, &has_getter);
  
  jsval_t setter = js_mkundef();
  bool has_setter = false;
  if (!has_getter) {
    lkp_with_setter(js, obj, keystr, keylen, &setter, &has_setter);
  }
  
  if (has_getter || has_setter) {
    jsval_t key = js_mkstr(js, keystr, keylen);
    return mkpropref((jsoff_t)vdata(obj), (jsoff_t)vdata(key));
  }
  
  if (prop_off != 0) {
    return mkval(T_PROP, prop_off);
  }
  
  jsval_t dyn_result = try_dynamic_getter(js, obj, keystr, keylen);
  if (vtype(dyn_result) != T_UNDEF) {
    jsval_t key = js_mkstr(js, keystr, keylen);
    return mkpropref((jsoff_t)vdata(obj), (jsoff_t)vdata(key));
  }
  
  jsoff_t off = lkp_proto(js, obj, keystr, keylen);
  if (off == 0) {
    jsval_t key = js_mkstr(js, keystr, keylen);
    return mkpropref((jsoff_t)vdata(obj), (jsoff_t)vdata(key));
  }
  return mkval(T_PROP, off);
}

static jsval_t do_dot_op(struct js *js, jsval_t l, jsval_t r) {
  const char *raw_ptr = (char *) &js->code[coderefoff(r)];
  size_t raw_len = codereflen(r);
  
  char decoded_buf[256];
  size_t plen = decode_ident_escapes(raw_ptr, raw_len, decoded_buf, sizeof(decoded_buf));
  const char *ptr = decoded_buf;
  
  if (vtype(r) != T_CODEREF) return js_mkerr_typed(js, JS_ERR_SYNTAX, "ident expected");
  uint8_t t = vtype(l);
  
  if (t == T_STR && streq(ptr, plen, "length", 6)) {
    jsoff_t byte_len;
    jsoff_t str_off = vstr(js, l, &byte_len);
    const char *str_data = (const char *)&js->mem[str_off];
    return tov(D(utf16_strlen(str_data, byte_len)));
  }
  
  if (t == T_ARR && streq(ptr, plen, "length", 6)) {
    jsval_t key = js_mkstr(js, "length", 6);
    return mkpropref((jsoff_t)vdata(l), (jsoff_t)vdata(key));
  }
  
  if (t == T_STR || t == T_NUM || t == T_BOOL || t == T_BIGINT) {
    jsval_t key = js_mkstr(js, ptr, plen);
    return mkprim_propref(l, (jsoff_t)vdata(key));
  }
  
  if (t == T_PROMISE) {
    jsoff_t off = lkp_proto(js, mkval(T_OBJ, vdata(l)), ptr, plen);
    if (off != 0) {
      return resolveprop(js, mkval(T_PROP, off));
    }
    jsval_t promise_proto = get_ctor_proto(js, "Promise", 7);
    if (vtype(promise_proto) != T_UNDEF && vtype(promise_proto) != T_NULL) {
      off = lkp_proto(js, promise_proto, ptr, plen);
      if (off != 0) {
        return resolveprop(js, mkval(T_PROP, off));
      }
    }
    return js_mkundef();
  }
  
  if (t == T_FUNC) {
    if ((js->flags & F_STRICT) && (streq(ptr, plen, "caller", 6) || streq(ptr, plen, "arguments", 9))) {
      return js_mkerr_typed(js, JS_ERR_TYPE, "'%.*s' not allowed on functions in strict mode", (int)plen, ptr);
    }
    if (plen == STR_PROTO_LEN && memcmp(ptr, STR_PROTO, STR_PROTO_LEN) == 0) {
      jsval_t proto = get_slot(js, mkval(T_OBJ, vdata(l)), SLOT_PROTO);
      if (vtype(proto) != T_UNDEF) return proto;
      return get_prototype_for_type(js, T_FUNC);
    }
    jsval_t func_obj = mkval(T_OBJ, vdata(l));
    jsoff_t off = lkp_proto(js, l, ptr, plen);
    if (off != 0) {
      jsoff_t obj_off = (jsoff_t)vdata(l);
      descriptor_entry_t *desc = lookup_descriptor(obj_off, ptr, plen);
      if (desc) {
        jsval_t key = js_mkstr(js, ptr, plen);
        return mkpropref(obj_off, (jsoff_t)vdata(key));
      }
      return mkval(T_PROP, off);
    }
    if (streq(ptr, plen, "name", 4)) return js_mkstr(js, "", 0);
    jsval_t key = js_mkstr(js, ptr, plen);
    jsval_t prop = setprop(js, func_obj, key, js_mkundef());
    return prop;
  }
  
  if (t == T_CFUNC) {
    if ((js->flags & F_STRICT) && (streq(ptr, plen, "caller", 6) || streq(ptr, plen, "arguments", 9))) {
      return js_mkerr_typed(js, JS_ERR_TYPE, "'%.*s' not allowed on functions in strict mode", (int)plen, ptr);
    }
    jsoff_t off = lkp_proto(js, l, ptr, plen);
    if (off != 0) return resolveprop(js, mkval(T_PROP, off));
    if (streq(ptr, plen, "name", 4)) return js_mkstr(js, "", 0);
    return js_mkundef();
  }
  
  if (t == T_SYMBOL) {
    if (streq(ptr, plen, "description", 11)) {
      const char *desc = sym_get_desc(js, l);
      if (desc) return js_mkstr(js, desc, strlen(desc));
      return js_mkundef();
    }
    return js_mkundef();
  }
  
  if (t != T_OBJ && t != T_ARR) {
    jsoff_t saved_toff = js->toff;
    jsoff_t saved_tlen = js->tlen;
    
    js->toff = coderefoff(r);
    js->tlen = codereflen(r);
    
    jsval_t err = js_mkerr(
      js, "Cannot read properties of %s (reading '%.*s')",
      t == T_UNDEF ? "undefined" : t == T_NULL ? "null" : typestr(t),
      (int)plen, ptr
    );
    
    js->toff = saved_toff;
    js->tlen = saved_tlen;
    
    return err;
  }
  
  if ((streq(ptr, plen, "callee", 6) || streq(ptr, plen, "caller", 6)) &&
      vtype(get_slot(js, l, SLOT_STRICT_ARGS)) != T_UNDEF) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "'%.*s' not allowed on strict arguments", (int)plen, ptr);
  }
  
  if (plen == STR_PROTO_LEN && memcmp(ptr, STR_PROTO, STR_PROTO_LEN) == 0) {
    jsval_t key = js_mkstr(js, ptr, plen);
    return mkpropref((jsoff_t)vdata(l), (jsoff_t)vdata(key));
  }
  
  jsoff_t own_off = lkp(js, l, ptr, plen);
  if (own_off != 0) {
    jsoff_t obj_off = (jsoff_t)vdata(l);
    descriptor_entry_t *desc = lookup_descriptor(obj_off, ptr, plen);
    if (desc) {
      jsval_t key = js_mkstr(js, ptr, plen);
      return mkpropref((jsoff_t)vdata(l), (jsoff_t)vdata(key));
    }
    return mkval(T_PROP, own_off);
  }
  
  jsval_t result = try_dynamic_getter(js, l, ptr, plen);
  if (vtype(result) != T_UNDEF) {
    own_off = lkp(js, l, ptr, plen);
    if (own_off != 0) return mkval(T_PROP, own_off);
  }

  jsval_t key = js_mkstr(js, ptr, plen);
  return mkpropref((jsoff_t)vdata(l), (jsoff_t)vdata(key));
}

static jsval_t do_optional_chain_op(struct js *js, jsval_t l, jsval_t r) {
  if (vtype(l) == T_NULL || vtype(l) == T_UNDEF) return js_mkundef();
  return do_dot_op(js, l, r);
}

static jsval_t js_call_params(struct js *js) {
  jsoff_t pos = js->pos;
  uint8_t flags = js->flags;
  js->flags |= F_NOEXEC;
  js->consumed = 1;
  
  while (next(js) != TOK_EOF) {
    if (next(js) == TOK_RPAREN) break;
    if (next(js) == TOK_REST) js->consumed = 1;
    js_expr(js);
    if (next(js) == TOK_RPAREN) break;
    EXPECT(TOK_COMMA, js->flags = flags);
  }
  
  EXPECT(TOK_RPAREN, js->flags = flags);
  js->flags = flags;
  return mkcoderef(pos, js->pos - pos - js->tlen);
}

static void reverse(jsval_t *args, int nargs) {
  for (int i = 0; i < nargs / 2; i++) {
    jsval_t tmp = args[i];
    args[i] = args[nargs - i - 1], args[nargs - i - 1] = tmp;
  }
}

static int parse_call_args(struct js *js, UT_array *args, jsval_t *err_out) {
  while (js->pos < js->clen) {
    if (next(js) == TOK_RPAREN) break;
    bool is_spread = (next(js) == TOK_REST);
    if (is_spread) js->consumed = 1;
    jsval_t arg = resolveprop(js, js_expr(js));
    if (is_err(arg)) { *err_out = arg; return -1; }
    if (is_spread && vtype(arg) == T_ARR) {
      jsoff_t len = js_arr_len(js, arg);
      for (jsoff_t i = 0; i < len; i++) {
        jsval_t elem = js_arr_get(js, arg, i);
        utarray_push_back(args, &elem);
      }
    } else utarray_push_back(args, &arg);
    if (next(js) == TOK_COMMA) js->consumed = 1;
  }
  
  return (int)utarray_len(args);
}

static jsval_t call_c(struct js *js, jsval_t (*fn)(struct js *, jsval_t *, int)) {
  UT_array *args;
  utarray_new(args, &jsval_icd);
  jsval_t err, res;
  
  int argc = parse_call_args(js, args, &err);
  if (argc < 0) { utarray_free(args); return err; }
  
  jsval_t *argv = (jsval_t *)utarray_front(args);
  jsval_t saved_this = js->this_val;
  js->this_val = peek_this();
  res = fn(js, argv, argc);
  js->this_val = saved_this;
  
  utarray_free(args);
  return res;
}

static jsoff_t extract_default_param_value(const char *fn, jsoff_t fnlen, jsoff_t start_pos, jsoff_t *out_start, jsoff_t *out_len) {
  jsoff_t after_ident = skiptonext(fn, fnlen, start_pos, NULL);
  if (after_ident >= fnlen || fn[after_ident] != '=') {
    *out_start = 0;
    *out_len = 0;
    return after_ident;
  }
  
  jsoff_t default_start = skiptonext(fn, fnlen, after_ident + 1, NULL);
  jsoff_t default_len = 0;
  jsoff_t depth = 0;
  bool in_string = false;
  char string_char = 0;
  
  for (jsoff_t i = default_start; i < fnlen; i++) {
    if (in_string) {
      if (fn[i] == '\\' && i + 1 < fnlen) {
        default_len += 2;
        i++;
        continue;
      }
      if (fn[i] == string_char) {
        in_string = false;
      }
      default_len++;
    } else {
      if (fn[i] == '"' || fn[i] == '\'' || fn[i] == '`') {
        in_string = true;
        string_char = fn[i];
        default_len++;
      } else if (fn[i] == '(' || fn[i] == '[' || fn[i] == '{') {
        depth++;
        default_len++;
      } else if (fn[i] == ')' || fn[i] == ']' || fn[i] == '}') {
        if (depth == 0 && fn[i] == ')') break;
        depth--;
        default_len++;
      } else if (depth == 0 && fn[i] == ',') {
        break;
      } else {
        default_len++;
      }
    }
  }
  
  *out_start = default_start;
  *out_len = default_len;
  return skiptonext(fn, fnlen, default_start + default_len, NULL);
}

static jsoff_t skip_default_expr(const char *p, jsoff_t len, jsoff_t pos) {
  int depth = 0;
  while (pos < len) {
    char c = p[pos];
    if (c == '(' || c == '[' || c == '{') depth++;
    else if (c == ')' || c == ']' || c == '}') { if (depth == 0) break; depth--; }
    else if (c == ',' && depth == 0) break;
    pos++;
  }
  return pos;
}

static jsval_t bind_destruct_pattern(struct js *js, const char *p, jsoff_t len, jsval_t val, jsval_t scope) {
  jsoff_t pos = skiptonext(p, len, 0, NULL);
  if (pos >= len) return js_mkundef();
  
  bool is_arr = (p[pos] == '[');
  if (!is_arr && p[pos] != '{') return js_mkerr(js, "invalid destructuring pattern");
  
  pos++;
  int idx = 0;
  
  while (pos < len) {
    pos = skiptonext(p, len, pos, NULL);
    if (pos >= len) break;
    if ((is_arr && p[pos] == ']') || (!is_arr && p[pos] == '}')) break;
    if (p[pos] == ',') { pos++; idx++; continue; }
    
    bool is_rest = (pos + 2 < len && p[pos] == '.' && p[pos+1] == '.' && p[pos+2] == '.');
    if (is_rest) { pos += 3; pos = skiptonext(p, len, pos, NULL); }
    
    jsoff_t name_len = 0;
    if (parseident(&p[pos], len - pos, &name_len) != TOK_IDENTIFIER) break;
    
    jsoff_t var_pos = pos, var_len = name_len;
    jsoff_t src_pos = pos, src_len = name_len;
    pos += name_len;
    pos = skiptonext(p, len, pos, NULL);
    
    if (!is_arr && !is_rest && pos < len && p[pos] == ':') {
      pos = skiptonext(p, len, pos + 1, NULL);
      jsoff_t rlen = 0;
      if (parseident(&p[pos], len - pos, &rlen) == TOK_IDENTIFIER) {
        var_pos = pos; var_len = rlen;
        pos += rlen;
        pos = skiptonext(p, len, pos, NULL);
      }
    }
    
    jsval_t prop_val;
    if (is_rest && is_arr) {
      jsval_t rest = js_mkarr(js);
      if (is_err(rest)) return rest;
      jsoff_t alen = js_arr_len(js, val);
      for (jsoff_t i = idx; i < alen; i++) js_arr_push(js, rest, js_arr_get(js, val, i));
      prop_val = rest;
    } else if (is_rest) {
      prop_val = mkobj(js, 0);
    } else if (is_arr) {
      prop_val = js_arr_get(js, val, idx);
    } else {
      jsoff_t off = lkp(js, val, &p[src_pos], src_len);
      prop_val = off > 0 ? resolveprop(js, mkval(T_PROP, off)) : js_mkundef();
    }
    
    if (is_rest) goto bind;
    if (pos >= len || p[pos] != '=') goto bind;
    
    pos++;
    jsoff_t def_start = pos;
    pos = skip_default_expr(p, len, pos);
    if (vtype(prop_val) != T_UNDEF) goto bind;
    
    prop_val = js_eval_str(js, &p[def_start], pos - def_start);
    if (is_err(prop_val)) return prop_val;
    prop_val = resolveprop(js, prop_val);
    
bind:;
    jsval_t vname = js_mkstr(js, &p[var_pos], var_len);
    if (is_err(vname)) return vname;
    jsval_t r = setprop(js, scope, vname, prop_val);
    if (is_err(r)) return r;
    
    idx++;
    pos = skiptonext(p, len, pos, NULL);
    if (pos < len && p[pos] == ',') pos++;
  }
  
  return js_mkundef();
}

static bool is_strict_function_body(const char *body, size_t len) {
  size_t i = 0;
  while (i < len && (body[i] == ' ' || body[i] == '\t' || body[i] == '\n' || body[i] == '\r')) i++;
  if (i + 12 <= len && (body[i] == '\'' || body[i] == '"')) {
    char q = body[i];
    if (memcmp(&body[i+1], "use strict", 10) == 0 && body[i+11] == q) return true;
  }
  return false;
}

static parsed_func_t *get_or_parse_func(const char *fn, jsoff_t fnlen) {
  uint64_t h = hash_key(fn, fnlen);
  parsed_func_t *cached = NULL;
  HASH_FIND(hh, func_parse_cache, &h, sizeof(h), cached);
  if (cached) return cached;
  
  parsed_func_t *pf = (parsed_func_t *)malloc(sizeof(parsed_func_t));
  if (!pf) return NULL;
  memset(pf, 0, sizeof(*pf));
  pf->code_hash = h;
  utarray_new(pf->params, &parsed_param_icd);
  
  jsoff_t fnpos = 1;
  
  while (fnpos < fnlen) {
    fnpos = skiptonext(fn, fnlen, fnpos, NULL);
    if (fnpos < fnlen && fn[fnpos] == ')') break;
    
    bool is_rest = false;
    if (fnpos + 3 < fnlen && fn[fnpos] == '.' && fn[fnpos + 1] == '.' && fn[fnpos + 2] == '.') {
      is_rest = true;
      pf->has_rest = true;
      fnpos += 3;
      fnpos = skiptonext(fn, fnlen, fnpos, NULL);
    }
    
    jsoff_t identlen = 0;
    
    uint8_t tok = parseident(&fn[fnpos], fnlen - fnpos, &identlen);
    bool is_valid_ident = (tok == TOK_IDENTIFIER || is_contextual_keyword(tok));
    
    if (!is_valid_ident && (fn[fnpos] == '{' || fn[fnpos] == '[')) {
      char bracket_open = fn[fnpos];
      char bracket_close = (bracket_open == '{') ? '}' : ']';
      
      jsoff_t pattern_start = fnpos;
      int depth = 1; fnpos++;
      
      while (fnpos < fnlen && depth > 0) {
        if (fn[fnpos] == bracket_open) depth++;
        else if (fn[fnpos] == bracket_close) depth--;
        fnpos++;
      }
      jsoff_t pattern_len = fnpos - pattern_start;
      
      {
        parsed_param_t pp = {0};
        pp.is_destruct = true;
        pp.pattern_off = pattern_start;
        pp.pattern_len = pattern_len;
        
        fnpos = skiptonext(fn, fnlen, fnpos, NULL);
        if (fnpos < fnlen && fn[fnpos] == '=') {
          fnpos = extract_default_param_value(fn, fnlen, fnpos, &pp.default_start, &pp.default_len);
        }
        utarray_push_back(pf->params, &pp);
        pf->param_count++;
      }
      
      if (fnpos < fnlen && fn[fnpos] == ',') fnpos++;
      continue;
    }
    
    if (!is_valid_ident) break;
    
    if (is_rest) {
      pf->rest_param_start = fnpos;
      pf->rest_param_len = identlen;
      fnpos = skiptonext(fn, fnlen, fnpos + identlen, NULL);
      break;
    }
    
    {
      parsed_param_t pp = {0};
      pp.name_off = fnpos;
      pp.name_len = identlen;
      pp.is_destruct = false;
      fnpos = extract_default_param_value(fn, fnlen, fnpos + identlen, &pp.default_start, &pp.default_len);
      utarray_push_back(pf->params, &pp);
      pf->param_count++;
    }
    
    if (fnpos < fnlen && fn[fnpos] == ',') fnpos++;
  }
  
  if (fnpos < fnlen && fn[fnpos] == ')') fnpos++;
  fnpos = skiptonext(fn, fnlen, fnpos, NULL);
  if (fnpos < fnlen && fn[fnpos] == '{') fnpos++;
  
  pf->body_start = fnpos;
  pf->body_len = (fnlen > fnpos + 1) ? (fnlen - fnpos - 1) : 0;
  pf->is_strict = is_strict_function_body(&fn[fnpos], pf->body_len);
  
  HASH_ADD(hh, func_parse_cache, code_hash, sizeof(pf->code_hash), pf);
  return pf;
}

static bool is_eval_or_arguments(struct js *js, jsoff_t toff, jsoff_t tlen) {
  if (tlen == 4 && memcmp(&js->code[toff], "eval", 4) == 0) return true;
  if (tlen == 9 && memcmp(&js->code[toff], "arguments", 9) == 0) return true;
  return false;
}

static bool code_uses_arguments(const char *code, jsoff_t len) {
  if (len < 9) return false;
  for (jsoff_t i = 0; i + 8 < len; i++) {
    if (code[i] == 'a' && memcmp(&code[i], INTERN_ARGUMENTS, 9) == 0) {
      if (i > 0 && (is_alpha(code[i-1]) || code[i-1] == '_' || (code[i-1] >= '0' && code[i-1] <= '9'))) continue;
      if (i + 9 < len && (is_alpha(code[i+9]) || code[i+9] == '_' || (code[i+9] >= '0' && code[i+9] <= '9'))) continue;
      return true;
    }
  }
  return false;
}

static void setup_arguments(struct js *js, jsval_t scope, jsval_t *args, int nargs, bool strict) {
  if (vtype(js->current_func) == T_FUNC) {
    jsval_t func_obj = mkval(T_OBJ, vdata(js->current_func));
    if (vtype(get_slot(js, func_obj, SLOT_THIS)) != T_UNDEF) return;
  }
  
  jsval_t arguments_obj = mkobj(js, 0);
  for (int i = 0; i < nargs; i++) {
    if (i < 10) {
      setprop(js, arguments_obj, js_mkstr(js, INTERN_IDX[i], 1), args[i]);
    } else {
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
      setprop(js, arguments_obj, js_mkstr(js, idxstr, idxlen), args[i]);
    }
  }
  setprop_interned(js, arguments_obj, INTERN_LENGTH, 6, tov((double) nargs));
  if (strict) {
    set_slot(js, arguments_obj, SLOT_STRICT_ARGS, tov(1));
  } else if (vtype(js->current_func) == T_FUNC) {
    setprop_interned(js, arguments_obj, INTERN_CALLEE, 6, js->current_func);
  }
  
  const char *toStringTag_key = get_toStringTag_sym_key();
  if (toStringTag_key && toStringTag_key[0] != '\0') {
    setprop(js, arguments_obj, js_mkstr(js, toStringTag_key, strlen(toStringTag_key)), js_mkstr(js, "Arguments", 9));
  }
  
  arguments_obj = mkval(T_ARR, vdata(arguments_obj));
  setprop_interned(js, scope, INTERN_ARGUMENTS, 9, arguments_obj);
  
  if (!strict && vtype(js->current_func) == T_FUNC) {
    jsval_t func_obj = mkval(T_OBJ, vdata(js->current_func));
    setprop_interned(js, func_obj, INTERN_ARGUMENTS, 9, arguments_obj);
  }
}

static inline void restore_saved_scope(struct js *js) {
  if (saved_scope_stack && utarray_len(saved_scope_stack) >= 2) {
    jsval_t *saved_this_ptr = (jsval_t *)utarray_back(saved_scope_stack);
    js->this_val = *saved_this_ptr;
    utarray_pop_back(saved_scope_stack);
    
    jsval_t *saved_scope_ptr = (jsval_t *)utarray_back(saved_scope_stack);
    js->scope = *saved_scope_ptr;
    utarray_pop_back(saved_scope_stack);
  }
}

jsval_t call_js_internal(
  struct js *js, const char *fn, jsoff_t fnlen,
  jsval_t closure_scope, jsval_t *bound_args, int bound_argc, jsval_t func_val
) {
  if (saved_scope_stack == NULL) utarray_new(saved_scope_stack, &jsval_icd);
  utarray_push_back(saved_scope_stack, &js->scope);
  utarray_push_back(saved_scope_stack, &js->this_val);
  
  jsval_t target_this = peek_this();
  jsoff_t parent_scope_offset;
  
  if (vtype(closure_scope) == T_OBJ) {
    parent_scope_offset = (jsoff_t) vdata(closure_scope);
  } else {
    parent_scope_offset = (jsoff_t) vdata(js->scope);
  }
  
  if (global_scope_stack == NULL) utarray_new(global_scope_stack, &jsoff_icd);
  jsval_t function_scope = mkobj(js, parent_scope_offset);
  jsoff_t function_scope_offset = (jsoff_t)vdata(function_scope);
  utarray_push_back(global_scope_stack, &function_scope_offset);
  
  const char *caller_code = js->code;
  jsoff_t caller_clen = js->clen;
  jsoff_t caller_pos = js->pos;
  
  UT_array *args_arr;
  utarray_new(args_arr, &jsval_icd);
  
  for (int i = 0; i < bound_argc; i++) { utarray_push_back(args_arr, &bound_args[i]); } 
  caller_pos = skiptonext(caller_code, caller_clen, caller_pos, NULL);
  
  while (caller_pos < caller_clen && caller_code[caller_pos] != ')') {
    bool is_spread = (
      caller_code[caller_pos] == '.' && caller_pos + 2 < caller_clen &&
      caller_code[caller_pos + 1] == '.' && caller_code[caller_pos + 2] == '.'
    );
    if (is_spread) caller_pos += 3;
    js->pos = caller_pos;
    js->consumed = 1;
    jsval_t arg = resolveprop(js, js_expr(js));
    caller_pos = js->pos;
    if (is_spread && vtype(arg) == T_ARR) {
      jsoff_t len = js_arr_len(js, arg);
      for (jsoff_t i = 0; i < len; i++) {
        jsval_t elem = js_arr_get(js, arg, i);
        utarray_push_back(args_arr, &elem);
      }
    } else {
      utarray_push_back(args_arr, &arg);
    }
    caller_pos = skiptonext(caller_code, caller_clen, caller_pos, NULL);
    if (caller_pos < caller_clen && caller_code[caller_pos] == ',') caller_pos++;
    caller_pos = skiptonext(caller_code, caller_clen, caller_pos, NULL);
  }
  js->pos = caller_pos;
  
  jsval_t *args = (jsval_t *)utarray_front(args_arr);
  int argc = (int)utarray_len(args_arr);
  js->scope = function_scope;
  
  parsed_func_t *pf = get_or_parse_func(fn, fnlen);
  if (!pf) {
    utarray_free(args_arr);
    restore_saved_scope(js);
    if (global_scope_stack && utarray_len(global_scope_stack) > 0) utarray_pop_back(global_scope_stack);
    return js_mkerr(js, "failed to parse function");
  }
  
  int argi = 0;
  for (int i = 0; i < pf->param_count; i++) {
    parsed_param_t *pp = (parsed_param_t *)utarray_eltptr(pf->params, (unsigned int)i);
    
    if (pp->is_destruct) {
      jsval_t arg_val = (argi < argc) ? args[argi++] : js_mkundef();
      if (vtype(arg_val) == T_UNDEF && pp->default_len > 0) {
        arg_val = js_eval_str(js, &fn[pp->default_start], pp->default_len);
      }
      jsval_t r = bind_destruct_pattern(js, &fn[pp->pattern_off], pp->pattern_len, arg_val, function_scope);
      if (is_err(r)) {
        utarray_free(args_arr);
        restore_saved_scope(js);
        if (global_scope_stack && utarray_len(global_scope_stack) > 0) utarray_pop_back(global_scope_stack);
        return r;
      }
    } else {
      jsval_t v;
      if (argi < argc) {
        v = args[argi++];
      } else if (pp->default_len > 0) {
        v = js_eval_str(js, &fn[pp->default_start], pp->default_len);
      } else {
        v = js_mkundef();
      }
      jsval_t k = js_mkstr(js, &fn[pp->name_off], pp->name_len);
      if (!is_err(k)) mkprop_fast(js, function_scope, k, v, 0);
    }
  }
  
  if (pf->has_rest && pf->rest_param_len > 0) {
    jsval_t rest_array = mkarr(js);
    if (!is_err(rest_array)) {
      jsoff_t idx = 0;
      while (argi < argc) {
        jsval_t key;
        if (idx < 10 && INTERN_IDX[idx]) {
          key = js_mkstr(js, INTERN_IDX[idx], 1);
        } else {
          char idxstr[16];
          size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
          key = js_mkstr(js, idxstr, idxlen);
        }
        setprop(js, rest_array, key, args[argi++]);
        idx++;
      }
      jsval_t len_key = js_mkstr(js, "length", 6);
      setprop(js, rest_array, len_key, tov((double) idx));
      rest_array = mkval(T_ARR, vdata(rest_array));
      setprop(js, function_scope, js_mkstr(js, &fn[pf->rest_param_start], pf->rest_param_len), rest_array);
    }
  }
  
  bool needs_arguments = code_uses_arguments(&fn[pf->body_start], pf->body_len);
  bool func_strict = pf->is_strict;
  
  if (!func_strict && vtype(func_val) == T_FUNC) {
    jsval_t func_obj = mkval(T_OBJ, vdata(func_val));
    jsval_t strict_slot = get_slot(js, func_obj, SLOT_STRICT);
    func_strict = (vtype(strict_slot) == T_BOOL && vdata(strict_slot) == 1);
  }
  
  if (needs_arguments) {
    setup_arguments(js, function_scope, args, argc, func_strict);
  }
  
   jsval_t slot_name = get_slot(js, func_val, SLOT_NAME);
   if (vtype(slot_name) == T_STR && vtype(func_val) == T_FUNC) {
     jsoff_t len;
     (void)vstr(js, slot_name, &len);
     if (len > 0) mkprop_fast(js, function_scope, slot_name, func_val, CONSTMASK);
   }
  
  if (vtype(func_val) == T_FUNC) {
    jsval_t func_obj = mkval(T_OBJ, vdata(func_val));
    hoist_var_declarations_from_slot(js, function_scope, func_obj);
    jsval_t no_func_decls = get_slot(js, func_obj, SLOT_NO_FUNC_DECLS);
    js->skip_func_hoist = (vtype(no_func_decls) == T_BOOL && vdata(no_func_decls) == 1);
  } else js->skip_func_hoist = false;
  
  if (func_strict && (vtype(target_this) == T_UNDEF || vtype(target_this) == T_NULL ||
      (vtype(target_this) == T_OBJ && vdata(target_this) == 0))) {
    js->this_val = js_mkundef();
  } else js->this_val = target_this;
  
  js->flags = F_CALL | (func_strict ? F_STRICT : 0);
  jsval_t res = js_eval(js, &fn[pf->body_start], pf->body_len);
  js->skip_func_hoist = false;
  
  if (!is_err(res) && !(js->flags & F_RETURN)) res = js_mkundef();
  if (global_scope_stack && utarray_len(global_scope_stack) > 0)  utarray_pop_back(global_scope_stack);
  
  utarray_free(args_arr);
  restore_saved_scope(js);
  
  return res;
}

jsval_t call_js(struct js *js, const char *fn, jsoff_t fnlen, jsval_t closure_scope) {
  return call_js_internal(js, fn, fnlen, closure_scope, NULL, 0, js_mkundef());
}

jsval_t call_js_with_args(struct js *js, jsval_t func, jsval_t *args, int nargs) {
  if (vtype(func) == T_CFUNC) {
    jsval_t (*fn)(struct js *, jsval_t *, int) = (jsval_t(*)(struct js *, jsval_t *, int)) vdata(func);
    return fn(js, args, nargs);
  }
  
  if (vtype(func) != T_FUNC) return js_mkerr(js, "not a function");
  jsval_t func_obj = mkval(T_OBJ, vdata(func));
  
  jsval_t *combined_args = NULL;
  int combined_nargs = nargs;
  int bound_argc = 0;
  
  jsval_t bound_arr = get_slot(js, func_obj, SLOT_BOUND_ARGS);
  if (vtype(bound_arr) == T_ARR) {
    jsoff_t len_off = lkp_interned(js, bound_arr, INTERN_LENGTH, 6);
    if (len_off != 0) {
      jsval_t len_val = resolveprop(js, mkval(T_PROP, len_off));
      if (vtype(len_val) == T_NUM) {
        bound_argc = (int) tod(len_val);
      }
    }
    
    if (bound_argc > 0) {
      combined_nargs = bound_argc + nargs;
      combined_args = (jsval_t *)ant_calloc(sizeof(jsval_t) * combined_nargs);
      if (combined_args) {
        for (int i = 0; i < bound_argc; i++) {
          char idx[16];
          snprintf(idx, sizeof(idx), "%d", i);
          jsoff_t prop_off = lkp(js, bound_arr, idx, strlen(idx));
          combined_args[i] = (prop_off != 0) ? resolveprop(js, mkval(T_PROP, prop_off)) : js_mkundef();
        }
        for (int i = 0; i < nargs; i++) {
          combined_args[bound_argc + i] = args[i];
        }
        args = combined_args;
        nargs = combined_nargs;
      }
    }
  }

  jsval_t cfunc_slot = get_slot(js, func_obj, SLOT_CFUNC);
  if (vtype(cfunc_slot) == T_CFUNC) {
    jsval_t bound_this = get_slot(js, func_obj, SLOT_BOUND_THIS);
    
    jsval_t saved_this = js->this_val;
    if (vtype(bound_this) != T_UNDEF) {
      push_this(bound_this);
      js->this_val = bound_this;
    }
    
    jsval_t saved_func = js->current_func;
    js->current_func = func;
    
    jsval_t (*fn)(struct js *, jsval_t *, int) = (jsval_t(*)(struct js *, jsval_t *, int)) vdata(cfunc_slot);
    jsval_t result = fn(js, args, nargs);
    
    js->current_func = saved_func;
    if (vtype(bound_this) != T_UNDEF) {
      pop_this();
      js->this_val = saved_this;
    }
    
    if (combined_args) free(combined_args);
    return result;
  }
  
  jsoff_t fnlen;
  const char *fn = get_func_code(js, func_obj, &fnlen);
  if (!fn) {
    if (combined_args) free(combined_args);
    return js_mkerr(js, "function has no code");
  }
  
  jsval_t closure_scope = get_slot(js, func_obj, SLOT_SCOPE);
  jsval_t saved_super = js->super_val;
  
  jsval_t func_super = get_slot(js, func_obj, SLOT_SUPER);
  if (vtype(func_super) != T_UNDEF) js->super_val = func_super;
  
  jsval_t captured_this = get_slot(js, func_obj, SLOT_THIS);
  if (vtype(captured_this) != T_UNDEF) {
    pop_this();
    push_this(captured_this);
  }
  
  jsval_t bound_this = get_slot(js, func_obj, SLOT_BOUND_THIS);
  if (vtype(bound_this) != T_UNDEF) {
    pop_this();
    push_this(bound_this);
  }
  
  jsval_t result = call_js_code_with_args(js, fn, fnlen, closure_scope, args, nargs, func);
  js->super_val = saved_super;
  if (combined_args) free(combined_args);
  
  return result;
}

jsval_t call_js_code_with_args(struct js *js, const char *fn, jsoff_t fnlen, jsval_t closure_scope, jsval_t *args, int nargs, jsval_t func_val) {
  jsoff_t parent_scope_offset;
  if (vtype(closure_scope) == T_OBJ) {
    parent_scope_offset = (jsoff_t) vdata(closure_scope);
  } else parent_scope_offset = (jsoff_t) vdata(js->scope);
  
  jsval_t saved_scope = js->scope;
  if (global_scope_stack == NULL) utarray_new(global_scope_stack, &jsoff_icd);
  utarray_push_back(global_scope_stack, &parent_scope_offset);
   jsval_t function_scope = mkobj(js, parent_scope_offset);
   js->scope = function_scope;
   
   jsval_t slot_name = get_slot(js, func_val, SLOT_NAME);
   if (vtype(slot_name) == T_STR && vtype(func_val) == T_FUNC) {
     jsoff_t len; vstr(js, slot_name, &len);
     if (len > 0) mkprop(js, function_scope, slot_name, func_val, CONSTMASK);
   }
   
  jsval_t func_obj = mkval(T_OBJ, vdata(func_val));
  hoist_var_declarations_from_slot(js, function_scope, func_obj);
  
  jsoff_t fnpos = 1;
  int arg_idx = 0;
  bool has_rest = false;
  jsoff_t rest_param_start = 0, rest_param_len = 0;
  
  while (fnpos < fnlen) {
    fnpos = skiptonext(fn, fnlen, fnpos, NULL);
    if (fnpos < fnlen && fn[fnpos] == ')') break;
    
    bool is_rest = false;
    if (fnpos + 3 < fnlen && fn[fnpos] == '.' && fn[fnpos + 1] == '.' && fn[fnpos + 2] == '.') {
      is_rest = true;
      has_rest = true;
      fnpos += 3;
      fnpos = skiptonext(fn, fnlen, fnpos, NULL);
    }
    
    jsoff_t identlen = 0;
    uint8_t tok = parseident(&fn[fnpos], fnlen - fnpos, &identlen);
    
    fnpos = skiptonext(fn, fnlen, fnpos, NULL);
    if (tok != TOK_IDENTIFIER && fnpos < fnlen && (fn[fnpos] == '{' || fn[fnpos] == '[')) {
      char bracket_open = fn[fnpos];
      char bracket_close = (bracket_open == '{') ? '}' : ']';
      jsoff_t pattern_start = fnpos;
      int depth = 1;
      fnpos++;
      while (fnpos < fnlen && depth > 0) {
        if (fn[fnpos] == bracket_open) depth++;
        else if (fn[fnpos] == bracket_close) depth--;
        fnpos++;
      }
      jsoff_t pattern_len = fnpos - pattern_start;
      
      jsval_t arg_val = (arg_idx < nargs) ? args[arg_idx] : js_mkundef();
      jsval_t r = bind_destruct_pattern(js, &fn[pattern_start], pattern_len, arg_val, function_scope);
      if (is_err(r)) return r;
      
      arg_idx++;
      fnpos = skiptonext(fn, fnlen, fnpos, NULL);
      if (fnpos < fnlen && fn[fnpos] == ',') fnpos++;
      continue;
    }
    
    if (tok != TOK_IDENTIFIER) break;
    
    if (is_rest) {
      rest_param_start = fnpos;
      rest_param_len = identlen;
      fnpos = skiptonext(fn, fnlen, fnpos + identlen, NULL);
      break;
    }
    
    jsoff_t param_name_pos = fnpos;
    jsoff_t default_start = 0, default_len = 0;
    fnpos = extract_default_param_value(fn, fnlen, fnpos + identlen, &default_start, &default_len);
    
    jsval_t v;
    if (arg_idx < nargs) {
      v = args[arg_idx];
    } else if (default_len > 0) {
      v = js_eval_str(js, &fn[default_start], default_len);
    } else {
      v = js_mkundef();
    }
    setprop(js, function_scope, js_mkstr(js, &fn[param_name_pos], identlen), v);
    arg_idx++;
    if (fnpos < fnlen && fn[fnpos] == ',') fnpos++;
  }
  
  if (has_rest && rest_param_len > 0) {
    jsval_t rest_array = mkarr(js);
    if (!is_err(rest_array)) {
      jsoff_t idx = 0;
      while (arg_idx < nargs) {
        char idxstr[16];
        size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
        jsval_t key = js_mkstr(js, idxstr, idxlen);
        setprop(js, rest_array, key, args[arg_idx]);
        idx++;
        arg_idx++;
      }
      jsval_t len_key = js_mkstr(js, "length", 6);
      setprop(js, rest_array, len_key, tov((double) idx));
      rest_array = mkval(T_ARR, vdata(rest_array));
      setprop(js, function_scope, js_mkstr(js, &fn[rest_param_start], rest_param_len), rest_array);
    }
  }
  
  if (fnpos < fnlen && fn[fnpos] == ')') fnpos++;
  fnpos = skiptonext(fn, fnlen, fnpos, NULL);
  if (fnpos >= fnlen) return js_mkerr(js, "unexpected end of function");
  if (fn[fnpos] == '{') fnpos++;
  jsoff_t body_len = fnlen - fnpos - 1;
  
  bool func_strict = is_strict_function_body(&fn[fnpos], body_len);
  if (code_uses_arguments(&fn[fnpos], body_len)) {
    setup_arguments(js, function_scope, args, nargs, func_strict);
  }
  
  jsval_t saved_this = js->this_val;
  jsval_t target_this = peek_this();
  
  if (func_strict && (vtype(target_this) == T_UNDEF || vtype(target_this) == T_NULL ||
      (vtype(target_this) == T_OBJ && vdata(target_this) == 0))) {
    js->this_val = js_mkundef();
  } else {
    js->this_val = target_this;
  }
  js->flags = F_CALL | (func_strict ? F_STRICT : 0);
  
  jsval_t res = js_eval(js, &fn[fnpos], body_len);
  if (!is_err(res) && !(js->flags & F_RETURN)) res = js_mkundef();
  
  js->this_val = saved_this;
  if (global_scope_stack && utarray_len(global_scope_stack) > 0) utarray_pop_back(global_scope_stack);
  
  js->scope = saved_scope;  
  return res;
}

static jsval_t call_ffi(struct js *js, unsigned int func_index) {
  UT_array *args;
  utarray_new(args, &jsval_icd);
  jsval_t err, res;
  
  int argc = parse_call_args(js, args, &err);
  if (argc < 0) { utarray_free(args); return err; }
  
  jsval_t *argv = (jsval_t *)utarray_front(args);
  res = ffi_call_by_index(js, func_index, argv, argc);
  
  utarray_free(args);
  return res;
}

static jsval_t do_call_op(struct js *js, jsval_t func, jsval_t args) {
  if (js_stack_overflow(js)) {
    return js_mkerr_typed(js, JS_ERR_RANGE | JS_ERR_NO_STACK, "Maximum call stack size exceeded");
  }
  
  if (vtype(args) != T_CODEREF) return js_mkerr(js, "bad call");
  if (vtype(func) != T_FUNC && vtype(func) != T_CFUNC && vtype(func) != T_FFI) return js_mkerr(js, "calling non-function");
  
  if (vtype(func) == T_FFI) {
    const char *code = js->code;
    jsoff_t clen = js->clen, pos = js->pos;
    uint8_t tok = js->tok, flags = js->flags;
    
    js->code = &js->code[coderefoff(args)];
    js->clen = codereflen(args);
    js->pos = skiptonext(js->code, js->clen, 0, NULL);
    
    jsval_t res = call_ffi(js, (unsigned int)vdata(func));
    
    js->code = code; js->clen = clen; js->pos = pos;
    js->flags = (flags & ~F_THROW) | (js->flags & F_THROW);
    js->tok = tok;
    js->consumed = 1;
    return res;
  }
  
  jsval_t target_this = peek_this();
  jsval_t target_proto = (vtype(target_this) == T_OBJ) ? get_slot(js, target_this, SLOT_PROTO) : js_mkundef();
  
  if (vtype(func) == T_FUNC && vtype(target_this) == T_OBJ) {
    if (vtype(target_proto) == T_UNDEF) {
      jsval_t func_obj = mkval(T_OBJ, vdata(func));
      
      jsval_t target_func = get_slot(js, func_obj, SLOT_TARGET_FUNC);
      jsval_t proto_source = func_obj;
      if (vtype(target_func) == T_FUNC) {
        proto_source = mkval(T_OBJ, vdata(target_func));
      }
      
      jsoff_t proto_off = lkp_interned(js, proto_source, INTERN_PROTOTYPE, 9);
      if (proto_off != 0) {
        jsval_t proto = resolveprop(js, mkval(T_PROP, proto_off));
        if (vtype(proto) == T_OBJ) set_proto(js, target_this, proto);
      }
    }
  }
  
  const char *code = js->code;
  jsoff_t clen = js->clen, pos = js->pos;
  
  js->code = &js->code[coderefoff(args)];
  js->clen = codereflen(args);
  js->pos = skiptonext(js->code, js->clen, 0, NULL);
  
  uint8_t tok = js->tok, flags = js->flags;
  jsval_t res = js_mkundef();
  
  if (vtype(func) == T_FUNC) {
    jsval_t func_obj = mkval(T_OBJ, vdata(func));
    jsval_t cfunc_slot = get_slot(js, func_obj, SLOT_CFUNC);
    if (vtype(cfunc_slot) == T_CFUNC) {
      jsval_t bound_this_slot = get_slot(js, func_obj, SLOT_BOUND_THIS);
      bool has_bound_this = vtype(bound_this_slot) != T_UNDEF;
      
      if (has_bound_this) {
        pop_this();
        push_this(bound_this_slot);
      }
      
      jsval_t saved_func = js->current_func;
      js->current_func = func;
      
      int bound_argc;
      jsval_t *bound_args = resolve_bound_args(js, func_obj, NULL, 0, &bound_argc);
      
      if (!bound_args) {
        res = call_c(js, (jsval_t(*)(struct js *, jsval_t *, int)) vdata(cfunc_slot));
      } else {
        UT_array *args_arr;
        utarray_new(args_arr, &jsval_icd);
        for (int i = 0; i < bound_argc; i++) utarray_push_back(args_arr, &bound_args[i]);
        free(bound_args);
        
        jsval_t err;
        int call_argc = parse_call_args(js, args_arr, &err);
        if (call_argc < 0) {
          utarray_free(args_arr);
          js->current_func = saved_func;
          if (has_bound_this) pop_this();
          return err;
        }
        
        jsval_t *argv = (jsval_t *)utarray_front(args_arr);
        int total_argc = (int)utarray_len(args_arr);
        jsval_t saved_this = js->this_val;
        js->this_val = peek_this();
        res = ((jsval_t(*)(struct js *, jsval_t *, int)) vdata(cfunc_slot))(js, argv, total_argc);
        js->this_val = saved_this;
        utarray_free(args_arr);
      }
      
      js->current_func = saved_func;
      
      if (has_bound_this) {
        pop_this();
        push_this(target_this);
      }
    } else {
      jsval_t builtin_slot = get_slot(js, func_obj, SLOT_BUILTIN);
      if (vtype(builtin_slot) == T_NUM && (int)tod(builtin_slot) == BUILTIN_OBJECT) res = call_c(js, builtin_Object); else {
      jsoff_t fnlen;
      
      const char *code_str = get_func_code(js, func_obj, &fnlen);
      if (!code_str) return js_mkerr(js, "function has no code");
      
      jsval_t closure_scope = get_slot(js, func_obj, SLOT_SCOPE);
      jsval_t async_slot = get_slot(js, func_obj, SLOT_ASYNC);
      bool is_async = (async_slot == js_true);
      
      jsval_t captured_this = js_mkundef();
      bool is_arrow = false;
      bool is_bound = false;
      
      jsval_t this_slot = get_slot(js, func_obj, SLOT_THIS);
      if (vtype(this_slot) != T_UNDEF) {
        captured_this = this_slot; is_arrow = true;
      }
      
      jsval_t bound_this_slot = get_slot(js, func_obj, SLOT_BOUND_THIS);
      if (vtype(bound_this_slot) != T_UNDEF && vtype(js->new_target) == T_UNDEF) {
        captured_this = bound_this_slot; is_bound = true;
      }
      
      int bound_argc;
      jsval_t *bound_args = resolve_bound_args(js, func_obj, NULL, 0, &bound_argc);
      
      jsval_t nfe_name_val = js_mkundef();
      jsval_t slot_name = get_slot(js, func_obj, SLOT_NAME);
      
      if (vtype(slot_name) == T_STR) nfe_name_val = slot_name; else {
        jsoff_t nfe_name_off = lkp(js, func_obj, "name", 4);
        if (nfe_name_off != 0) nfe_name_val = resolveprop(js, mkval(T_PROP, nfe_name_off));
      }
      
      const char *func_name = NULL;
      
      if (vtype(nfe_name_val) == T_STR) {
        jsoff_t name_len, name_offset = vstr(js, nfe_name_val, &name_len);
        func_name = (const char *)&js->mem[name_offset];
      }
      
      const char *final_name = func_name ? func_name : "<anonymous>";
      push_call_frame(js->filename, final_name,  code, (uint32_t) pos);
      
      jsval_t saved_func = js->current_func;
      js->current_func = func;
      
      jsval_t saved_super = js->super_val;
      jsval_t func_super = get_slot(js, func_obj, SLOT_SUPER);
      if (vtype(func_super) != T_UNDEF) js->super_val = func_super;
      
      if (is_arrow || is_bound) {
        pop_this();
        push_this(captured_this);
      }
      
      if (get_slot(js, func_obj, SLOT_DEFAULT_CTOR) == js_true) {
        jsval_t super_ctor = js->super_val;
        uint8_t st = vtype(super_ctor);
        if (st == T_FUNC || st == T_CFUNC) {
          js->code = code; js->clen = clen; js->pos = pos;
          res = do_call_op(js, super_ctor, args);
          js->super_val = saved_super;
          js->current_func = saved_func;
          pop_call_frame(); goto restore_state;
        }
      }
      
      jsval_t count_val = get_slot(js, func_obj, SLOT_FIELD_COUNT);
      if (vtype(count_val) != T_NUM || vtype(target_this) != T_OBJ) goto skip_fields;
      
      int field_count = (int)tod(count_val);
      jsval_t src_val = get_slot(js, func_obj, SLOT_SOURCE);
      jsval_t fields_meta = get_slot(js, func_obj, SLOT_FIELDS);
      if (vtype(src_val) == T_UNDEF || vtype(fields_meta) == T_UNDEF) goto skip_fields;
      
      if (vtype(src_val) != T_CFUNC) goto skip_fields;
      const char *source = (const char *)vdata(src_val);
      
      jsoff_t meta_len, meta_ptr_off = vstr(js, fields_meta, &meta_len);
      const jsoff_t *metadata = (const jsoff_t *)(&js->mem[meta_ptr_off]);
      
      for (int i = 0; i < field_count; i++) {
        jsoff_t name_off = metadata[i * 4 + 0];
        jsoff_t name_len = metadata[i * 4 + 1];
        jsoff_t init_start = metadata[i * 4 + 2];
        jsoff_t init_end = metadata[i * 4 + 3];
        
        jsval_t fname = js_mkstr(js, &source[name_off], name_len);
        if (is_err(fname)) {
          js->current_func = saved_func;
          pop_call_frame();
          return fname;
        }
        
        jsval_t field_val = js_mkundef();
        if (init_start > 0 && init_end > init_start) {
          field_val = js_eval_str(js, &source[init_start], init_end - init_start);
          field_val = resolveprop(js, field_val);
        }
        
        jsval_t set_res = setprop(js, target_this, fname, field_val);
        if (is_err(set_res)) {
          js->current_func = saved_func;
          pop_call_frame();
          return set_res;
        }
      }
skip_fields:
        if (is_async) {
          UT_array *call_args;
          utarray_new(call_args, &jsval_icd);
          for (int i = 0; i < bound_argc; i++) utarray_push_back(call_args, &bound_args[i]);
          jsval_t err;
          int call_argc = parse_call_args(js, call_args, &err);
          if (call_argc < 0) {
            utarray_free(call_args);
            pop_call_frame();
            if (bound_args) free(bound_args);
            js->super_val = saved_super;
            js->current_func = saved_func;
            return err;
          }
          jsval_t *argv = (jsval_t *)utarray_front(call_args);
          int argc = (int)utarray_len(call_args);
          res = start_async_in_coroutine(js, code_str, fnlen, closure_scope, argv, argc);
          utarray_free(call_args);
        } else res = call_js_internal(js, code_str, fnlen, closure_scope, bound_args, bound_argc, func);
        pop_call_frame();
        if (bound_args) free(bound_args);
        js->super_val = saved_super;
        js->current_func = saved_func;
      }
    }
  } else {
    res = call_c(js, (jsval_t(*)(struct js *, jsval_t *, int)) vdata(func));
  }
  
restore_state:
  js->code = code, js->clen = clen, js->pos = pos;
  js->flags = (flags & ~F_THROW) | (js->flags & F_THROW);
  js->tok = tok;
  js->consumed = 1;
  
  return res;
}

static jsval_t js_call_toString(struct js *js, jsval_t value) {
  jsoff_t ts_off = lkp(js, value, "toString", 8);
  if (ts_off == 0) ts_off = lkp_proto(js, value, "toString", 8);
  if (ts_off == 0) goto fallback;
  
  jsval_t ts_func = resolveprop(js, mkval(T_PROP, ts_off));
  uint8_t ft = vtype(ts_func);
  if (ft != T_FUNC && ft != T_CFUNC) goto fallback;
  
  jsval_t saved_this = js->this_val;
  js->this_val = value;
  jsval_t result;
  
  if (ft == T_CFUNC) {
    result = ((jsval_t (*)(struct js *, jsval_t *, int))vdata(ts_func))(js, NULL, 0);
  } else {
    jsval_t func_obj = mkval(T_OBJ, vdata(ts_func));
    jsoff_t fnlen;
    const char *code_str = get_func_code(js, func_obj, &fnlen);
    if (!code_str) goto restore_fallback;
    
    jsval_t closure_scope = get_slot(js, func_obj, SLOT_SCOPE);
    if (vtype(closure_scope) == T_UNDEF) closure_scope = js->scope;
    
    result = call_js(js, code_str, fnlen, closure_scope);
  }
  
  js->this_val = saved_this;
  if (vtype(result) == T_STR) return result;
  
  uint8_t rtype = vtype(result);
  if (rtype != T_OBJ && rtype != T_ARR && rtype != T_FUNC) {
    char buf[256];
    size_t len = tostr(js, result, buf, sizeof(buf));
    return js_mkstr(js, buf, len);
  }
  
restore_fallback:
  js->this_val = saved_this;
fallback:;
  char buf[4096];
  size_t len = tostr(js, value, buf, sizeof(buf));
  return js_mkstr(js, buf, len);
}

static jsval_t js_call_valueOf(struct js *js, jsval_t value) {
  jsoff_t off = lkp(js, value, "valueOf", 7);
  if (off == 0) off = lkp_proto(js, value, "valueOf", 7);
  if (off == 0) return value;
  
  jsval_t fn = resolveprop(js, mkval(T_PROP, off));
  uint8_t ft = vtype(fn);
  if (ft != T_FUNC && ft != T_CFUNC) return value;
  
  jsval_t saved = js->this_val;
  js->this_val = value;
  jsval_t result;
  
  if (ft == T_CFUNC) {
    result = ((jsval_t (*)(struct js *, jsval_t *, int))vdata(fn))(js, NULL, 0);
  } else {
    jsval_t func_obj = mkval(T_OBJ, vdata(fn));
    jsoff_t fnlen;
    const char *code_str = get_func_code(js, func_obj, &fnlen);
    if (!code_str) { js->this_val = saved; return value; }
    
    jsval_t closure_scope = get_slot(js, func_obj, SLOT_SCOPE);
    if (vtype(closure_scope) == T_UNDEF) closure_scope = js->scope;
    
    result = call_js(js, code_str, fnlen, closure_scope);
  }
  
  js->this_val = saved;
  return result;
}

static inline bool strict_eq_values(struct js *js, jsval_t l, jsval_t r) {
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

static inline jsval_t coerce_to_str(struct js *js, jsval_t v) {
  if (vtype(v) == T_STR) return v;
  if (vtype(v) == T_ARR) {
    char buf[1024];
    size_t len = array_to_string(js, v, buf, sizeof(buf));
    return js_mkstr(js, buf, len);
  }
  return js_tostring_val(js, v);
}

static jsval_t do_op(struct js *js, uint8_t op, jsval_t lhs, jsval_t rhs) {
  if (js->flags & F_NOEXEC) return 0;
  
  jsval_t l = is_assign(op) ? lhs : resolveprop(js, lhs);
  jsval_t r = resolveprop(js, rhs);
  
  if (is_err(l)) return l;
  if (is_err(r)) return r;
  
  if (is_assign(op) && vtype(lhs) != T_PROP && vtype(lhs) != T_PROPREF) {
    if (!(js->flags & F_STRICT) && vtype(lhs) == T_UNDEF) return r;
    if (!(js->flags & F_STRICT) && vtype(lhs) == T_CODEREF && op == TOK_ASSIGN) {
      jsoff_t id_off = coderefoff(lhs), id_len = codereflen(lhs);
      jsval_t global_scope = js->scope;
      while (vdata(upper(js, global_scope)) != 0) global_scope = upper(js, global_scope);
      jsval_t key = js_mkstr(js, &js->code[id_off], id_len);
      if (is_err(key)) return key;
      jsval_t prop = setprop(js, global_scope, key, r);
      return is_err(prop) ? prop : r;
    }
    return js_mkerr_typed(js, (js->flags & F_STRICT) && vtype(lhs) == T_UNDEF ? JS_ERR_TYPE : JS_ERR_SYNTAX,
      (js->flags & F_STRICT) && vtype(lhs) == T_UNDEF ? "Cannot create property on primitive value" : "Invalid left-hand side in assignment");
  }

#define L(tok) [tok] = &&L_##tok
  static const void *dispatch[TOK_MAX] = {
    L(TOK_TYPEOF), L(TOK_VOID), L(TOK_INSTANCEOF), L(TOK_IN),
    L(TOK_CALL), L(TOK_BRACKET), L(TOK_ASSIGN), L(TOK_DOT),
    L(TOK_OPTIONAL_CHAIN), L(TOK_POSTINC), L(TOK_POSTDEC),
    L(TOK_NOT), L(TOK_UMINUS), L(TOK_UPLUS), L(TOK_SEQ),
    L(TOK_SNE), L(TOK_EQ), L(TOK_NE), L(TOK_PLUS),
    L(TOK_MINUS), L(TOK_MUL), L(TOK_DIV), L(TOK_REM),
    L(TOK_EXP), L(TOK_LT), L(TOK_LE), L(TOK_GT), L(TOK_GE),
    L(TOK_XOR), L(TOK_AND), L(TOK_OR), L(TOK_TILDA),
    L(TOK_SHL), L(TOK_SHR), L(TOK_ZSHR),
  };
#undef L

  if (op < TOK_MAX && dispatch[op]) goto *dispatch[op];
  goto L_default;

  L_TOK_TYPEOF: {
    const char *ts = typestr(vtype(r));
    return js_mkstr(js, ts, strlen(ts));
  }
  L_TOK_VOID:           return js_mkundef();
  L_TOK_INSTANCEOF:     return do_instanceof(js, l, r);
  L_TOK_IN:             return do_in(js, l, r);
  L_TOK_CALL:           return do_call_op(js, l, r);
  L_TOK_BRACKET:        return do_bracket_op(js, l, rhs);
  L_TOK_ASSIGN:         return assign(js, lhs, r);
  L_TOK_DOT:            return do_dot_op(js, l, r);
  L_TOK_OPTIONAL_CHAIN: return do_optional_chain_op(js, l, r);

  L_TOK_POSTINC:
  L_TOK_POSTDEC: {
    uint8_t lhs_type = vtype(lhs);
    if (lhs_type != T_PROP && lhs_type != T_PROPREF)
      return js_mkerr_typed(js, JS_ERR_SYNTAX, "Invalid left-hand side expression in postfix operation");
    do_assign_op(js, op == TOK_POSTINC ? TOK_PLUS_ASSIGN : TOK_MINUS_ASSIGN, lhs, tov(1));
    return l;
  }

  L_TOK_NOT: return mkval(T_BOOL, !js_truthy(js, r));
  
  L_TOK_UMINUS:
    if (vtype(r) == T_BIGINT) return bigint_neg(js, r);
    return tov(-js_to_number(js, r));
  
  L_TOK_UPLUS:
    if (vtype(r) == T_BIGINT) return js_mkerr(js, "Cannot convert a BigInt value to a number");
    return tov(js_to_number(js, r));

  L_TOK_TILDA: return tov((double)(~js_to_int32(js_to_number(js, r))));

  L_TOK_SEQ:
  L_TOK_SNE: {
    bool eq = strict_eq_values(js, l, r);
    return mkval(T_BOOL, op == TOK_SEQ ? eq : !eq);
  }

  L_TOK_EQ:
  L_TOK_NE: {
    bool eq = false;
    uint8_t lt = vtype(l), rtype = vtype(r);

    if ((lt == T_NULL && rtype == T_NULL) || (lt == T_UNDEF && rtype == T_UNDEF) ||
        (lt == T_UNDEF && rtype == T_NULL) || (lt == T_NULL && rtype == T_UNDEF)) {
      eq = true;
    } else if (lt == T_NULL || rtype == T_NULL || lt == T_UNDEF || rtype == T_UNDEF) {
      eq = false;
    } else if (lt == rtype) {
      eq = strict_eq_values(js, l, r);
    } else if ((lt == T_BIGINT && rtype == T_NUM) || (lt == T_NUM && rtype == T_BIGINT)) {
      double num_val = lt == T_NUM ? tod(l) : tod(r);
      jsval_t bigint_val = lt == T_BIGINT ? l : r;
      if (isfinite(num_val) && num_val == trunc(num_val)) {
        bool neg = num_val < 0;
        if (neg) num_val = -num_val;
        char buf[64];
        snprintf(buf, sizeof(buf), "%.0f", num_val);
        eq = bigint_compare(js, bigint_val, js_mkbigint(js, buf, strlen(buf), neg)) == 0;
      }
    } else if (lt == T_BOOL) {
      return do_op(js, op, tov(vdata(l) ? 1.0 : 0.0), r);
    } else if (rtype == T_BOOL) {
      return do_op(js, op, l, tov(vdata(r) ? 1.0 : 0.0));
    } else if ((lt == T_NUM && rtype == T_STR) || (lt == T_STR && rtype == T_NUM)) {
      eq = js_to_number(js, l) == js_to_number(js, r);
    } else if (lt == T_ARR || lt == T_OBJ) {
      jsval_t l_prim = js_tostring_val(js, l);
      if (!is_err(l_prim)) return do_op(js, op, l_prim, r);
    } else if (rtype == T_ARR || rtype == T_OBJ) {
      jsval_t r_prim = js_tostring_val(js, r);
      if (!is_err(r_prim)) return do_op(js, op, l, r_prim);
    }
    return mkval(T_BOOL, op == TOK_EQ ? eq : !eq);
  }

  L_TOK_PLUS: {
    if (vtype(l) == T_NUM && vtype(r) == T_NUM) return tov(tod(l) + tod(r));
    jsval_t lu = unwrap_primitive(js, l);
    jsval_t ru = unwrap_primitive(js, r);
    if (vtype(lu) == T_BIGINT && vtype(ru) == T_BIGINT) return bigint_add(js, lu, ru);
    if (vtype(lu) == T_BIGINT || vtype(ru) == T_BIGINT) return js_mkerr(js, "Cannot mix BigInt value and other types");
    if (is_non_numeric(lu) || is_non_numeric(ru) || (vtype(lu) == T_STR && vtype(ru) == T_STR)) {
      jsval_t l_str = coerce_to_str(js, l);
      if (is_err(l_str)) return l_str;
      jsval_t r_str = coerce_to_str(js, r);
      if (is_err(r_str)) return r_str;
      return do_string_op(js, op, l_str, r_str);
    }
    return tov(js_to_number(js, l) + js_to_number(js, r));
  }

  L_TOK_MINUS:
  L_TOK_MUL:
  L_TOK_DIV:
  L_TOK_REM:
  L_TOK_EXP: {
    uint8_t lt = vtype(l), rtype = vtype(r);
    if (lt == T_NUM && rtype == T_NUM) {
      double a = tod(l), b = tod(r);
      switch (op) {
        case TOK_MINUS: return tov(a - b);
        case TOK_MUL:   return tov(a * b);
        case TOK_DIV:   return tov(a / b);
        case TOK_REM:   return tov(a - b * ((double)(long)(a / b)));
        case TOK_EXP:   return tov(pow(a, b));
      }
    }
    if (lt == T_BIGINT && rtype == T_BIGINT) {
      switch (op) {
        case TOK_MINUS: return bigint_sub(js, l, r);
        case TOK_MUL:   return bigint_mul(js, l, r);
        case TOK_DIV:   return bigint_div(js, l, r);
        case TOK_REM:   return bigint_mod(js, l, r);
        case TOK_EXP:   return bigint_exp(js, l, r);
      }
    }
    if (lt == T_BIGINT || rtype == T_BIGINT)
      return js_mkerr(js, "Cannot mix BigInt value and other types");
    double a = js_to_number(js, l), b = js_to_number(js, r);
    switch (op) {
      case TOK_MINUS: return tov(a - b);
      case TOK_MUL:   return tov(a * b);
      case TOK_DIV:   return tov(a / b);
      case TOK_REM:   return tov(a - b * ((double)(long)(a / b)));
      case TOK_EXP:   return tov(pow(a, b));
    }
  }

  L_TOK_LT:
  L_TOK_LE:
  L_TOK_GT:
  L_TOK_GE: {
    uint8_t lt = vtype(l), rtype = vtype(r);
    if (lt == T_NUM && rtype == T_NUM) {
      double a = tod(l), b = tod(r);
      switch (op) {
        case TOK_LT: return mkval(T_BOOL, a < b);
        case TOK_LE: return mkval(T_BOOL, a <= b);
        case TOK_GT: return mkval(T_BOOL, a > b);
        case TOK_GE: return mkval(T_BOOL, a >= b);
      }
    }
    if (lt == T_BIGINT && rtype == T_BIGINT) {
      int cmp = bigint_compare(js, l, r);
      switch (op) {
        case TOK_LT: return mkval(T_BOOL, cmp < 0);
        case TOK_LE: return mkval(T_BOOL, cmp <= 0);
        case TOK_GT: return mkval(T_BOOL, cmp > 0);
        case TOK_GE: return mkval(T_BOOL, cmp >= 0);
      }
    }
    if (lt == T_BIGINT || rtype == T_BIGINT)
      return js_mkerr(js, "Cannot mix BigInt value and other types");
    if (lt == T_STR && rtype == T_STR)
      return do_string_op(js, op, l, r);
    double a = js_to_number(js, l), b = js_to_number(js, r);
    switch (op) {
      case TOK_LT: return mkval(T_BOOL, a < b);
      case TOK_LE: return mkval(T_BOOL, a <= b);
      case TOK_GT: return mkval(T_BOOL, a > b);
      case TOK_GE: return mkval(T_BOOL, a >= b);
    }
  }

  L_TOK_XOR:
  L_TOK_AND:
  L_TOK_OR:
  L_TOK_SHL:
  L_TOK_SHR:
  L_TOK_ZSHR: {
    uint8_t lt = vtype(l), rtype = vtype(r);
    if (lt == T_BIGINT || rtype == T_BIGINT)
      return js_mkerr(js, "Cannot mix BigInt value and other types");
    int32_t ai = (lt == T_NUM) ? js_to_int32(tod(l)) : js_to_int32(js_to_number(js, l));
    uint32_t bi = (rtype == T_NUM) ? js_to_uint32(tod(r)) : js_to_uint32(js_to_number(js, r));
    switch (op) {
      case TOK_XOR:  return tov((double)(ai ^ (int32_t)bi));
      case TOK_AND:  return tov((double)(ai & (int32_t)bi));
      case TOK_OR:   return tov((double)(ai | (int32_t)bi));
      case TOK_SHL:  return tov((double)(ai << (bi & 0x1f)));
      case TOK_SHR:  return tov((double)(ai >> (bi & 0x1f)));
      case TOK_ZSHR: return tov((double)((uint32_t)ai >> (bi & 0x1f)));
    }
  }

  L_default:
    if (is_assign(op)) return do_assign_op(js, op, lhs, r);
    return js_mkerr(js, "unknown op %d", (int)op);
}

static jsval_t js_template_literal(struct js *js) {
  uint8_t *in = (uint8_t *) &js->code[js->toff];
  size_t template_len = js->tlen;
  
  size_t n = 1; 
  jsval_t parts[64]; 
  int part_count = 0;
  
  while (n < template_len - 1 && part_count < 64) {
    size_t part_start = n;
    while (n < template_len - 1) {
      if (in[n] == '\\' && n + 1 < template_len - 1) {
        n += 2;
        continue;
      }
      if (in[n] == '$' && n + 1 < template_len - 1 && in[n + 1] == '{') {
        break;
      }
      n++;
    }
    if (n > part_start || (n == part_start && (n >= template_len - 1 || in[n] != '$'))) {
      size_t part_len = n - part_start;
      size_t needed = sizeof(jsoff_t) + part_len;
      if (js->brk + needed > js->size) {
        if (!js_try_grow_memory(js, needed)) return js_mkerr(js, "oom");
      }
      uint8_t *out = &js->mem[js->brk + sizeof(jsoff_t)];
      size_t out_len = 0;
      
      for (size_t i = part_start; i < n; i++) {
        if (in[i] == '\\' && i + 1 < n) {
          i++;
          if (in[i] == 'n') out[out_len++] = '\n';
          else if (in[i] == 't') out[out_len++] = '\t';
          else if (in[i] == 'r') out[out_len++] = '\r';
          else if (in[i] == '\\') out[out_len++] = '\\';
          else if (in[i] == '`') out[out_len++] = '`';
          else out[out_len++] = in[i];
        } else {
          out[out_len++] = in[i];
        }
      }
      
      parts[part_count++] = js_mkstr(js, NULL, out_len);
    }
    
    if (n < template_len - 1 && in[n] == '$' && in[n + 1] == '{') {
      n += 2;
      size_t expr_start = n;
      int brace_count = 1;
      
      while (n < template_len - 1 && brace_count > 0) {
        if (in[n] == '{') brace_count++;
        else if (in[n] == '}') brace_count--;
        if (brace_count > 0) n++;
      }
      
      if (brace_count != 0) return js_mkerr_typed(js, JS_ERR_SYNTAX, "unclosed ${");
      
      jsval_t expr_result = js_eval_str(js, (const char *)&in[expr_start], (jsoff_t)(n - expr_start));
      if (is_err(expr_result)) return expr_result;
      expr_result = resolveprop(js, expr_result);

      if (vtype(expr_result) != T_STR) {
        const char *str = js_str(js, expr_result);
        expr_result = js_mkstr(js, str, strlen(str));
      }
      
      parts[part_count++] = expr_result;
      n++;
    }
  }
  
  if (part_count == 0) return js_mkstr(js, "", 0);
  if (part_count == 1) return parts[0];
  
  size_t total_len = 0;
  for (int i = 0; i < part_count; i++) {
    if (vtype(parts[i]) == T_STR) {
      total_len += vstrlen(js, parts[i]);
    }
  }
  
  jsval_t result = js_mkstr(js, NULL, total_len);
  if (is_err(result)) return result;
  
  jsoff_t result_len, result_off = vstr(js, result, &result_len);
  size_t pos = 0;
  
  for (int i = 0; i < part_count; i++) {
    if (vtype(parts[i]) == T_STR) {
      jsoff_t part_len, part_off = vstr(js, parts[i], &part_len);
      memmove(&js->mem[result_off + pos], &js->mem[part_off], part_len);
      pos += part_len;
    }
  }
  
  return result;
}

static jsval_t js_tagged_template(struct js *js, jsval_t tag_func) {
  if (js->flags & F_NOEXEC) return js_mkundef();
  
  js_parse_state_t saved;
  JS_SAVE_STATE(js, saved);
  
  uint8_t *in = (uint8_t *) &js->code[js->toff];
  size_t template_len = js->tlen;
  jsval_t strings[64], values[64];
  int string_count = 0, value_count = 0;
  size_t n = 1;
  
  while (n < template_len - 1) {
    size_t part_start = n;
    
    while (n < template_len - 1 && !(in[n] == '$' && n + 1 < template_len - 1 && in[n + 1] == '{')) {
      if (in[n] == '\\' && n + 1 < template_len - 1) n += 2;
      else n++;
    }
    
    size_t out_len = 0;
    size_t needed = sizeof(jsoff_t) + (n - part_start);
    if (js->brk + needed > js->size) {
      if (!js_try_grow_memory(js, needed)) {
        return js_mkerr(js, "oom");
      }
    }
    uint8_t *out = &js->mem[js->brk + sizeof(jsoff_t)];
    
    for (size_t i = part_start; i < n; i++) {
      if (in[i] == '\\' && i + 1 < n) {
        i++;
        if (in[i] == 'n') out[out_len++] = '\n';
        else if (in[i] == 't') out[out_len++] = '\t';
        else if (in[i] == 'r') out[out_len++] = '\r';
        else if (in[i] == '\\') out[out_len++] = '\\';
        else if (in[i] == '`') out[out_len++] = '`';
        else out[out_len++] = in[i];
      } else {
        out[out_len++] = in[i];
      }
    }
    strings[string_count++] = js_mkstr(js, NULL, out_len);
    
    if (n >= template_len - 1 || in[n] != '$') break;
    
    n += 2;
    int brace_count = 1;
    size_t expr_start = n;
    while (n < template_len - 1 && brace_count > 0) {
      if (in[n] == '{') brace_count++;
      else if (in[n] == '}') brace_count--;
      if (brace_count > 0) n++;
    }
    if (brace_count != 0) return js_mkerr_typed(js, JS_ERR_SYNTAX, "unclosed ${");
    
    jsval_t expr_result = js_eval_str(js, (const char *)&in[expr_start], (jsoff_t)(n - expr_start));
    if (is_err(expr_result)) return expr_result;
    expr_result = resolveprop(js, expr_result);
    values[value_count++] = expr_result;
    n++;
  }
  
  jsval_t strings_arr = mkarr(js);
  for (int i = 0; i < string_count; i++) {
    char idx[16];
    snprintf(idx, sizeof(idx), "%d", i);
    setprop(js, strings_arr, js_mkstr(js, idx, strlen(idx)), strings[i]);
  }
  setprop(js, strings_arr, js_mkstr(js, "length", 6), tov((double)string_count));
  strings_arr = mkval(T_ARR, vdata(strings_arr));
  
  jsval_t args[65];
  args[0] = strings_arr;
  for (int i = 0; i < value_count; i++) {
    args[i + 1] = values[i];
  }
  
  uint8_t saved_flags = js->flags;
  jsval_t result = call_js_with_args(js, tag_func, args, 1 + value_count);
  
  JS_RESTORE_STATE(js, saved);
  js->flags = saved_flags;
  js->consumed = 1;
  
  return result;
}

static jsval_t js_str_literal(struct js *js) {
  uint8_t *in = (uint8_t *) &js->code[js->toff];
  size_t n1 = 0, n2 = 0;
  size_t needed = sizeof(jsoff_t) + js->tlen;
  if (js->brk + needed > js->size) {
    if (!js_try_grow_memory(js, needed)) return js_mkerr(js, "oom");
  }
  uint8_t *out = &js->mem[js->brk + sizeof(jsoff_t)];
  while (n2++ + 2 < js->tlen) {
    if (in[n2] == '\\') {
      if (in[n2 + 1] == in[0]) {
        out[n1++] = in[0];
      } else if (in[n2 + 1] == 'n') {
        out[n1++] = '\n';
      } else if (in[n2 + 1] == 't') {
        out[n1++] = '\t';
      } else if (in[n2 + 1] == 'r') {
        out[n1++] = '\r';
      } else if (in[n2 + 1] == '0' && !(in[n2 + 2] >= '0' && in[n2 + 2] <= '7')) {
        out[n1++] = '\0';
      } else if (in[n2 + 1] >= '0' && in[n2 + 1] <= '7') {
        if (js->flags & F_STRICT) {
          return js_mkerr_typed(js, JS_ERR_SYNTAX, "octal escape sequences are not allowed in strict mode");
        }
        int val = in[n2 + 1] - '0';
        int extra = 0;
        if (in[n2 + 2] >= '0' && in[n2 + 2] <= '7') {
          val = val * 8 + (in[n2 + 2] - '0');
          extra++;
          if (in[n2 + 3] >= '0' && in[n2 + 3] <= '7' && val * 8 + (in[n2 + 3] - '0') <= 255) {
            val = val * 8 + (in[n2 + 3] - '0');
            extra++;
          }
        }
        n2 += extra;
        out[n1++] = (uint8_t)val;
      } else if (in[n2 + 1] == 'v') {
        out[n1++] = '\v';
      } else if (in[n2 + 1] == 'f') {
        out[n1++] = '\f';
      } else if (in[n2 + 1] == 'b') {
        out[n1++] = '\b';
      } else if (in[n2 + 1] == 'x' && is_xdigit(in[n2 + 2]) &&
                 is_xdigit(in[n2 + 3])) {
        out[n1++] = (uint8_t) ((unhex(in[n2 + 2]) << 4U) | unhex(in[n2 + 3]));
        n2 += 2;
      } else if (in[n2 + 1] == 'u' && in[n2 + 2] == '{') {
        uint32_t cp = 0;
        size_t i = n2 + 3;
        while (i < js->tlen && is_xdigit(in[i])) {
          cp = (cp << 4) | unhex(in[i]);
          i++;
        }
        if (in[i] == '}') {
          if (cp < 0x80) {
            out[n1++] = (uint8_t) cp;
          } else if (cp < 0x800) {
            out[n1++] = (uint8_t) (0xC0 | (cp >> 6));
            out[n1++] = (uint8_t) (0x80 | (cp & 0x3F));
          } else if (cp < 0x10000) {
            out[n1++] = (uint8_t) (0xE0 | (cp >> 12));
            out[n1++] = (uint8_t) (0x80 | ((cp >> 6) & 0x3F));
            out[n1++] = (uint8_t) (0x80 | (cp & 0x3F));
          } else {
            out[n1++] = (uint8_t) (0xF0 | (cp >> 18));
            out[n1++] = (uint8_t) (0x80 | ((cp >> 12) & 0x3F));
            out[n1++] = (uint8_t) (0x80 | ((cp >> 6) & 0x3F));
            out[n1++] = (uint8_t) (0x80 | (cp & 0x3F));
          }
          n2 = i;
        } else {
          out[n1++] = in[n2 + 1];
        }
      } else if (in[n2 + 1] == 'u' && is_xdigit(in[n2 + 2]) &&
                 is_xdigit(in[n2 + 3]) && is_xdigit(in[n2 + 4]) &&
                 is_xdigit(in[n2 + 5])) {
        uint32_t cp = (unhex(in[n2 + 2]) << 12U) | (unhex(in[n2 + 3]) << 8U) |
                      (unhex(in[n2 + 4]) << 4U) | unhex(in[n2 + 5]);
        if (cp < 0x80) {
          out[n1++] = (uint8_t) cp;
        } else if (cp < 0x800) {
          out[n1++] = (uint8_t) (0xC0 | (cp >> 6));
          out[n1++] = (uint8_t) (0x80 | (cp & 0x3F));
        } else {
          out[n1++] = (uint8_t) (0xE0 | (cp >> 12));
          out[n1++] = (uint8_t) (0x80 | ((cp >> 6) & 0x3F));
          out[n1++] = (uint8_t) (0x80 | (cp & 0x3F));
        }
        n2 += 4;
      } else if (in[n2 + 1] == '\\') {
        out[n1++] = '\\';
      } else {
        out[n1++] = in[n2 + 1];
      }
      n2++;
    } else {
      out[n1++] = ((uint8_t *) js->code)[js->toff + n2];
    }
  }
  return js_mkstr(js, NULL, n1);
}

static jsval_t js_bigint_literal(struct js *js) {
  const char *start = &js->code[js->toff];
  size_t len = js->tlen - 1;
  while (len > 1 && start[0] == '0') { start++; len--; }
  bool neg = false;
  if (len > 0 && start[0] == '-') { neg = true; start++; len--; }
  return js_mkbigint(js, start, len, neg);
}

static jsval_t js_arr_destruct_assign(struct js *js) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  js->consumed = 1;
  
  js_parse_state_t pattern_state;
  JS_SAVE_STATE(js, pattern_state);
  
  int depth = 1;
  while (depth > 0 && next(js) != TOK_EOF) {
    if (js->tok == TOK_LBRACKET) depth++;
    else if (js->tok == TOK_RBRACKET) depth--;
    if (depth > 0) js->consumed = 1;
  }
  js->consumed = 1;
  
  if (next(js) != TOK_ASSIGN) {
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "array destructuring requires assignment");
  }
  js->consumed = 1;
  
  jsval_t v = js_expr(js);
  if (is_err(v)) return v;
  
  jsval_t arr = js_mkundef();
  if (exe) {
    arr = resolveprop(js, v);
    if (vtype(arr) != T_ARR && vtype(arr) != T_STR) {
      return js_mkerr(js, "cannot array destructure non-iterable");
    }
  }
  
  js_parse_state_t end_state;
  JS_SAVE_STATE(js, end_state);
  JS_RESTORE_STATE(js, pattern_state);
  
  int index = 0;
  while (next(js) != TOK_RBRACKET && next(js) != TOK_EOF) {
    if (next(js) == TOK_COMMA) {
      js->consumed = 1;
      index++;
      continue;
    }
    
    bool is_rest = false;
    if (next(js) == TOK_REST) {
      is_rest = true;
      js->consumed = 1;
    }
    
    if (next(js) != TOK_IDENTIFIER) {
      return js_mkerr_typed(js, JS_ERR_SYNTAX, "identifier expected in array destructuring");
    }
    jsoff_t var_off = js->toff, var_len = js->tlen;
    js->consumed = 1;
    
    jsoff_t default_off = 0, default_len = 0;
    if (!is_rest && next(js) == TOK_ASSIGN) {
      js->consumed = 1;
      default_off = js->pos;
      uint8_t sf = js->flags;
      js->flags |= F_NOEXEC;
      jsval_t r = js_expr(js);
      js->flags = sf;
      if (is_err(r)) return r;
      default_len = js->pos - default_off;
    }
    
    if (exe) {
      const char *var_name = &js->code[var_off];
      
      jsval_t prop_val;
      if (is_rest) {
        jsval_t rest_arr = js_mkarr(js);
        if (is_err(rest_arr)) return rest_arr;
        jsoff_t total_len = vtype(arr) == T_STR ? 0 : js_arr_len(js, arr);
        if (vtype(arr) == T_STR) {
          jsoff_t slen;
          vstr(js, arr, &slen);
          total_len = slen;
        }
        for (jsoff_t i = index; i < total_len; i++) {
          jsval_t elem;
          if (vtype(arr) == T_STR) {
            jsoff_t slen, soff = vstr(js, arr, &slen);
            elem = js_mkstr(js, (char *)&js->mem[soff + i], 1);
          } else {
            elem = js_arr_get(js, arr, i);
          }
          js_arr_push(js, rest_arr, elem);
        }
        prop_val = rest_arr;
      } else {
        if (vtype(arr) == T_STR) {
          jsoff_t slen, soff = vstr(js, arr, &slen);
          if ((jsoff_t)index < slen) {
            prop_val = js_mkstr(js, (char *)&js->mem[soff + index], 1);
          } else {
            prop_val = js_mkundef();
          }
        } else {
          prop_val = js_arr_get(js, arr, index);
        }
        
        if (vtype(prop_val) == T_UNDEF && default_len > 0) {
          prop_val = js_eval_slice(js, default_off, default_len);
          if (is_err(prop_val)) return prop_val;
          prop_val = resolveprop(js, prop_val);
        }
      }
      
      jsoff_t existing = lkp_scope(js, js->scope, var_name, var_len);
      if (existing != 0) {
        jsval_t res = setprop(js, js->scope, js_mkstr(js, var_name, var_len), prop_val);
        if (is_err(res)) return res;
      } else {
        jsval_t global_scope = js->scope;
        while (vdata(upper(js, global_scope)) != 0) {
          global_scope = upper(js, global_scope);
        }
        jsval_t res = setprop(js, global_scope, js_mkstr(js, var_name, var_len), prop_val);
        if (is_err(res)) return res;
      }
    }
    
    index++;
    if (next(js) == TOK_RBRACKET) break;
    EXPECT(TOK_COMMA);
  }
  
  JS_RESTORE_STATE(js, end_state);
  return v;
}

static jsval_t js_arr_literal(struct js *js);

static jsval_t js_arr_or_destruct(struct js *js) {
  jsoff_t saved_pos = js->pos;
  uint8_t saved_tok = js->tok;
  uint8_t saved_consumed = js->consumed;
  uint8_t saved_flags = js->flags;
  
  js->flags |= F_NOEXEC;
  js->consumed = 1;
  int depth = 1;
  while (depth > 0 && next(js) != TOK_EOF) {
    if (js->tok == TOK_LBRACKET) depth++;
    else if (js->tok == TOK_RBRACKET) depth--;
    if (depth > 0) js->consumed = 1;
  }
  js->consumed = 1;
  bool is_destruct = (next(js) == TOK_ASSIGN);
  
  js->pos = saved_pos;
  js->tok = saved_tok;
  js->consumed = saved_consumed;
  js->flags = saved_flags;
  
  if (is_destruct) {
    return js_arr_destruct_assign(js);
  }
  return js_arr_literal(js);
}

static jsval_t js_arr_literal(struct js *js) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  jsval_t arr = exe ? mkarr(js) : js_mkundef();
  if (is_err(arr)) return arr;

  js->consumed = 1;
  jsoff_t idx = 0;
  while (next(js) != TOK_RBRACKET) {
    if (next(js) == TOK_COMMA) {
      idx++;
      js->consumed = 1;
      continue;
    }
    
    bool is_spread = (next(js) == TOK_REST);
    if (is_spread) js->consumed = 1;

    jsval_t val = js_expr(js);
    if (!exe) goto next_elem;
    if (is_err(val)) return val;

    jsval_t resolved = resolveprop(js, val);
    if (!is_spread) {
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
      jsval_t key = js_mkstr(js, idxstr, idxlen);
      jsval_t res = setprop(js, arr, key, resolved);
      if (is_err(res)) return res;
      idx++;
      goto next_elem;
    }

    uint8_t t = vtype(resolved);
    if (t != T_ARR && t != T_STR) goto next_elem;

    if (t == T_STR) {
      jsoff_t slen, soff = vstr(js, resolved, &slen);
      for (jsoff_t i = 0; i < slen; i++) {
        char idxstr[16];
        size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
        jsval_t key = js_mkstr(js, idxstr, idxlen);
        jsval_t ch = js_mkstr(js, (char *)&js->mem[soff + i], 1);
        setprop(js, arr, key, ch);
        idx++;
      }
      goto next_elem;
    }

    jsoff_t len = js_arr_len(js, resolved);
    for (jsoff_t i = 0; i < len; i++) {
      char src_idx[16], dst_idx[16];
      snprintf(src_idx, sizeof(src_idx), "%u", (unsigned)i);
      snprintf(dst_idx, sizeof(dst_idx), "%u", (unsigned)idx);
      jsval_t key = js_mkstr(js, src_idx, strlen(src_idx));
      jsoff_t prop_off = lkp(js, resolved, (char *)&js->mem[(jsoff_t)vdata(key) + sizeof(jsoff_t)], strlen(src_idx));
      jsval_t elem = (prop_off != 0) ? resolveprop(js, mkval(T_PROP, prop_off)) : js_mkundef();
      setprop(js, arr, js_mkstr(js, dst_idx, strlen(dst_idx)), elem);
      idx++;
    }

  next_elem:
    if (next(js) == TOK_RBRACKET) break;
    EXPECT(TOK_COMMA);
  }

  EXPECT(TOK_RBRACKET);
  if (exe) {
    jsval_t len_key = js_mkstr(js, "length", 6);
    jsval_t len_val = tov((double)idx);
    jsval_t res = setprop(js, arr, len_key, len_val);
    if (is_err(res)) return res;
    arr = mkval(T_ARR, vdata(arr));
  }
  return arr;
}

static jsval_t js_regex_literal(struct js *js) {
  jsoff_t start = js->pos;
  jsoff_t pattern_start = start;
  bool in_class = false;
  
  while (js->pos < js->clen) {
    char c = js->code[js->pos];
    if (c == '\\' && js->pos + 1 < js->clen) {
      js->pos += 2;
      continue;
    }
    if (c == '[') in_class = true;
    else if (c == ']') in_class = false;
    else if (c == '/' && !in_class) break;
    js->pos++;
  }
  
  if (js->pos >= js->clen || js->code[js->pos] != '/') {
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "unterminated regex");
  }
  
  jsoff_t pattern_end = js->pos;
  js->pos++;
  
  jsoff_t flags_start = js->pos;
  while (js->pos < js->clen) {
    char c = js->code[js->pos];
    if (c == 'g' || c == 'i' || c == 'm' || c == 's' || c == 'u' || c == 'y') {
      js->pos++;
    } else {
      break;
    }
  }
  jsoff_t flags_end = js->pos;
  
  if (js->flags & F_NOEXEC) return js_mkundef();
  
  jsval_t pattern = js_mkstr(js, &js->code[pattern_start], pattern_end - pattern_start);
  jsval_t flags = js_mkstr(js, &js->code[flags_start], flags_end - flags_start);
  
  jsval_t regexp_obj = mkobj(js, 0);
  jsval_t regexp_proto = get_ctor_proto(js, "RegExp", 6);
  if (vtype(regexp_proto) == T_OBJ) set_proto(js, regexp_obj, regexp_proto);

  setprop(js, regexp_obj, js_mkstr(js, "source", 6), pattern);
  setprop(js, regexp_obj, js_mkstr(js, "flags", 5), flags);

  jsoff_t flen = flags_end - flags_start;
  const char *fstr = &js->code[flags_start];
  bool global = false, ignoreCase = false, multiline = false, dotAll = false, sticky = false;
  for (jsoff_t i = 0; i < flen; i++) {
    if (fstr[i] == 'g') global = true;
    if (fstr[i] == 'i') ignoreCase = true;
    if (fstr[i] == 'm') multiline = true;
    if (fstr[i] == 's') dotAll = true;
    if (fstr[i] == 'y') sticky = true;
  }

  setprop(js, regexp_obj, js_mkstr(js, "global", 6), mkval(T_BOOL, global ? 1 : 0));
  setprop(js, regexp_obj, js_mkstr(js, "ignoreCase", 10), mkval(T_BOOL, ignoreCase ? 1 : 0));
  setprop(js, regexp_obj, js_mkstr(js, "multiline", 9), mkval(T_BOOL, multiline ? 1 : 0));
  setprop(js, regexp_obj, js_mkstr(js, "dotAll", 6), mkval(T_BOOL, dotAll ? 1 : 0));
  setprop(js, regexp_obj, js_mkstr(js, "sticky", 6), mkval(T_BOOL, sticky ? 1 : 0));
  setprop(js, regexp_obj, js_mkstr(js, "lastIndex", 9), tov(0));

  return regexp_obj;
}

static jsval_t set_obj_property(struct js *js, jsval_t obj, jsval_t key, jsval_t val, bool is_computed, bool *proto_set) {
  bool is_proto = false;
  if (!is_computed && vtype(key) == T_STR) {
    jsoff_t klen;
    const char *kstr = (char *)&js->mem[vstr(js, key, &klen)];
    is_proto = (klen == STR_PROTO_LEN && memcmp(kstr, STR_PROTO, STR_PROTO_LEN) == 0);
  }

  if (is_proto) {
    if (*proto_set) return js_mkerr_typed(js, JS_ERR_SYNTAX, "Duplicate __proto__ fields are not allowed in object literals");
    *proto_set = true;
    uint8_t pt = vtype(val);
    if (pt == T_OBJ || pt == T_ARR || pt == T_FUNC || pt == T_NULL) {
      set_proto(js, obj, pt == T_NULL ? js_mknull() : val);
    }
    return js_mkundef();
  }

  if (vtype(val) == T_FUNC) {
    jsval_t func_obj = mkval(T_OBJ, vdata(val));
    if (lkp(js, func_obj, "name", 4) == 0) {
      jsval_t name_key = js_mkstr(js, "name", 4);
      if (!is_err(name_key)) setprop(js, func_obj, name_key, key);
    }
  }
  
  return setprop(js, obj, key, val);
}

static jsval_t js_obj_literal(struct js *js) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  jsval_t obj = exe ? mkobj(js, 0) : js_mkundef();
  if (is_err(obj)) return obj;
  if (exe) {
    jsval_t object_proto = get_ctor_proto(js, "Object", 6);
    if (vtype(object_proto) == T_OBJ) set_proto(js, obj, object_proto);
  }
  js->consumed = 1;
  bool proto_set_in_literal = false;
  
  while (next(js) != TOK_RBRACE) {
    jsval_t key = 0;
    jsoff_t id_off = 0, id_len = 0;
    bool is_computed = false;
    
    if (js->tok == TOK_REST) {
      js->consumed = 1;
      jsval_t spread_expr = js_expr(js);
      if (is_err(spread_expr)) return spread_expr;
      if (!exe) goto spread_next;
      
      jsval_t spread_obj = resolveprop(js, spread_expr);
      uint8_t st = vtype(spread_obj);
      if (st != T_OBJ && st != T_ARR && st != T_FUNC) goto spread_next;
      
      jsval_t src_obj = (st == T_OBJ) ? spread_obj : mkval(T_OBJ, vdata(spread_obj));
      jsoff_t next_prop_off = loadoff(js, (jsoff_t) vdata(src_obj)) & ~(3U | FLAGMASK);
      
      while (next_prop_off < js->brk && next_prop_off != 0) {
        jsoff_t header = loadoff(js, next_prop_off);
        if (is_slot_prop(header)) { next_prop_off = next_prop(header); continue; }
        
        jsoff_t koff = loadoff(js, next_prop_off + (jsoff_t) sizeof(next_prop_off));
        jsoff_t klen = offtolen(loadoff(js, koff));
        const char *prop_key = (char *) &js->mem[koff + sizeof(koff)];
        jsval_t prop_val = loadval(js, next_prop_off + (jsoff_t) (sizeof(next_prop_off) + sizeof(koff)));
        
        next_prop_off = next_prop(header);
        if (is_internal_prop(prop_key, klen)) continue;
        
        jsval_t key_str = js_mkstr(js, prop_key, klen);
        setprop(js, obj, key_str, prop_val);
      }
      
    spread_next:
      if (next(js) == TOK_RBRACE) break;
      EXPECT(TOK_COMMA);
      continue;
    }
    
    bool is_getter = false, is_setter = false;
    if (js->tok == TOK_IDENTIFIER) {
      bool is_get = (js->tlen == 3 && memcmp(js->code + js->toff, "get", 3) == 0);
      bool is_set = (js->tlen == 3 && memcmp(js->code + js->toff, "set", 3) == 0);
      
      if (is_get || is_set) {
        jsoff_t saved_pos = js->pos;
        uint8_t saved_tok = js->tok;
        uint8_t saved_consumed = js->consumed;
        jsoff_t saved_toff = js->toff;
        jsoff_t saved_tlen = js->tlen;
        
        js->consumed = 1;
        uint8_t peek = next(js);
        
        if (peek == TOK_IDENTIFIER || peek == TOK_STRING || peek == TOK_NUMBER || peek == TOK_LBRACKET) {
          is_getter = is_get;
          is_setter = is_set;
          
          if (peek == TOK_IDENTIFIER) {
            id_off = js->toff;
            id_len = js->tlen;
            if (exe) key = js_mkstr_ident(js, js->code + js->toff, js->tlen);
          } else if (peek == TOK_STRING) {
            id_off = js->toff;
            id_len = js->tlen;
            if (exe) key = js_str_literal(js);
          } else if (peek == TOK_NUMBER) {
            id_off = js->toff;
            id_len = js->tlen;
            if (exe) {
              double num = strtod(js->code + js->toff, NULL);
              char buf[64];
              size_t n = strnum(tov(num), buf, sizeof(buf));
              key = js_mkstr(js, buf, n);
            }
          } else if (peek == TOK_LBRACKET) {
            is_computed = true;
            js->consumed = 1;
            jsval_t key_expr = js_expr(js);
            if (is_err(key_expr)) return key_expr;
            
            if (exe) {
              jsval_t resolved_key = resolveprop(js, key_expr);
              if (vtype(resolved_key) == T_STR) {
                key = resolved_key;
              } else {
                char buf[64];
                size_t n = tostr(js, resolved_key, buf, sizeof(buf));
                key = js_mkstr(js, buf, n);
              }
              if (is_err(key)) return key;
            }
            
            if (next(js) != TOK_RBRACKET) {
              return js_mkerr_typed(js, JS_ERR_SYNTAX, "] expected after computed property name");
            }
          }
        } else {
          js->pos = saved_pos;
          js->tok = saved_tok;
          js->consumed = saved_consumed;
          js->toff = saved_toff;
          js->tlen = saved_tlen;
          
          id_off = saved_toff;
          id_len = saved_tlen;
          if (exe) key = js_mkstr_ident(js, js->code + saved_toff, saved_tlen);
        }
      } else {
        id_off = js->toff;
        id_len = js->tlen;
        if (exe) key = js_mkstr_ident(js, js->code + js->toff, js->tlen);
      }
    } else if (js->tok == TOK_STRING) {
      if (exe) key = js_str_literal(js);
    } else if (js->tok == TOK_NUMBER) {
      if (exe) {
        double num = strtod(js->code + js->toff, NULL);
        char buf[64];
        size_t n = strnum(tov(num), buf, sizeof(buf));
        key = js_mkstr(js, buf, n);
      }
    } else if (js->tok == TOK_LBRACKET) {
      is_computed = true;
      js->consumed = 1;
      jsval_t key_expr = js_expr(js);
      if (is_err(key_expr)) return key_expr;
      
      if (exe) {
        jsval_t resolved_key = resolveprop(js, key_expr);
        if (vtype(resolved_key) == T_STR) {
          key = resolved_key;
        } else if (vtype(resolved_key) == T_SYMBOL) {
          char buf[64];
          snprintf(buf, sizeof(buf), "__sym_%llu__", (unsigned long long)sym_get_id(resolved_key));
          key = js_mkstr(js, buf, strlen(buf));
        } else {
          char buf[64];
          size_t n = tostr(js, resolved_key, buf, sizeof(buf));
          key = js_mkstr(js, buf, n);
        }
        if (is_err(key)) return key;
      }
      
      if (next(js) != TOK_RBRACKET) {
        return js_mkerr_typed(js, JS_ERR_SYNTAX, "] expected after computed property name");
      }
    } else if (is_keyword_propname(js->tok)) {
      id_off = js->toff;
      id_len = js->tlen;
      if (exe) key = js_mkstr(js, js->code + js->toff, js->tlen);
    } else {
      return js_mkerr_typed(js, JS_ERR_SYNTAX, "parse error");
    }
    js->consumed = 1;
    
    if (!is_computed && id_len > 0 && (next(js) == TOK_COMMA || next(js) == TOK_RBRACE)) {
      jsval_t val = lookup(js, js->code + id_off, id_len);
      if (exe) {
        if (is_err(val)) return val;
        if (is_err(key)) return key;
        jsval_t res = setprop(js, obj, key, resolveprop(js, val));
        if (is_err(res)) return res;
      }
    } else if (
        (is_getter || is_setter) ||
        (!is_computed && id_len > 0 && next(js) == TOK_LPAREN) ||
        (is_computed && next(js) == TOK_LPAREN)
      ) {
      uint8_t flags = js->flags;
      jsoff_t pos = js->pos - 1;
      js->consumed = 1;
      if (!parse_func_params(js, &flags, NULL)) {
        js->flags = flags;
        return js_mkerr_typed(js, JS_ERR_SYNTAX, "invalid parameters");
      }
      EXPECT(TOK_RPAREN, js->flags = flags);
      EXPECT(TOK_LBRACE, js->flags = flags);
      js->consumed = 0;
      js->flags |= F_NOEXEC;
      jsval_t block_res = js_block(js, false);
      if (is_err(block_res)) {
        js->flags = flags;
        return block_res;
      }
      js->flags = flags;
      js->consumed = 1;
      
      if (exe) {
        jsval_t func_obj = mkobj(js, 0);
        if (is_err(func_obj)) return func_obj;
        set_func_code(js, func_obj, &js->code[pos], js->pos - pos);
        jsval_t name_key = js_mkstr(js, "name", 4);
        setprop(js, func_obj, name_key, key);
        
        jsval_t closure_scope = for_let_capture_scope(js);
        if (is_err(closure_scope)) return closure_scope;
        set_slot(js, func_obj, SLOT_SCOPE, closure_scope);
        
        jsval_t func_proto = get_slot(js, js_glob(js), SLOT_FUNC_PROTO);
        if (vtype(func_proto) == T_FUNC) set_proto(js, func_obj, func_proto);
        jsval_t val = mkval(T_FUNC, (unsigned long) vdata(func_obj));
        
        if (is_getter || is_setter) {
          jsoff_t key_len;
          const char *key_str = NULL;
          if (vtype(key) == T_STR) {
            jsoff_t key_off = vstr(js, key, &key_len);
            key_str = (char *)&js->mem[key_off];
          }
          
          if (key_str) {
            if (is_getter) {
              js_set_getter_desc(js, obj, key_str, key_len, val, JS_DESC_E | JS_DESC_C);
            } else js_set_setter_desc(js, obj, key_str, key_len, val, JS_DESC_E | JS_DESC_C);
          }
        } else {
          jsval_t res = setprop(js, obj, key, val);
          if (is_err(res)) return res;
        }
      }
    } else {
      EXPECT(TOK_COLON);
      jsval_t val = js_expr(js);
      if (exe) {
        if (is_err(val)) return val;
        if (is_err(key)) return key;
        jsval_t res = set_obj_property(
          js, obj, key, resolveprop(js, val), 
          is_computed, &proto_set_in_literal
        );
        if (is_err(res)) return res;
      }
    }
    
    if (next(js) == TOK_RBRACE) break;
    EXPECT(TOK_COMMA);
  }
  
  EXPECT(TOK_RBRACE);
  return obj;
}

static void skip_default_value(struct js *js) {
  int depth = 0;
  while (next(js) != TOK_EOF) {
    uint8_t tok = next(js);
    if (depth == 0 && (tok == TOK_RPAREN || tok == TOK_COMMA)) break;
    js->consumed = 1;
    if (tok == TOK_LPAREN || tok == TOK_LBRACKET || tok == TOK_LBRACE) depth++;
    else if (tok == TOK_RPAREN || tok == TOK_RBRACKET || tok == TOK_RBRACE) depth--;
  }
}

static void skip_destructuring_pattern(struct js *js) {
  uint8_t open_tok = js->tok;
  uint8_t close_tok = (open_tok == TOK_LBRACE) ? TOK_RBRACE : TOK_RBRACKET;
  int depth = 1;
  js->consumed = 1;
  while (depth > 0 && next(js) != TOK_EOF) {
    if (js->tok == open_tok) depth++;
    else if (js->tok == close_tok) depth--;
    if (depth > 0) js->consumed = 1;
  }
  js->consumed = 1;
}

typedef struct { const char *name; size_t len; } param_entry_t;
static const UT_icd param_entry_icd = { sizeof(param_entry_t), NULL, NULL, NULL };

static bool parse_func_params(struct js *js, uint8_t *flags, int *out_count) {
  UT_array *params;
  utarray_new(params, &param_entry_icd);
  
  #define FAIL(msg, ...) do { \
    if (flags) js->flags = *flags; \
    js_mkerr_typed(js, JS_ERR_SYNTAX, msg, ##__VA_ARGS__); \
    utarray_free(params); \
    return false; \
  } while(0)
  
  while (next(js) != TOK_EOF && next(js) != TOK_RPAREN) {
    bool is_rest = (next(js) == TOK_REST);
    if (is_rest) { js->consumed = 1; next(js); }
    
    if (next(js) == TOK_LBRACE || next(js) == TOK_LBRACKET) {
      skip_destructuring_pattern(js);
      param_entry_t entry = {NULL, 0};
      utarray_push_back(params, &entry);
      if (next(js) == TOK_ASSIGN) { js->consumed = 1; skip_default_value(js); }
    } else if (is_valid_param_name(next(js))) {
      const char *name = &js->code[js->toff];
      size_t len = js->tlen;
      
      if ((js->flags & F_STRICT) && is_strict_restricted(name, len))
      FAIL("cannot use '%.*s' as parameter name in strict mode", (int)len, name);
      
      if (js->flags & F_STRICT) {
        param_entry_t *p = NULL;
        while ((p = (param_entry_t *)utarray_next(params, p))) {
          if (p->len == len && p->name && memcmp(p->name, name, len) == 0)
          FAIL("duplicate parameter name '%.*s' in strict mode", (int)len, name);
        }
      }
      
      param_entry_t entry = {name, len};
      utarray_push_back(params, &entry);
      js->consumed = 1;
      if (next(js) == TOK_ASSIGN) { js->consumed = 1; skip_default_value(js); }
    } else {
      FAIL("identifier expected");
    }
    
    if (is_rest && next(js) != TOK_RPAREN) FAIL("rest parameter must be last");
    if (next(js) == TOK_RPAREN) break;
    if (next(js) != TOK_COMMA) FAIL("parse error");
    js->consumed = 1;
  }
  
  #undef FAIL
  
  if (out_count) *out_count = (int)utarray_len(params);
  utarray_free(params);
  return true;
}

static jsval_t js_func_literal(struct js *js, bool is_async) {
  uint8_t flags = js->flags;
  js->consumed = 1;
  jsoff_t name_off = 0, name_len = 0;
  if (next(js) == TOK_IDENTIFIER) {
    name_off = js->toff;
    name_len = js->tlen;
    js->consumed = 1;
  }
  
  EXPECT(TOK_LPAREN, js->flags = flags);
  jsoff_t pos = js->pos - 1;
  int param_count = 0;
  if (!parse_func_params(js, &flags, &param_count)) {
    js->flags = flags;
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "invalid parameters");
  }
  
  EXPECT(TOK_RPAREN, js->flags = flags);
  EXPECT(TOK_LBRACE, js->flags = flags);
  js->consumed = 0;
  js->flags |= F_NOEXEC;
  
  jsval_t res = js_block(js, false);
  if (is_err(res)) {
    js->flags = flags;
    return res;
  }
  js->flags = flags;
  js->consumed = 1;
  jsval_t func_obj = mkobj(js, 0);
  if (is_err(func_obj)) return func_obj;
  set_func_code(js, func_obj, &js->code[pos], js->pos - pos);
  
  jsval_t len_key = js_mkstr(js, "length", 6);
  if (is_err(len_key)) return len_key;
  jsval_t res_len = setprop(js, func_obj, len_key, tov(param_count));
  if (is_err(res_len)) return res_len;
  js_set_descriptor(js, func_obj, "length", 6, JS_DESC_C);
  
  if (is_async) {
    set_slot(js, func_obj, SLOT_ASYNC, js_true);
    jsval_t async_proto = get_slot(js, js_glob(js), SLOT_ASYNC_PROTO);
    if (vtype(async_proto) == T_FUNC) set_proto(js, func_obj, async_proto);
  } else {
    jsval_t func_proto = get_slot(js, js_glob(js), SLOT_FUNC_PROTO);
    if (vtype(func_proto) == T_FUNC) set_proto(js, func_obj, func_proto);
  }
  
  if (name_len > 0) {
    jsval_t name_val = js_mkstr(js, &js->code[name_off], name_len);
    if (is_err(name_val)) return name_val;
    set_slot(js, func_obj, SLOT_NAME, name_val);
    jsval_t name_key = js_mkstr(js, "name", 4);
    if (is_err(name_key)) return name_key;
    jsval_t res3 = setprop(js, func_obj, name_key, name_val);
    if (is_err(res3)) return res3;
  }
  
  if (!(flags & F_NOEXEC)) {
    jsval_t closure_scope = for_let_capture_scope(js);
    if (is_err(closure_scope)) return closure_scope;
    set_slot(js, func_obj, SLOT_SCOPE, closure_scope);
    if (flags & F_STRICT) set_slot(js, func_obj, SLOT_STRICT, js_true);
  }
  
  jsval_t func = mkval(T_FUNC, (unsigned long) vdata(func_obj));
  
  jsval_t proto_setup = setup_func_prototype(js, func);
  if (is_err(proto_setup)) return proto_setup;
  
  return func;
}

#define RTL_BINOP(_f1, _f2, _cond)  \
  jsval_t res = _f1(js);            \
  while (!is_err(res) && (_cond)) { \
    uint8_t op = js->tok;           \
    js->consumed = 1;               \
    jsval_t rhs = _f2(js);          \
    if (is_err(rhs)) return rhs;    \
    res = do_op(js, op, res, rhs);  \
  }                                 \
  return res;

#define LTR_BINOP(_f, _cond)        \
  jsval_t res = _f(js);             \
  while (!is_err(res) && (_cond)) { \
    uint8_t op = js->tok;           \
    js->consumed = 1;               \
    jsval_t rhs = _f(js);           \
    if (is_err(rhs)) return rhs;    \
    res = do_op(js, op, res, rhs);  \
  }                                 \
  return res;

static jsval_t js_class_expr(struct js *js, bool is_expression);

static jsval_t js_literal(struct js *js) {
  next(js);
  js->consumed = 1;
  
  switch (js->tok) {
    case TOK_ERR:
      if ((js->flags & F_STRICT) && js->toff < js->clen && js->code[js->toff] == '0' && 
          js->toff + 1 < js->clen && is_digit(js->code[js->toff + 1])) {
        return js_mkerr_typed(js, JS_ERR_SYNTAX, "octal literals are not allowed in strict mode");
      }
      return js_mkerr_typed(js, JS_ERR_SYNTAX, "parse error");
    case TOK_NUMBER:      return js->tval;
    case TOK_BIGINT:      return js_bigint_literal(js);
    case TOK_STRING:      return js_str_literal(js);
    case TOK_TEMPLATE:    return js_template_literal(js);
    case TOK_LBRACE:      return js_obj_literal(js);
    case TOK_LBRACKET:    return js_arr_or_destruct(js);
    case TOK_DIV:         return js_regex_literal(js);
    case TOK_CLASS:       return js_class_expr(js, true);
    case TOK_FUNC: {
      uint8_t la = lookahead(js);
      if (la != TOK_LPAREN && la != TOK_IDENTIFIER) {
        return mkcoderef((jsoff_t) js->toff, (jsoff_t) js->tlen);
      }
      return js_func_literal(js, false);
    }
    case TOK_ASYNC: {
      jsoff_t async_off = js->toff, async_len = js->tlen;
      js->consumed = 1;
      uint8_t next_tok = next(js);
      if (next_tok == TOK_FUNC) {
        return js_func_literal(js, true);
      } else if (next_tok == TOK_LPAREN) {
        return js_async_arrow_paren(js);
      } else if (next_tok == TOK_IDENTIFIER) {
        jsoff_t id_start = js->toff;
        jsoff_t id_len = js->tlen;
        js->consumed = 1;
        if (next(js) == TOK_ARROW) {
          js->consumed = 1;
          char param_buf[256];
          if (id_len + 3 > sizeof(param_buf)) return js_mkerr(js, "param too long");
          param_buf[0] = '(';
          memcpy(param_buf + 1, &js->code[id_start], id_len);
          param_buf[id_len + 1] = ')';
          param_buf[id_len + 2] = '\0';
          uint8_t flags = js->flags;
          bool is_expr = next(js) != TOK_LBRACE;
          jsoff_t body_start = is_expr ? js->toff : js->pos;
          jsoff_t body_end = 0;
          jsval_t body_result;
          if (is_expr) {
            js->flags |= F_NOEXEC;
            body_result = js_assignment(js);
            if (is_err(body_result)) { js->flags = flags; return body_result; }
            uint8_t tok = next(js);
            body_end = is_body_end_tok(tok) ? js->toff : js->pos;
          } else {
            body_start = js->toff;
            js->flags |= F_NOEXEC;
            js->consumed = 1;
            body_result = js_block(js, false);
            if (is_err(body_result)) { js->flags = flags; return body_result; }
            if (js->tok == TOK_RBRACE && js->consumed) {
              body_end = js->pos;
            } else if (next(js) == TOK_RBRACE) {
              body_end = js->pos;
              js->consumed = 1;
            } else body_end = js->pos;
          }
          js->flags = flags;
          size_t fn_size = id_len + (body_end - body_start) + 64;
          char *fn_str = (char *) malloc(fn_size);
          if (!fn_str) return js_mkerr(js, "oom");
          jsoff_t fn_pos = 0;
          memcpy(fn_str + fn_pos, param_buf, id_len + 2);
          fn_pos += id_len + 2;
          if (is_expr) {
            fn_str[fn_pos++] = '{';
            memcpy(fn_str + fn_pos, "return ", 7);
            fn_pos += 7;
            size_t body_len = body_end - body_start;
            memcpy(fn_str + fn_pos, &js->code[body_start], body_len);
            fn_pos += body_len;
            fn_str[fn_pos++] = '}';
          } else {
            size_t body_len = body_end - body_start;
            memcpy(fn_str + fn_pos, &js->code[body_start], body_len);
            fn_pos += body_len;
          }
          jsval_t func_obj = mkobj(js, 0);
          if (is_err(func_obj)) { free(fn_str); return func_obj; }
          set_func_code(js, func_obj, fn_str, fn_pos);
          free(fn_str);
          set_slot(js, func_obj, SLOT_ASYNC, js_true);
          set_slot(js, func_obj, SLOT_ARROW, js_true);
          jsval_t async_proto = get_slot(js, js_glob(js), SLOT_ASYNC_PROTO);
          if (vtype(async_proto) == T_FUNC) set_proto(js, func_obj, async_proto);
          if (!(flags & F_NOEXEC)) {
            jsval_t closure_scope = for_let_capture_scope(js);
            if (is_err(closure_scope)) return closure_scope;
            set_slot(js, func_obj, SLOT_SCOPE, closure_scope);
            set_slot(js, func_obj, SLOT_THIS, js->this_val);
          }
          return mkval(T_FUNC, (unsigned long) vdata(func_obj));
        }
        return mkcoderef((jsoff_t) id_start, (jsoff_t) id_len);
      }
      return mkcoderef(async_off, async_len);
    }
    
    case TOK_SUPER: {
      jsval_t super_ctor = js->super_val;
      uint8_t la = lookahead(js);
      if ((la == TOK_DOT || la == TOK_LBRACKET) && vtype(super_ctor) == T_FUNC) {
        jsval_t ctor_obj = mkval(T_OBJ, vdata(super_ctor));
        jsoff_t proto_off = lkp_interned(js, ctor_obj, INTERN_PROTOTYPE, 9);
        if (proto_off == 0) return js_mkundef();
        jsval_t proto = resolveprop(js, mkval(T_PROP, proto_off));
        
        next(js); js->consumed = 1;
        const char *prop; jsoff_t prop_len;
        if (la == TOK_DOT) {
          if (next(js) != TOK_IDENTIFIER && !is_keyword_propname(js->tok))
            return js_mkerr_typed(js, JS_ERR_SYNTAX, "identifier expected");
          prop = &js->code[js->toff]; prop_len = js->tlen;
          js->consumed = 1;
        } else {
          jsval_t idx = js_expr(js);
          if (is_err(idx)) return idx;
          if (next(js) != TOK_RBRACKET) return js_mkerr_typed(js, JS_ERR_SYNTAX, "] expected");
          js->consumed = 1;
          idx = resolveprop(js, idx);
          if (vtype(idx) == T_STR) {
            prop_len = 0; prop = (const char *)&js->mem[vstr(js, idx, &prop_len)];
          } else {
            char buf[32]; prop_len = (jsoff_t)tostr(js, idx, buf, sizeof(buf));
            prop = buf;
          }
        }
        
        jsoff_t off = lkp(js, proto, prop, prop_len);
        if (off == 0) off = lkp_proto(js, proto, prop, prop_len);
        if (off == 0) return js_mkundef();
        
        jsval_t method = resolveprop(js, mkval(T_PROP, off));
        if (vtype(method) != T_FUNC) return method;
        
        jsval_t bound = mkobj(js, 0);
        jsval_t method_obj = mkval(T_OBJ, vdata(method));
        
        set_slot(js, bound, SLOT_CODE, get_slot(js, method_obj, SLOT_CODE));
        set_slot(js, bound, SLOT_CODE_LEN, get_slot(js, method_obj, SLOT_CODE_LEN));
        set_slot(js, bound, SLOT_SCOPE, get_slot(js, method_obj, SLOT_SCOPE));
        set_slot(js, bound, SLOT_BOUND_THIS, js->this_val);
        
        jsval_t func_proto = get_slot(js, js_glob(js), SLOT_FUNC_PROTO);
        if (vtype(func_proto) == T_FUNC) set_proto(js, bound, func_proto);
        
        return mkval(T_FUNC, vdata(bound));
      }
      return super_ctor;
    }
    
    case TOK_TRUE:        return js_true;
    case TOK_FALSE:       return js_false;
    
    case TOK_NULL:        return js_mknull();
    case TOK_UNDEF:       return js_mkundef();
    case TOK_THIS:        return js->this_val;
    
    default:
      if (is_identifier_like(js->tok)) return mkcoderef((jsoff_t) js->toff, (jsoff_t) js->tlen);
      size_t tok_len = js->tlen > 20 ? 20 : js->tlen;
      if (tok_len == 0) return js_mkerr_typed(js, JS_ERR_SYNTAX, "Unexpected token 'EOF'");
      return js_mkerr_typed(js, JS_ERR_SYNTAX, "Unexpected token '%.*s'", (int)tok_len, &js->code[js->toff]);
  }
}

static jsval_t js_arrow_func(struct js *js, jsoff_t params_start, jsoff_t params_end, bool is_async) {
  uint8_t flags = js->flags;
  bool is_expr = next(js) != TOK_LBRACE;
  jsoff_t body_start, body_end_actual;
  jsval_t body_result;
  
  if (is_expr) {
    body_start = js->toff;
    js->flags |= F_NOEXEC;
    body_result = js_assignment(js);
    if (is_err(body_result)) {
      js->flags = flags;
      return body_result;
    }
    uint8_t tok = next(js);
    body_end_actual = is_body_end_tok(tok) ? js->toff : js->pos;
  } else {
    body_start = js->toff;
    js->flags |= F_NOEXEC;
    js->consumed = 1;
    body_result = js_block(js, false);
    if (is_err(body_result)) {
      js->flags = flags;
      return body_result;
    }
    if (js->tok == TOK_RBRACE && js->consumed) {
      body_end_actual = js->pos;
    } else if (next(js) == TOK_RBRACE) {
      body_end_actual = js->pos;
      js->consumed = 1;
    } else {
      body_end_actual = js->pos;
    }
  }
  
  js->flags = flags;
  
  size_t fn_size = (params_end - params_start) + (body_end_actual - body_start) + 32;
  char *fn_str = (char *) malloc(fn_size);
  if (!fn_str) return js_mkerr(js, "oom");
  
  jsoff_t fn_pos = 0;
  
  size_t param_len = params_end - params_start;
  memcpy(fn_str + fn_pos, &js->code[params_start], param_len);
  fn_pos += param_len;

  if (is_expr) {
    fn_str[fn_pos++] = '{';
    memcpy(fn_str + fn_pos, "return ", 7);
    fn_pos += 7;
    size_t body_len = body_end_actual - body_start;
    memcpy(fn_str + fn_pos, &js->code[body_start], body_len);
    fn_pos += body_len;
    fn_str[fn_pos++] = '}';
  } else {
    size_t body_len = body_end_actual - body_start;
    memcpy(fn_str + fn_pos, &js->code[body_start], body_len);
    fn_pos += body_len;
  }

  jsval_t func_obj = mkobj(js, 0);
  if (is_err(func_obj)) { free(fn_str); return func_obj; }
  
  set_func_code(js, func_obj, fn_str, fn_pos);
  free(fn_str);
  
  if (is_async) {
    set_slot(js, func_obj, SLOT_ASYNC, js_true);
    jsval_t async_proto = get_slot(js, js_glob(js), SLOT_ASYNC_PROTO);
    if (vtype(async_proto) == T_FUNC) set_proto(js, func_obj, async_proto);
  } else {
    jsval_t func_proto = get_slot(js, js_glob(js), SLOT_FUNC_PROTO);
    if (vtype(func_proto) == T_FUNC) set_proto(js, func_obj, func_proto);
  }
  
  if (!(flags & F_NOEXEC)) {
    jsval_t closure_scope = for_let_capture_scope(js);
    if (is_err(closure_scope)) return closure_scope;
    set_slot(js, func_obj, SLOT_SCOPE, closure_scope);
    set_slot(js, func_obj, SLOT_THIS, js->this_val);
  }
  
  set_slot(js, func_obj, SLOT_ARROW, js_true);
  return mkval(T_FUNC, (unsigned long) vdata(func_obj));
}

static jsval_t js_async_arrow_paren(struct js *js) {
  jsoff_t paren_start = js->pos - 1;
  js->consumed = 1;
  jsoff_t saved_pos = js->pos;
  uint8_t saved_tok = js->tok;
  uint8_t saved_consumed = js->consumed;
  uint8_t saved_flags = js->flags;
  int paren_depth = 1;
  js->flags |= F_NOEXEC;
  while (paren_depth > 0 && next(js) != TOK_EOF) {
    if (js->tok == TOK_LPAREN) paren_depth++;
    else if (js->tok == TOK_RPAREN) paren_depth--;
    js->consumed = 1;
  }
  jsoff_t paren_end = js->pos;
  bool is_arrow = lookahead(js) == TOK_ARROW;
  js->pos = saved_pos;
  js->tok = saved_tok;
  js->consumed = saved_consumed;
  js->flags = saved_flags;
  if (is_arrow) {
    js->flags |= F_NOEXEC;
    while (next(js) != TOK_RPAREN && next(js) != TOK_EOF) {
      js->consumed = 1;
    }
    if (next(js) != TOK_RPAREN) return js_mkerr_typed(js, JS_ERR_SYNTAX, ") expected");
    js->consumed = 1;
    js->flags = saved_flags;
    if (next(js) != TOK_ARROW) return js_mkerr_typed(js, JS_ERR_SYNTAX, "=> expected");
    js->consumed = 1;
    return js_arrow_func(js, paren_start, paren_end, true);
  }
  return js_mkerr_typed(js, JS_ERR_SYNTAX, "async ( must be arrow function");
}

static jsval_t js_group(struct js *js) {
  if (next(js) == TOK_LPAREN) {
    if (++js->parse_depth > JS_MAX_PARSE_DEPTH) {
      js->parse_depth--;
      return js_mkerr_typed(js, JS_ERR_RANGE | JS_ERR_NO_STACK, "Maximum call stack size exceeded");
    }
    jsoff_t paren_start = js->pos - 1;
    js->consumed = 1;
    
    jsoff_t saved_pos = js->pos;
    uint8_t saved_tok = js->tok;
    uint8_t saved_consumed = js->consumed;
    uint8_t saved_flags = js->flags;
    
    int paren_depth = 1;
    bool could_be_arrow = true;
    js->flags |= F_NOEXEC;
    
    while (paren_depth > 0 && next(js) != TOK_EOF) {
      if (js->tok == TOK_LPAREN) paren_depth++;
      else if (js->tok == TOK_RPAREN) paren_depth--;
      if (paren_depth > 0 && !is_valid_arrow_param_tok(js->tok)) could_be_arrow = false;
      js->consumed = 1;
    }
    
    jsoff_t paren_end = js->pos;
    bool is_arrow = could_be_arrow && lookahead(js) == TOK_ARROW;
    
    js->pos = saved_pos;
    js->tok = saved_tok;
    js->consumed = saved_consumed;
    js->flags = saved_flags;
    
    if (is_arrow) {
      js->flags |= F_NOEXEC;
      int skip_paren_depth = 1;
      while (skip_paren_depth > 0 && next(js) != TOK_EOF) {
        if (js->tok == TOK_LPAREN) skip_paren_depth++;
        else if (js->tok == TOK_RPAREN) skip_paren_depth--;
        if (skip_paren_depth > 0) js->consumed = 1;
      }
      
      if (next(js) != TOK_RPAREN) { 
        js->parse_depth--; 
        return js_mkerr_typed(js, JS_ERR_SYNTAX, ") expected"); 
      }
      
      js->consumed = 1;
      js->flags = saved_flags;
      
      if (next(js) != TOK_ARROW) { 
        js->parse_depth--; 
        return js_mkerr_typed(js, JS_ERR_SYNTAX, "=> expected"); 
      }
      
      js->consumed = 1;
      js->parse_depth--;
      
      return js_arrow_func(js, paren_start, paren_end, false);
    } else {
      if (next(js) == TOK_RPAREN) {
        js->parse_depth--;
        return js_mkerr_typed(js, JS_ERR_SYNTAX, "Parenthesized expression cannot be empty");
      }
      
      jsval_t v = js_expr(js);
      if (is_err(v)) { js->parse_depth--; return v; }
      
      while (next(js) == TOK_COMMA) {
        js->consumed = 1;
        v = js_expr(js);
        if (is_err(v)) { js->parse_depth--; return v; }
      }
      
      if (next(js) != TOK_RPAREN) { 
        js->parse_depth--;
        return js_mkerr_typed(js, JS_ERR_SYNTAX, ") expected"); 
      }
      
      js->consumed = 1;
      js->parse_depth--;
      
      return v;
    }
  } else return js_literal(js);
}

static jsval_t js_call_dot(struct js *js) {
  jsval_t res = js_group(js);
  jsval_t obj = js_mkundef();
  if (is_err(res)) return res;
  if (vtype(res) == T_CODEREF) {
    if (lookahead(js) == TOK_ARROW) return res;
    if (lookahead(js) == TOK_TEMPLATE) {
      jsval_t tag_func = lookup(js, &js->code[coderefoff(res)], codereflen(res));
      if (is_err(tag_func)) return tag_func;
      if (!(js->flags & F_NOEXEC) && !is_err(tag_func)) tag_func = resolveprop(js, tag_func);
      js->consumed = 1;
      next(js);
      js->consumed = 1;
      res = js_tagged_template(js, tag_func);
      if (is_err(res)) return res;
      goto js_call_dot_loop;
    }
    if ((js->flags & F_STRICT) && is_eval_or_arguments(js, coderefoff(res), codereflen(res))) {
      uint8_t la = lookahead(js);
      if (la == TOK_ASSIGN || la == TOK_PLUS_ASSIGN || la == TOK_MINUS_ASSIGN ||
          la == TOK_MUL_ASSIGN || la == TOK_DIV_ASSIGN || la == TOK_REM_ASSIGN ||
          la == TOK_SHL_ASSIGN || la == TOK_SHR_ASSIGN || la == TOK_ZSHR_ASSIGN ||
          la == TOK_AND_ASSIGN || la == TOK_XOR_ASSIGN || la == TOK_OR_ASSIGN ||
          la == TOK_POSTINC || la == TOK_POSTDEC) {
        return js_mkerr_typed(js, JS_ERR_SYNTAX, "cannot modify eval or arguments in strict mode");
      }
    }
    jsoff_t id_off = coderefoff(res);
    jsoff_t id_len = codereflen(res);
    jsoff_t saved_pos = js->pos;
    uint8_t saved_tok = js->tok;
    uint8_t saved_consumed = js->consumed;
    res = lookup(js, &js->code[id_off], id_len);
    if (is_err(res)) {
      if (!(js->flags & F_STRICT) && !(js->flags & F_NOEXEC)) {
        js->pos = saved_pos;
        js->tok = saved_tok;
        js->consumed = saved_consumed;
        uint8_t next_tok = next(js);
        if (next_tok == TOK_ASSIGN) {
          js->flags &= (uint8_t)~F_THROW;
          js->thrown_value = js_mkundef();
          if (js->errmsg) js->errmsg[0] = '\0';
          return mkcoderef(id_off, id_len);
        }
      }
      if (is_err(res)) return res;
    }
  }
  js_call_dot_loop:
  while (next(js) == TOK_LPAREN || next(js) == TOK_DOT || next(js) == TOK_OPTIONAL_CHAIN || next(js) == TOK_LBRACKET || next(js) == TOK_TEMPLATE) {
    if (js->tok == TOK_TEMPLATE) {
      if (vtype(res) == T_PROP) res = resolveprop(js, res);
      if (is_err(res)) return res;
      js->consumed = 1;
      res = js_tagged_template(js, res);
      if (is_err(res)) return res;
    } else if (js->tok == TOK_DOT || js->tok == TOK_OPTIONAL_CHAIN) {
      uint8_t op = js->tok;
      js->consumed = 1;
      if (vtype(res) != T_PROP && vtype(res) != T_PROPREF) {
        obj = res;
      } else obj = resolveprop(js, res);
      jsval_t prop_name;
      uint8_t nxt = next(js);
      if (nxt == TOK_HASH) {
        js->consumed = 1;
        if (next(js) != TOK_IDENTIFIER) {
          return js_mkerr_typed(js, JS_ERR_SYNTAX, "private field name expected");
        }
        js->consumed = 1;
        prop_name = mkcoderef((jsoff_t) js->toff, (jsoff_t) js->tlen);
      } else if (nxt == TOK_IDENTIFIER || is_keyword_propname(nxt)) {
        js->consumed = 1;
        prop_name = mkcoderef((jsoff_t) js->toff, (jsoff_t) js->tlen);
      } else if (nxt == TOK_LBRACKET) {
        js->consumed = 1;
        jsval_t idx = js_expr(js);
        if (is_err(idx)) return idx;
        if (next(js) != TOK_RBRACKET) return js_mkerr_typed(js, JS_ERR_SYNTAX, "] expected");
        js->consumed = 1;
        if (op == TOK_OPTIONAL_CHAIN && (vtype(obj) == T_NULL || vtype(obj) == T_UNDEF)) {
          res = js_mkundef();
        } else {
          res = do_op(js, TOK_BRACKET, res, idx);
        }
        continue;
      } else {
        prop_name = js_group(js);
      }
      if (op == TOK_OPTIONAL_CHAIN && (vtype(obj) == T_NULL || vtype(obj) == T_UNDEF)) {
        res = js_mkundef();
      } else {
        res = do_op(js, op, res, prop_name);
      }
    } else if (js->tok == TOK_LBRACKET) {
      js->consumed = 1;
      if (vtype(res) != T_PROP && vtype(res) != T_PROPREF) {
        obj = res;
      } else {
        obj = resolveprop(js, res);
      }
      jsval_t idx = js_expr(js);
      if (is_err(idx)) return idx;
      if (next(js) != TOK_RBRACKET) return js_mkerr_typed(js, JS_ERR_SYNTAX, "] expected");
      js->consumed = 1;
      res = do_op(js, TOK_BRACKET, res, idx);
    } else {
      jsval_t func_this = obj;
      bool is_super_call = (vtype(js->super_val) != T_UNDEF && res == js->super_val);
      if (vtype(obj) == T_UNDEF) {
        if (vtype(res) == T_PROPREF) {
          jsoff_t obj_off = propref_obj(res);
          func_this = mkval(T_OBJ, obj_off);
        } else func_this = js->this_val;
      }
      push_this(func_this);
      jsval_t params = js_call_params(js);
      if (is_err(params)) {
        pop_this();
        return params;
      }
      res = do_op(js, TOK_CALL, res, params);
      pop_this();
      if (is_super_call && !is_err(res) && is_object_type(res)) {
        jsoff_t proto_off = lkp_interned(js, mkval(T_OBJ, vdata(js->current_func)), INTERN_PROTOTYPE, 9);
        if (proto_off) set_proto(js, res, resolveprop(js, mkval(T_PROP, proto_off)));
        js->this_val = res;
        if (global_this_stack.depth > 0) global_this_stack.stack[global_this_stack.depth - 1] = res;
      }
      obj = js_mkundef();
    }
  }
  return res;
}

static jsval_t js_postfix(struct js *js) {
  jsval_t res = js_call_dot(js);
  if (is_err(res)) return res;
  next(js);
  if ((js->tok == TOK_POSTINC || js->tok == TOK_POSTDEC) && !js->had_newline) {
    js->consumed = 1;
    res = do_op(js, js->tok, res, 0);
  }
  return res;
}

static inline jsval_t resolve_coderef(struct js *js, jsval_t v) {
  if (vtype(v) == T_CODEREF) return lookup(js, &js->code[coderefoff(v)], codereflen(v));
  return v;
}

static void unlink_prop(struct js *js, jsoff_t obj_off, jsoff_t prop_off, jsoff_t prev_off) {
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

static jsval_t check_frozen_sealed(struct js *js, jsval_t obj, const char *action) {
  if (js_truthy(js, get_slot(js, obj, SLOT_FROZEN))) {
    if (js->flags & F_STRICT) return js_mkerr(js, "cannot %s property of frozen object", action);
    return js_false;
  }
  if (js_truthy(js, get_slot(js, obj, SLOT_SEALED))) {
    if (js->flags & F_STRICT) return js_mkerr(js, "cannot %s property of sealed object", action);
    return js_false;
  }
  return js_mkundef();
}

static jsval_t js_unary(struct js *js) {
  uint8_t tok = next(js);

  static const void *dispatch[] = {
    [TOK_NEW]     = &&do_new,
    [TOK_DELETE]  = &&do_delete,
    [TOK_AWAIT]   = &&do_await,
    [TOK_POSTINC] = &&do_prefix_inc,
    [TOK_POSTDEC] = &&do_prefix_inc,
    [TOK_NOT]     = &&do_unary_op,
    [TOK_TILDA]   = &&do_unary_op,
    [TOK_TYPEOF]  = &&do_typeof,
    [TOK_VOID]    = &&do_unary_op,
    [TOK_MINUS]   = &&do_unary_op,
    [TOK_PLUS]    = &&do_unary_op,
  };

  if (tok < sizeof(dispatch)/sizeof(dispatch[0]) && dispatch[tok]) {
    goto *dispatch[tok];
  }
  return js_postfix(js);

  do_new: {
    js->consumed = 1;
    jsval_t obj = mkobj(js, 0);
    jsval_t saved_this = js->this_val;
    jsval_t saved_new_target = js->new_target;

    jsval_t ctor = js_group(js);
    if (is_err(ctor)) { return ctor; }

    while (next(js) == TOK_DOT || next(js) == TOK_LBRACKET) {
      ctor = resolve_coderef(js, ctor);
      if (is_err(ctor)) { return ctor; }

      if (js->tok == TOK_DOT) {
        js->consumed = 1;
        if (next(js) != TOK_IDENTIFIER && !is_keyword_propname(js->tok)) {
          return js_mkerr_typed(js, JS_ERR_SYNTAX, "identifier expected");
        }
        js->consumed = 1;
        ctor = do_op(js, TOK_DOT, ctor, mkcoderef((jsoff_t)js->toff, (jsoff_t)js->tlen));
      } else {
        js->consumed = 1;
        jsval_t idx = js_expr(js);
        if (is_err(idx)) { return idx; }
        if (next(js) != TOK_RBRACKET) { return js_mkerr_typed(js, JS_ERR_SYNTAX, "] expected"); }
        js->consumed = 1;
        ctor = do_op(js, TOK_BRACKET, ctor, idx);
      }
    }

    ctor = resolve_coderef(js, ctor);
    if (is_err(ctor)) { return ctor; }
    if (vtype(ctor) == T_PROP || vtype(ctor) == T_PROPREF) ctor = resolveprop(js, ctor);

    js->new_target = ctor;

    jsval_t result;
    push_this(obj);
    if (next(js) == TOK_LPAREN) {
      jsval_t params = js_call_params(js);
      if (is_err(params)) { 
        pop_this(); 
        js->new_target = saved_new_target; return params; 
      }
      result = do_op(js, TOK_CALL, ctor, params);
    } else {
      result = do_op(js, TOK_CALL, ctor, mkcoderef(0, 0));
      js->consumed = 0;
    }
    
    jsval_t constructed_obj = peek_this();
    pop_this();
    
    js->this_val = saved_this;
    js->new_target = saved_new_target;
    
    if (is_err(result)) return result;
    
    uint8_t rtype = vtype(result);
    jsval_t new_result = (
      rtype == T_OBJ || rtype == T_ARR ||
      rtype == T_PROMISE || rtype == T_FUNC
     ) ? result : constructed_obj;
    
    if (vtype(new_result) == T_OBJ && (vtype(ctor) == T_FUNC || vtype(ctor) == T_CFUNC)) {
      set_slot(js, new_result, SLOT_CTOR, ctor);
    }

    jsval_t call_obj = js_mkundef();
    while (next(js) == TOK_DOT || next(js) == TOK_LBRACKET || next(js) == TOK_OPTIONAL_CHAIN || next(js) == TOK_LPAREN) {
      uint8_t op = js->tok;
      if (op == TOK_DOT || op == TOK_OPTIONAL_CHAIN) {
        js->consumed = 1;
        call_obj = new_result;
        if (op == TOK_OPTIONAL_CHAIN && (vtype(call_obj) == T_NULL || vtype(call_obj) == T_UNDEF)) {
          new_result = call_obj = js_mkundef();
        } else {
          if (next(js) != TOK_IDENTIFIER && !is_keyword_propname(js->tok)) {
            return js_mkerr_typed(js, JS_ERR_SYNTAX, "identifier expected");
          }
          js->consumed = 1;
          new_result = do_op(js, op, new_result, mkcoderef((jsoff_t)js->toff, (jsoff_t)js->tlen));
        }
      } else if (op == TOK_LBRACKET) {
        js->consumed = 1;
        call_obj = new_result;
        jsval_t idx = js_expr(js);
        if (is_err(idx)) return idx;
        if (next(js) != TOK_RBRACKET) return js_mkerr_typed(js, JS_ERR_SYNTAX, "] expected");
        js->consumed = 1;
        new_result = do_op(js, TOK_BRACKET, new_result, idx);
      } else {
        jsval_t func_this = vtype(call_obj) == T_UNDEF ? js->this_val : call_obj;
        push_this(func_this);
        jsval_t params = js_call_params(js);
        if (is_err(params)) { pop_this(); return params; }
        new_result = do_op(js, TOK_CALL, new_result, params);
        pop_this();
        call_obj = js_mkundef();
      }
    }
    return new_result;
  }

  do_delete: {
    js->consumed = 1;

    if ((js->flags & F_STRICT) && next(js) == TOK_IDENTIFIER) {
      jsoff_t id_pos = js->pos;
      uint8_t id_tok = js->tok;
      jsoff_t id_toff = js->toff, id_tlen = js->tlen;
      js->consumed = 1;
      uint8_t after = next(js);
      if (after != TOK_DOT && after != TOK_LBRACKET && after != TOK_OPTIONAL_CHAIN) {
        return js_mkerr_typed(js, JS_ERR_SYNTAX, "cannot delete unqualified identifier in strict mode");
      }
      js->pos = id_pos; js->tok = id_tok; js->toff = id_toff; js->tlen = id_tlen; js->consumed = 0;
    }

    js_parse_state_t saved_state;
    JS_SAVE_STATE(js, saved_state);
    uint8_t saved_flags = js->flags;
    jsval_t operand = js_postfix(js);

    if (is_err(operand)) {
      JS_RESTORE_STATE(js, saved_state);
      js->flags = (saved_flags & ~F_THROW) | F_NOEXEC;
      js_postfix(js);
      js->flags = saved_flags;
      return js_true;
    }
    if (js->flags & F_NOEXEC) return js_true;

    if (vtype(operand) == T_PROPREF) {
      jsoff_t obj_off = propref_obj(operand);
      jsoff_t key_off = propref_key(operand);
      jsval_t obj = mkval(T_OBJ, obj_off);
      jsval_t key = mkval(T_STR, key_off);
      jsoff_t len;
      const char *key_str = (const char *)&js->mem[vstr(js, key, &len)];

      if (is_proxy(js, obj)) {
        jsval_t result = proxy_delete(js, obj, key_str, len);
        return is_err(result) ? result : js_bool(js_truthy(js, result));
      }

      jsval_t err = check_frozen_sealed(js, obj, "delete");
      if (vtype(err) != T_UNDEF) return err;

      jsoff_t prop_off = lkp(js, obj, key_str, len);
      if (prop_off == 0) return js_true;

      if (is_nonconfig_prop(js, prop_off)) {
        if (js->flags & F_STRICT) return js_mkerr_typed(js, JS_ERR_TYPE, "cannot delete non-configurable property");
        return js_false;
      }

      descriptor_entry_t *desc = lookup_descriptor(obj_off, key_str, len);
      if (desc && !desc->configurable) {
        if (js->flags & F_STRICT) return js_mkerr_typed(js, JS_ERR_TYPE, "cannot delete non-configurable property");
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

    if (vtype(operand) != T_PROP) return js_true;

    jsoff_t prop_off = (jsoff_t)vdata(operand);
    if (is_nonconfig_prop(js, prop_off)) {
      if (js->flags & F_STRICT) return js_mkerr(js, "cannot delete non-configurable property");
      return js_false;
    }

    jsoff_t owner_obj_off = 0, prev_prop_off = 0;
    bool is_first_prop = false;
    for (jsoff_t off = 0; off < js->brk; ) {
      jsoff_t v = loadoff(js, off);
      jsoff_t cleaned = v & ~FLAGMASK;
      jsoff_t n = esize(cleaned);
      if ((cleaned & 3) == T_OBJ) {
        jsoff_t first_prop = cleaned & ~3ULL;
        if (first_prop == prop_off) { owner_obj_off = off; is_first_prop = true; break; }
        for (jsoff_t cur = first_prop; cur != 0 && cur < js->brk; ) {
          jsoff_t nx = loadoff(js, cur) & ~(3U | FLAGMASK);
          if (nx == prop_off) { owner_obj_off = off; prev_prop_off = cur; break; }
          cur = nx;
        }
        if (owner_obj_off) break;
      }
      off += n;
    }

    if (owner_obj_off) {
      jsval_t owner_obj = mkval(T_OBJ, owner_obj_off);
      jsval_t err = check_frozen_sealed(js, owner_obj, "delete");
      if (vtype(err) != T_UNDEF) return err;

      jsoff_t key_str_off = loadoff(js, (jsoff_t)(prop_off + sizeof(jsoff_t)));
      jsoff_t key_len = (loadoff(js, key_str_off) >> 3) - 1;
      const char *key_str = (char *)&js->mem[key_str_off + sizeof(jsoff_t)];
      descriptor_entry_t *desc = lookup_descriptor(owner_obj_off, key_str, key_len);
      if (desc && !desc->configurable) {
        if (js->flags & F_STRICT) return js_mkerr_typed(js, JS_ERR_TYPE, "cannot delete non-configurable property");
        return js_false;
      }
      unlink_prop(js, owner_obj_off, prop_off, is_first_prop ? 0 : prev_prop_off);
    }
    return js_true;
  }

  do_await: {
    js->consumed = 1;
    jsval_t expr = js_unary(js);
    if (is_err(expr)) return expr;
    if (js->flags & F_NOEXEC) return expr;

    jsval_t resolved = resolveprop(js, expr);
    if (vtype(resolved) != T_PROMISE) return resolved;

    uint32_t pid = get_promise_id(js, resolved);
    promise_data_entry_t *pd = get_promise_data(pid, false);
    if (!pd) return js_mkerr(js, "invalid promise state");

    if (pd->state == 1) return pd->value;
    if (pd->state == 2) return js_throw(js, pd->value);

    mco_coro *current_mco = mco_running();
    if (!current_mco) return js_mkerr(js, "await can only be used inside async functions");

    async_exec_context_t *ctx = (async_exec_context_t *)mco_get_user_data(current_mco);
    if (!ctx || !ctx->coro) return js_mkerr(js, "invalid async context");

    coroutine_t *coro = ctx->coro;
    coro->awaited_promise = resolved;
    coro->is_settled = coro->is_ready = false;

    jsval_t resume_obj = mkobj(js, 0);
    set_slot(js, resume_obj, SLOT_CFUNC, js_mkfun(resume_coroutine_wrapper));
    set_slot(js, resume_obj, SLOT_CORO, tov((double)(uintptr_t)coro));

    jsval_t reject_obj = mkobj(js, 0);
    set_slot(js, reject_obj, SLOT_CFUNC, js_mkfun(reject_coroutine_wrapper));
    set_slot(js, reject_obj, SLOT_CORO, tov((double)(uintptr_t)coro));

    jsval_t then_args[] = { mkval(T_FUNC, vdata(resume_obj)), mkval(T_FUNC, vdata(reject_obj)) };
    jsval_t saved_this = js->this_val;
    js->this_val = resolved;
    (void)builtin_promise_then(js, then_args, 2);
    js->this_val = saved_this;

    js_parse_state_t saved;
    JS_SAVE_STATE(js, saved);
    uint8_t saved_flags = js->flags;
    mco_result mco_res = mco_yield(current_mco);
    
    JS_RESTORE_STATE(js, saved);
    js->flags = saved_flags;

    if (mco_res != MCO_SUCCESS) return js_mkerr(js, "failed to yield coroutine");

    jsval_t result = coro->result;
    bool is_error = coro->is_error;
    coro->is_settled = false;
    coro->awaited_promise = js_mkundef();

    return is_error ? js_throw(js, result) : result;
  }

  do_prefix_inc: {
    uint8_t op = js->tok;
    js->consumed = 1;
    if ((js->flags & F_STRICT) && next(js) == TOK_IDENTIFIER && is_eval_or_arguments(js, js->toff, js->tlen)) {
      return js_mkerr_typed(js, JS_ERR_SYNTAX, "cannot modify eval or arguments in strict mode");
    }
    jsval_t operand = js_unary(js);
    if (is_err(operand)) return operand;
    if (js->flags & F_NOEXEC) return operand;
    jsval_t resolved = resolveprop(js, operand);
    if (vtype(operand) == T_PROP || vtype(operand) == T_PROPREF) {
      do_assign_op(js, op == TOK_POSTINC ? TOK_PLUS_ASSIGN : TOK_MINUS_ASSIGN, operand, tov(1));
    } else {
      return js_mkerr_typed(js, JS_ERR_SYNTAX, "Invalid left-hand side in assignment");
    }
    return do_op(js, op == TOK_POSTINC ? TOK_PLUS : TOK_MINUS, resolved, tov(1));
  }
  
  do_typeof: {
    js->consumed = 1;
    jsoff_t saved_pos = js->pos;
    uint8_t saved_tok = js->tok, saved_consumed = js->consumed, saved_flags = js->flags;
    bool bare = is_typeof_bare_ident(js);
    jsval_t operand = js_unary(js);
    if (is_err(operand)) {
      if (!bare || get_error_type(js) != JS_ERR_REFERENCE) return operand;
      js->pos = saved_pos; js->tok = saved_tok; js->consumed = saved_consumed;
      js->flags = (saved_flags & ~F_THROW) | F_NOEXEC;
      js_unary(js);
      js->flags = saved_flags & ~F_THROW;
      operand = js_mkundef();
    }
    return do_op(js, TOK_TYPEOF, js_mkundef(), operand);
  }

  do_unary_op: {
    uint8_t t = js->tok;
    if (t == TOK_MINUS) t = TOK_UMINUS;
    if (t == TOK_PLUS) t = TOK_UPLUS;
    js->consumed = 1;
    jsval_t operand = js_unary(js);
    if (is_err(operand)) return operand;
    if (next(js) == TOK_EXP) {
      return js_mkerr_typed(
        js, JS_ERR_SYNTAX, 
        "Unary operator used immediately before exponentiation expression. Parenthesis must be used to disambiguate operator precedence"
      );
    }
    return do_op(js, t, js_mkundef(), operand);
  }
}

static jsval_t js_expr_bp(struct js *js, uint8_t min_bp) {
  jsval_t lhs = js_unary(js);
  if (is_err(lhs)) return lhs;

  loop: {
    uint8_t tok = next(js);
    if (tok >= TOK_MAX) return lhs;
    uint8_t bp = prec_table[tok];
    if (bp == 0 || bp < min_bp) return lhs;

    js->consumed = 1;

    static const void *dispatch[] = {
      [TOK_LOR]        = &&do_lor,
      [TOK_LAND]       = &&do_land,
      [TOK_NULLISH]    = &&do_nullish,
      [TOK_EXP]        = &&do_exp,
      [TOK_OR]         = &&do_binop,
      [TOK_XOR]        = &&do_binop,
      [TOK_AND]        = &&do_binop,
      [TOK_EQ]         = &&do_binop,
      [TOK_NE]         = &&do_binop,
      [TOK_SEQ]        = &&do_binop,
      [TOK_SNE]        = &&do_binop,
      [TOK_LT]         = &&do_binop,
      [TOK_LE]         = &&do_binop,
      [TOK_GT]         = &&do_binop,
      [TOK_GE]         = &&do_binop,
      [TOK_INSTANCEOF] = &&do_binop,
      [TOK_IN]         = &&do_binop,
      [TOK_SHL]        = &&do_binop,
      [TOK_SHR]        = &&do_binop,
      [TOK_ZSHR]       = &&do_binop,
      [TOK_PLUS]       = &&do_binop,
      [TOK_MINUS]      = &&do_binop,
      [TOK_MUL]        = &&do_binop,
      [TOK_DIV]        = &&do_binop,
      [TOK_REM]        = &&do_binop,
    };

    goto *dispatch[tok];

    do_binop: {
      jsval_t rhs = js_expr_bp(js, bp + 1);
      if (is_err(rhs)) return rhs;
      lhs = do_op(js, tok, lhs, rhs);
      goto loop;
    }

    do_exp: {
      jsval_t rhs = js_expr_bp(js, bp);
      if (is_err(rhs)) return rhs;
      lhs = do_op(js, TOK_EXP, lhs, rhs);
      goto loop;
    }

    do_lor: {
      uint8_t flags = js->flags;
      lhs = resolveprop(js, lhs);
      if (js_truthy(js, lhs)) js->flags |= F_NOEXEC;
      jsval_t rhs = js_expr_bp(js, bp);
      if (!(flags & F_NOEXEC) && !js_truthy(js, lhs)) lhs = rhs;
      js->flags = flags;
      if (is_err(rhs)) return rhs;
      goto loop;
    }

    do_land: {
      uint8_t flags = js->flags;
      lhs = resolveprop(js, lhs);
      if (!js_truthy(js, lhs)) js->flags |= F_NOEXEC;
      jsval_t rhs = js_expr_bp(js, bp);
      if (!(flags & F_NOEXEC) && js_truthy(js, lhs)) lhs = rhs;
      js->flags = flags;
      if (is_err(rhs)) return rhs;
      goto loop;
    }

    do_nullish: {
      uint8_t flags = js->flags;
      lhs = resolveprop(js, lhs);
      uint8_t lhs_type = vtype(lhs);
      if (lhs_type != T_NULL && lhs_type != T_UNDEF) js->flags |= F_NOEXEC;
      jsval_t rhs = js_expr_bp(js, bp);
      if (!(flags & F_NOEXEC) && (lhs_type == T_NULL || lhs_type == T_UNDEF)) lhs = rhs;
      js->flags = flags;
      if (is_err(rhs)) return rhs;
      goto loop;
    }
  }
}

static jsval_t js_ternary(struct js *js) {
  jsval_t res = js_expr_bp(js, 1);
  if (next(js) == TOK_Q) {
    uint8_t flags = js->flags;
    js->consumed = 1;
    if (js_truthy(js, resolveprop(js, res))) {
      res = js_ternary(js);
      js->flags |= F_NOEXEC;
      EXPECT(TOK_COLON, js->flags = flags);
      js_ternary(js);
      js->flags = flags;
    } else {
      js->flags |= F_NOEXEC;
      js_ternary(js);
      EXPECT(TOK_COLON, js->flags = flags);
      js->flags = flags;
      res = js_ternary(js);
    }
  }
  return res;
}

static jsval_t js_assignment(struct js *js) {
  jsval_t res = js_ternary(js);
  
  if (!is_err(res) && vtype(res) == T_CODEREF && next(js) == TOK_ARROW) {
    jsoff_t param_start = coderefoff(res);
    jsoff_t param_len = codereflen(res);
    js->consumed = 1;
    
    char param_buf[256];
    if (param_len + 3 > sizeof(param_buf)) return js_mkerr(js, "param too long");
    param_buf[0] = '(';
    memcpy(param_buf + 1, &js->code[param_start], param_len);
    param_buf[param_len + 1] = ')';
    param_buf[param_len + 2] = '\0';
    
    uint8_t flags = js->flags;
    bool is_expr = next(js) != TOK_LBRACE;
    jsoff_t body_start = js->pos;
    if (is_expr && js->tok != TOK_EOF) {
      body_start = js->toff;
    }
    jsval_t body_result;
    
    if (is_expr) {
      js->flags |= F_NOEXEC;
      body_result = js_assignment(js);
      if (is_err(body_result)) {
        js->flags = flags;
        return body_result;
      }
    } else {
      body_start = js->toff;
      js->flags |= F_NOEXEC;
      js->consumed = 1;
      body_result = js_block(js, false);
      if (is_err(body_result)) {
        js->flags = flags;
        return body_result;
      }
      if (js->tok == TOK_RBRACE && js->consumed) {
      } else if (next(js) == TOK_RBRACE) {
        js->consumed = 1;
      }
    }

    js->flags = flags;
    jsoff_t body_end;
    if (is_expr) {
      uint8_t tok = next(js);
      body_end = is_body_end_tok(tok) ? js->toff : js->pos;
    } else body_end = js->pos;

    size_t fn_size = param_len + (body_end - body_start) + 64;
    char *fn_str = (char *) malloc(fn_size);
    if (!fn_str) return js_mkerr(js, "oom");

    jsoff_t fn_pos = 0;
    memcpy(fn_str + fn_pos, param_buf, param_len + 2);
    fn_pos += param_len + 2;

    if (is_expr) {
      fn_str[fn_pos++] = '{';
      memcpy(fn_str + fn_pos, "return ", 7);
      fn_pos += 7;
      size_t body_len = body_end - body_start;
      memcpy(fn_str + fn_pos, &js->code[body_start], body_len);
      fn_pos += body_len;
      fn_str[fn_pos++] = '}';
    } else {
      size_t body_len = body_end - body_start;
      memcpy(fn_str + fn_pos, &js->code[body_start], body_len);
      fn_pos += body_len;
    }
    
    jsval_t func_obj = mkobj(js, 0);
    if (is_err(func_obj)) { free(fn_str); return func_obj; }
    set_func_code(js, func_obj, fn_str, fn_pos);
    free(fn_str);
    
    jsval_t func_proto = get_slot(js, js_glob(js), SLOT_FUNC_PROTO);
    if (vtype(func_proto) == T_FUNC) set_proto(js, func_obj, func_proto);
    
    if (!(flags & F_NOEXEC)) {
      jsval_t closure_scope = for_let_capture_scope(js);
      if (is_err(closure_scope)) return closure_scope;
      set_slot(js, func_obj, SLOT_SCOPE, closure_scope);
      set_slot(js, func_obj, SLOT_THIS, js->this_val);
    }
    
    return mkval(T_FUNC, (unsigned long) vdata(func_obj));
  }
  
  while (
    !is_err(res) && (next(js) == TOK_ASSIGN || js->tok == TOK_PLUS_ASSIGN ||
    js->tok == TOK_MINUS_ASSIGN || js->tok == TOK_MUL_ASSIGN ||
    js->tok == TOK_DIV_ASSIGN || js->tok == TOK_REM_ASSIGN ||
    js->tok == TOK_SHL_ASSIGN || js->tok == TOK_SHR_ASSIGN ||
    js->tok == TOK_ZSHR_ASSIGN || js->tok == TOK_AND_ASSIGN ||
    js->tok == TOK_XOR_ASSIGN || js->tok == TOK_OR_ASSIGN ||
    js->tok == TOK_LOR_ASSIGN || js->tok == TOK_LAND_ASSIGN ||
    js->tok == TOK_NULLISH_ASSIGN)
  ) {
    uint8_t op = js->tok;
    js->consumed = 1;
    
    if (op == TOK_LOR_ASSIGN || op == TOK_LAND_ASSIGN || op == TOK_NULLISH_ASSIGN) {
      jsval_t lhs_val = js_mkundef();
      bool should_assign = true;
      
      if (!(js->flags & F_NOEXEC)) {
        lhs_val = resolveprop(js, res);
        if (is_err(lhs_val)) return lhs_val;
        
        if (op == TOK_LOR_ASSIGN) {
          should_assign = !js_truthy(js, lhs_val);
        } else if (op == TOK_LAND_ASSIGN) {
          should_assign = js_truthy(js, lhs_val);
        } else should_assign = is_null(lhs_val) || is_undefined(lhs_val);
      }
      
      if (should_assign || (js->flags & F_NOEXEC)) {
        jsval_t rhs = js_assignment(js);
        if (is_err(rhs)) return rhs;
        
        if (!(js->flags & F_NOEXEC) && should_assign) {
          jsval_t rhs_resolved = resolveprop(js, rhs);
          if (is_err(rhs_resolved)) return rhs_resolved;
          res = assign(js, res, rhs_resolved);
        }
      } else {
        uint8_t saved_flags = js->flags;
        js->flags |= F_NOEXEC;
        jsval_t rhs = js_assignment(js);
        js->flags = saved_flags;
        if (is_err(rhs)) return rhs;
        res = lhs_val;
      }
      continue;
    }
    
    jsval_t lhs_val = js_mkundef();
    if (op != TOK_ASSIGN && !(js->flags & F_NOEXEC)) {
      lhs_val = resolveprop(js, res);
      if (is_err(lhs_val)) return lhs_val;
    }
    
    jsval_t rhs = js_assignment(js);
    if (is_err(rhs)) return rhs;
    
    if (op == TOK_ASSIGN) {
      res = do_op(js, op, res, rhs);
    } else {
      jsval_t rhs_resolved = resolveprop(js, rhs);
      if (is_err(rhs_resolved)) return rhs_resolved;
      
      uint8_t m[] = {
        TOK_PLUS, TOK_MINUS, TOK_MUL, TOK_DIV, TOK_REM, TOK_SHL,
        TOK_SHR,  TOK_ZSHR,  TOK_AND, TOK_XOR, TOK_OR
      };
      uint8_t binary_op = m[op - TOK_PLUS_ASSIGN];
      
      jsval_t op_result = do_op(js, binary_op, lhs_val, rhs_resolved);
      if (is_err(op_result)) return op_result;
      res = assign(js, res, op_result);
    }
  }
  
  return res;
}

static jsval_t js_decl(struct js *js, bool is_const) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  js->consumed = 1;
  for (;;) {
    if (next(js) == TOK_LBRACE) {
      js->consumed = 1;
      
      js_parse_state_t pattern_state;
      JS_SAVE_STATE(js, pattern_state);
      
      int depth = 1;
      while (depth > 0 && next(js) != TOK_EOF) {
        if (js->tok == TOK_LBRACE) depth++;
        else if (js->tok == TOK_RBRACE) depth--;
        if (depth > 0) js->consumed = 1;
      }
      js->consumed = 1;
      
      if (next(js) != TOK_ASSIGN) {
        return js_mkerr_typed(js, JS_ERR_SYNTAX, "destructuring requires assignment");
      }
      js->consumed = 1;
      
      jsval_t v = js_expr(js);
      if (is_err(v)) return v;
      
      jsval_t obj = js_mkundef();
      if (exe) {
        obj = resolveprop(js, v);
        if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR) {
          return js_mkerr(js, "cannot destructure non-object");
        }
      }
      
      js_parse_state_t end_state;
      JS_SAVE_STATE(js, end_state);
      JS_RESTORE_STATE(js, pattern_state);
      
      jsval_t picked_keys = exe ? js_mkarr(js) : js_mkundef();
      if (exe && is_err(picked_keys)) return picked_keys;
      
      while (next(js) != TOK_RBRACE && next(js) != TOK_EOF) {
        bool is_rest = false;
        if (next(js) == TOK_REST) {
          is_rest = true;
          js->consumed = 1;
        }
        
        if (next(js) != TOK_IDENTIFIER) {
          return js_mkerr_typed(js, JS_ERR_SYNTAX, "identifier expected in object destructuring");
        }
        jsoff_t src_off = js->toff, src_len = js->tlen;
        jsoff_t var_off = src_off, var_len = src_len;
        js->consumed = 1;
        
        bool is_nested_obj = false;
        bool is_nested_arr = false;
        jsoff_t nested_pattern_start = 0;
        
        if (!is_rest && next(js) == TOK_COLON) {
          js->consumed = 1;
          if (next(js) == TOK_LBRACE) {
            is_nested_obj = true;
            nested_pattern_start = js->toff;
            int inner_depth = 1;
            js->consumed = 1;
            while (inner_depth > 0 && next(js) != TOK_EOF) {
              if (js->tok == TOK_LBRACE) inner_depth++;
              else if (js->tok == TOK_RBRACE) inner_depth--;
              if (inner_depth > 0) js->consumed = 1;
            }
            js->consumed = 1;
          } else if (next(js) == TOK_LBRACKET) {
            is_nested_arr = true;
            nested_pattern_start = js->toff;
            int inner_depth = 1;
            js->consumed = 1;
            while (inner_depth > 0 && next(js) != TOK_EOF) {
              if (js->tok == TOK_LBRACKET) inner_depth++;
              else if (js->tok == TOK_RBRACKET) inner_depth--;
              if (inner_depth > 0) js->consumed = 1;
            }
            js->consumed = 1;
          } else {
            EXPECT_IDENT();
            var_off = js->toff;
            var_len = js->tlen;
            js->consumed = 1;
          }
        }
        
        jsoff_t default_off = 0, default_len = 0;
        if (!is_rest && !is_nested_obj && !is_nested_arr && next(js) == TOK_ASSIGN) {
          js->consumed = 1;
          default_off = js->pos;
          uint8_t sf = js->flags;
          js->flags |= F_NOEXEC;
          jsval_t r = js_expr(js);
          js->flags = sf;
          if (is_err(r)) return r;
          default_len = js->pos - default_off;
        }
        
        if (!exe) goto obj_destruct_next;
        
        if (is_rest) goto obj_destruct_rest;
        if (is_nested_obj || is_nested_arr) goto obj_destruct_nested;
        goto obj_destruct_simple;
        
obj_destruct_rest:;
        jsval_t rest_obj = mkobj(js, 0);
        if (is_err(rest_obj)) return rest_obj;
        jsoff_t scan = loadoff(js, (jsoff_t) vdata(obj)) & ~(3U | FLAGMASK);
        while (scan < js->brk && scan != 0) {
          jsoff_t header = loadoff(js, scan);
          if (is_slot_prop(header)) { scan = next_prop(header); continue; }
          
          const char *key; jsoff_t klen;
          get_prop_key(js, scan, &key, &klen);
          bool is_picked = false;
          jsoff_t picked_len = js_arr_len(js, picked_keys);
          for (jsoff_t i = 0; i < picked_len; i++) {
            jsval_t pk = js_arr_get(js, picked_keys, i);
            if (vtype(pk) != T_STR) continue;
            jsoff_t pklen, pkoff = vstr(js, pk, &pklen);
            if (klen == pklen && memcmp(key, &js->mem[pkoff], klen) == 0) { is_picked = true; break; }
          }
          if (!is_picked && !(klen == STR_PROTO_LEN && memcmp(key, STR_PROTO, STR_PROTO_LEN) == 0)) {
            jsval_t val = get_prop_val(js, scan);
            jsval_t key_str = js_mkstr(js, key, klen);
            if (is_err(key_str)) return key_str;
            jsval_t res = setprop(js, rest_obj, key_str, val);
            if (is_err(res)) return res;
          }
          scan = next_prop(header);
        }
        {
          const char *vn = &js->code[var_off];
          if (lkp_scope(js, js->scope, vn, var_len) > 0) return js_mkerr(js, "'%.*s' already declared", (int)var_len, vn);
          jsval_t x = mkprop(js, js->scope, js_mkstr(js, vn, var_len), rest_obj, is_const ? CONSTMASK : 0);
          if (is_err(x)) return x;
        }
        goto obj_destruct_next;
        
obj_destruct_nested:;
        {
          jsval_t sk = js_mkstr(js, &js->code[src_off], src_len);
          if (is_err(sk)) return sk;
          js_arr_push(js, picked_keys, sk);
          
          jsoff_t poff = lkp(js, obj, &js->code[src_off], src_len);
          jsval_t nobj = poff > 0 ? resolveprop(js, mkval(T_PROP, poff)) : js_mkundef();
          
          jsoff_t pattern_end = js->pos;
          js->pos = nested_pattern_start;
          js->consumed = 1;
          
          jsval_t saved_obj = obj;
          obj = nobj;
          
          if (!is_nested_obj) goto nested_done;
          if (next(js) != TOK_LBRACE) return js_mkerr_typed(js, JS_ERR_SYNTAX, "expected '{' in nested destructuring");
          js->consumed = 1;
          
          while (next(js) != TOK_RBRACE && next(js) != TOK_EOF) {
            if (next(js) != TOK_IDENTIFIER) return js_mkerr_typed(js, JS_ERR_SYNTAX, "identifier expected in nested destructuring");
            jsoff_t isoff = js->toff, islen = js->tlen;
            jsoff_t ivoff = isoff, ivlen = islen;
            js->consumed = 1;
            
            if (next(js) == TOK_COLON) {
              js->consumed = 1;
              EXPECT_IDENT();
              ivoff = js->toff; ivlen = js->tlen;
              js->consumed = 1;
            }
            
            const char *ivn = &js->code[ivoff];
            if (lkp_scope(js, js->scope, ivn, ivlen) > 0) return js_mkerr(js, "'%.*s' already declared", (int)ivlen, ivn);
            
            jsoff_t ipoff = lkp(js, nobj, &js->code[isoff], islen);
            jsval_t ival = ipoff > 0 ? resolveprop(js, mkval(T_PROP, ipoff)) : js_mkundef();
            jsval_t ix = mkprop(js, js->scope, js_mkstr(js, ivn, ivlen), ival, is_const ? CONSTMASK : 0);
            if (is_err(ix)) return ix;
            
            if (next(js) == TOK_RBRACE) break;
            EXPECT(TOK_COMMA);
          }
          js->consumed = 1;
          
nested_done:
          obj = saved_obj;
          js->pos = pattern_end;
          js->consumed = 1;
        }
        goto obj_destruct_next;
        
obj_destruct_simple:;
        {
          const char *vn = &js->code[var_off];
          if (lkp_scope(js, js->scope, vn, var_len) > 0) return js_mkerr(js, "'%.*s' already declared", (int)var_len, vn);
          
          jsval_t sk = js_mkstr(js, &js->code[src_off], src_len);
          if (is_err(sk)) return sk;
          js_arr_push(js, picked_keys, sk);
          
          jsoff_t poff = lkp(js, obj, &js->code[src_off], src_len);
          jsval_t pval = poff > 0 ? resolveprop(js, mkval(T_PROP, poff)) : js_mkundef();
          
          if (vtype(pval) == T_UNDEF && default_len > 0) {
            pval = js_eval_slice(js, default_off, default_len);
            if (is_err(pval)) return pval;
            pval = resolveprop(js, pval);
          }
          
          jsval_t x = mkprop(js, js->scope, js_mkstr(js, vn, var_len), pval, is_const ? CONSTMASK : 0);
          if (is_err(x)) return x;
        }
        
obj_destruct_next:
        
        if (next(js) == TOK_RBRACE) break;
        EXPECT(TOK_COMMA);
      }
      
      JS_RESTORE_STATE(js, end_state);
    } else if (next(js) == TOK_LBRACKET) {
      js->consumed = 1;
      
      js_parse_state_t pattern_state;
      JS_SAVE_STATE(js, pattern_state);
      
      int depth = 1;
      while (depth > 0 && next(js) != TOK_EOF) {
        if (js->tok == TOK_LBRACKET) depth++;
        else if (js->tok == TOK_RBRACKET) depth--;
        if (depth > 0) js->consumed = 1;
      }
      js->consumed = 1;
      
      if (next(js) != TOK_ASSIGN) {
        return js_mkerr_typed(js, JS_ERR_SYNTAX, "array destructuring requires assignment");
      }
      js->consumed = 1;
      
      jsval_t v = js_expr(js);
      if (is_err(v)) return v;
      
      jsval_t arr = js_mkundef();
      if (exe) {
        arr = resolveprop(js, v);
        if (vtype(arr) != T_ARR && vtype(arr) != T_STR) {
          return js_mkerr(js, "cannot array destructure non-iterable");
        }
      }
      
      js_parse_state_t end_state;
      JS_SAVE_STATE(js, end_state);
      JS_RESTORE_STATE(js, pattern_state);
      
      int index = 0;
      while (next(js) != TOK_RBRACKET && next(js) != TOK_EOF) {
        if (next(js) == TOK_COMMA) {
          js->consumed = 1;
          index++;
          continue;
        }
        
        bool is_rest = false;
        if (next(js) == TOK_REST) {
          is_rest = true;
          js->consumed = 1;
        }
        
        if (next(js) != TOK_IDENTIFIER) {
          return js_mkerr_typed(js, JS_ERR_SYNTAX, "identifier expected in array destructuring");
        }
        jsoff_t var_off = js->toff, var_len = js->tlen;
        js->consumed = 1;
        
        jsoff_t default_off = 0, default_len = 0;
        if (!is_rest && next(js) == TOK_ASSIGN) {
          js->consumed = 1;
          default_off = js->pos;
          uint8_t sf = js->flags;
          js->flags |= F_NOEXEC;
          jsval_t r = js_expr(js);
          js->flags = sf;
          if (is_err(r)) return r;
          default_len = js->pos - default_off;
        }
        
        if (exe) {
          const char *var_name = &js->code[var_off];
          if (lkp_scope(js, js->scope, var_name, var_len) > 0) {
            return js_mkerr(js, "'%.*s' already declared", (int)var_len, var_name);
          }
          
          jsval_t prop_val;
          if (is_rest) {
            jsval_t rest_arr = js_mkarr(js);
            if (is_err(rest_arr)) return rest_arr;
            jsoff_t total_len = vtype(arr) == T_STR ? 0 : js_arr_len(js, arr);
            if (vtype(arr) == T_STR) {
              jsoff_t slen;
              vstr(js, arr, &slen);
              total_len = slen;
            }
            for (jsoff_t i = index; i < total_len; i++) {
              jsval_t elem;
              if (vtype(arr) == T_STR) {
                jsoff_t slen, soff = vstr(js, arr, &slen);
                elem = js_mkstr(js, (char *)&js->mem[soff + i], 1);
              } else {
                elem = js_arr_get(js, arr, i);
              }
              js_arr_push(js, rest_arr, elem);
            }
            prop_val = rest_arr;
          } else {
            if (vtype(arr) == T_STR) {
              jsoff_t slen, soff = vstr(js, arr, &slen);
              if ((jsoff_t)index < slen) {
                prop_val = js_mkstr(js, (char *)&js->mem[soff + index], 1);
              } else {
                prop_val = js_mkundef();
              }
            } else {
              prop_val = js_arr_get(js, arr, index);
            }
            
            if (vtype(prop_val) == T_UNDEF && default_len > 0) {
              prop_val = js_eval_slice(js, default_off, default_len);
              if (is_err(prop_val)) return prop_val;
              prop_val = resolveprop(js, prop_val);
            }
          }
          
          jsval_t x = mkprop(js, js->scope, js_mkstr(js, var_name, var_len), prop_val, is_const ? CONSTMASK : 0);
          if (is_err(x)) return x;
        }
        
        index++;
        if (next(js) == TOK_RBRACKET) break;
        EXPECT(TOK_COMMA);
      }
      
      JS_RESTORE_STATE(js, end_state);
    } else {
      EXPECT_IDENT();
      js->consumed = 0;
      jsoff_t noff = js->toff, nlen = js->tlen;
      char *name = (char *) &js->code[noff];
      
      if ((js->flags & F_STRICT) && is_strict_restricted(name, nlen)) {
        return js_mkerr_typed(js, JS_ERR_SYNTAX, "cannot use '%.*s' as variable name in strict mode", (int) nlen, name);
      }
      
      if ((js->flags & F_STRICT) && is_strict_reserved(name, nlen)) {
        return js_mkerr_typed(js, JS_ERR_SYNTAX, "'%.*s' is reserved in strict mode", (int) nlen, name);
      }
      
      jsval_t v = js_mkundef();
      js->consumed = 1;
      if (next(js) == TOK_ASSIGN) {
        js->consumed = 1;
        v = js_expr(js);
        if (is_err(v)) return v;
      } else if (is_const) return js_mkerr_typed(js, JS_ERR_SYNTAX, "Missing initializer in const declaration");
      if (exe) {
        char decoded_name[256];
        size_t decoded_len = decode_ident_escapes(name, nlen, decoded_name, sizeof(decoded_name));
        
        jsval_t resolved = resolveprop(js, v);
        if (vtype(resolved) == T_FUNC) infer_func_name(js, resolved, decoded_name, decoded_len);
        
        if (lkp_scope(js, js->scope, decoded_name, decoded_len) > 0) return js_mkerr(js, "'%.*s' already declared", (int) decoded_len, decoded_name);
        jsval_t x = mkprop(js, js->scope, js_mkstr(js, decoded_name, decoded_len), resolved, is_const ? CONSTMASK : 0);
        
        if (is_err(x)) return x;
      }
    }
    
    uint8_t decl_next = next(js);
    bool asi = js->had_newline || decl_next == TOK_EOF || decl_next == TOK_RBRACE;
    if (decl_next == TOK_SEMICOLON || asi) break;
    EXPECT(TOK_COMMA);
  }
  return js_mkundef();
}

static jsval_t js_expr(struct js *js) {
  return js_assignment(js);
}

static jsval_t js_expr_comma(struct js *js) {
  jsval_t res = js_assignment(js);
  if (is_err(res)) return res;
  while (next(js) == TOK_COMMA) {
    js->consumed = 1;
    res = js_assignment(js);
    if (is_err(res)) return res;
  }
  return res;
}

static jsval_t js_eval_slice(struct js *js, jsoff_t off, jsoff_t len) {
  js_parse_state_t saved;
  JS_SAVE_STATE(js, saved);
  
  js->code = saved.code + off;
  js->clen = len;
  js->pos = 0;
  js->consumed = 1;
  
  jsval_t result = js_expr(js);
  
  JS_RESTORE_STATE(js, saved);
  return result;
}

static jsval_t js_eval_str(struct js *js, const char *code, jsoff_t len) {
  js_parse_state_t saved;
  JS_SAVE_STATE(js, saved);
  
  js->code = code;
  js->clen = len;
  js->pos = 0;
  js->consumed = 1;
  
  jsval_t result = js_expr(js);
  
  JS_RESTORE_STATE(js, saved);
  return result;
}

static jsval_t js_let(struct js *js) {
  return js_decl(js, false);
}

static jsval_t js_const(struct js *js) {
  return js_decl(js, true);
}

static jsval_t js_func_decl(struct js *js) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  uint8_t saved_flags = js->flags;
  js->consumed = 1;
  EXPECT_IDENT();
  js->consumed = 0;
  jsoff_t noff = js->toff, nlen = js->tlen;
  char *name = (char *) &js->code[noff];
  js->consumed = 1;
  EXPECT(TOK_LPAREN);
  jsoff_t pos = js->pos - 1;
  int param_count = 0;
  if (!parse_func_params(js, &saved_flags, &param_count)) {
    js->flags = saved_flags;
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "invalid parameters");
  }
  EXPECT(TOK_RPAREN);
  EXPECT(TOK_LBRACE);
  js->consumed = 0;
  uint8_t flags = js->flags;
  js->flags |= F_NOEXEC;
  jsval_t res = js_block(js, false);
  if (is_err(res)) {
    js->flags = flags;
    return res;
  }
  js->flags = flags;
   jsval_t func_obj = mkobj(js, 0);
   if (is_err(func_obj)) return func_obj;
   set_func_code(js, func_obj, &js->code[pos], js->pos - pos);
   
   jsval_t func_proto = get_slot(js, js_glob(js), SLOT_FUNC_PROTO);
   if (vtype(func_proto) == T_FUNC) set_proto(js, func_obj, func_proto);
   
   jsval_t len_key = js_mkstr(js, "length", 6);
   if (is_err(len_key)) return len_key;
   jsval_t res_len = setprop(js, func_obj, len_key, tov(param_count));
   if (is_err(res_len)) return res_len;
   js_set_descriptor(js, func_obj, "length", 6, JS_DESC_C);
   jsval_t name_key = js_mkstr(js, "name", 4);
   if (is_err(name_key)) return name_key;
   jsval_t name_val = js_mkstr(js, name, nlen);
   if (is_err(name_val)) return name_val;
   set_slot(js, func_obj, SLOT_NAME, name_val);
   jsval_t res3 = setprop(js, func_obj, name_key, name_val);
   if (is_err(res3)) return res3;
   if (exe) {
     jsval_t closure_scope = for_let_capture_scope(js);
     if (is_err(closure_scope)) return closure_scope;
     set_slot(js, func_obj, SLOT_SCOPE, closure_scope);
     if (flags & F_STRICT) {
       set_slot(js, func_obj, SLOT_STRICT, js_true);
     }
   }
   
   jsval_t func = mkval(T_FUNC, (unsigned long) vdata(func_obj));
   jsval_t proto_setup = setup_func_prototype(js, func);
   if (is_err(proto_setup)) return proto_setup;
  
   if (exe) {
     jsoff_t existing = lkp_scope(js, js->scope, name, nlen);
     if (existing > 0) {
       saveval(js, existing + sizeof(jsoff_t) * 2, func);
     } else {
       jsval_t x = mkprop(js, js->scope, js_mkstr(js, name, nlen), func, 0);
       if (is_err(x)) return x;
     }
   }
  
  return js_mkundef();
}

static jsval_t js_func_decl_async(struct js *js) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  js->consumed = 1;
  EXPECT_IDENT();
  js->consumed = 0;
  jsoff_t noff = js->toff, nlen = js->tlen;
  char *name = (char *) &js->code[noff];
  js->consumed = 1;
  EXPECT(TOK_LPAREN);
  jsoff_t pos = js->pos - 1;
  if (!parse_func_params(js, NULL, NULL)) {
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "invalid parameters");
  }
  EXPECT(TOK_RPAREN);
  EXPECT(TOK_LBRACE);
  js->consumed = 0;
  uint8_t flags = js->flags;
  js->flags |= F_NOEXEC;
  jsval_t res = js_block(js, false);
  if (is_err(res)) {
    js->flags = flags;
    return res;
  }
  js->flags = flags;
  jsval_t func_obj = mkobj(js, 0);
  if (is_err(func_obj)) return func_obj;
   set_func_code(js, func_obj, &js->code[pos], js->pos - pos);
   set_slot(js, func_obj, SLOT_ASYNC, js_true);
   jsval_t async_proto = get_slot(js, js_glob(js), SLOT_ASYNC_PROTO);
   if (vtype(async_proto) == T_FUNC) set_proto(js, func_obj, async_proto);
   jsval_t len_key = js_mkstr(js, "length", 6);
   if (is_err(len_key)) return len_key;
   jsval_t res_len = setprop(js, func_obj, len_key, tov(0));
   if (is_err(res_len)) return res_len;
   js_set_descriptor(js, func_obj, "length", 6, JS_DESC_C);
   jsval_t name_key = js_mkstr(js, "name", 4);
   if (is_err(name_key)) return name_key;
   jsval_t name_val = js_mkstr(js, name, nlen);
   if (is_err(name_val)) return name_val;
   set_slot(js, func_obj, SLOT_NAME, name_val);
   jsval_t res3 = setprop(js, func_obj, name_key, name_val);
   if (is_err(res3)) return res3;
  if (exe) {
    jsval_t closure_scope = for_let_capture_scope(js);
    if (is_err(closure_scope)) return closure_scope;
    set_slot(js, func_obj, SLOT_SCOPE, closure_scope);
    if (flags & F_STRICT) set_slot(js, func_obj, SLOT_STRICT, js_true);
  }
  jsval_t func = mkval(T_FUNC, (unsigned long) vdata(func_obj));
  
  jsval_t proto_setup = setup_func_prototype(js, func);
  if (is_err(proto_setup)) return proto_setup;
  
  if (exe) {
    jsoff_t existing = lkp_scope(js, js->scope, name, nlen);
    if (existing > 0) {
      saveval(js, existing + sizeof(jsoff_t) * 2, func);
    } else {
      jsval_t x = mkprop(js, js->scope, js_mkstr(js, name, nlen), func, 0);
      if (is_err(x)) return x;
    }
  }
  
  return js_mkundef();
}

static jsval_t js_block_or_stmt(struct js *js) {
  if (next(js) == TOK_LBRACE) return js_block(js, !(js->flags & F_NOEXEC));
  uint8_t stmt_tok = js->tok;
  jsval_t res = resolveprop(js, js_stmt(js));
  bool is_block_stmt = (
    stmt_tok == TOK_FUNC || stmt_tok == TOK_CLASS || 
    stmt_tok == TOK_IF || stmt_tok == TOK_WHILE || 
    stmt_tok == TOK_DO || stmt_tok == TOK_FOR || 
    stmt_tok == TOK_SWITCH || stmt_tok == TOK_TRY ||
    stmt_tok == TOK_LBRACE || stmt_tok == TOK_ASYNC
  );
  if (!is_block_stmt) js->consumed = 0;
  return res;
}

typedef struct {
  bool is_block;
  bool needs_scope;
  jsval_t loop_scope;
} loop_block_ctx_t;

static void loop_block_init(struct js *js, loop_block_ctx_t *ctx) {
  ctx->is_block = (lookahead(js) == TOK_LBRACE);
  ctx->needs_scope = false;
  ctx->loop_scope = js_mkundef();
  
  if (ctx->is_block && !(js->flags & F_NOEXEC)) {
    jsoff_t saved_pos = js->pos;
    uint8_t saved_tok = js->tok;
    uint8_t saved_consumed = js->consumed;
    
    js->consumed = 1;
    next(js);
    ctx->needs_scope = block_needs_scope(js);
    
    js->pos = saved_pos;
    js->tok = saved_tok;
    js->consumed = saved_consumed;
    
    if (ctx->needs_scope) ctx->loop_scope = js_mkscope(js);
  }
}

static inline jsval_t loop_block_exec(struct js *js, loop_block_ctx_t *ctx) {
  if (ctx->is_block) {
    next(js);
    return js_block(js, false);
  }
  
  return js_block_or_stmt(js);
}

static inline void loop_block_sync_scope(struct js *js, loop_block_ctx_t *ctx) {
  struct for_let_ctx *flc = for_let_current(js);
  if (flc && vtype(flc->body_scope) == T_OBJ) ctx->loop_scope = flc->body_scope;
}

#define loop_block_clear(js, ctx) if ((ctx)->needs_scope) scope_clear_props(js, (ctx)->loop_scope)
#define loop_block_cleanup(js, ctx) if ((ctx)->needs_scope) delscope(js)

static jsval_t js_if(struct js *js) {
  js->consumed = 1;
  EXPECT(TOK_LPAREN);
  
  jsval_t res = js_mkundef(), cond_expr = js_expr(js);
  if (is_err(cond_expr)) return cond_expr;
  jsval_t cond = resolveprop(js, cond_expr);
  if (is_err(cond)) return cond;
  
  EXPECT(TOK_RPAREN);
  
  bool cond_true = js_truthy(js, cond), exe = !(js->flags & F_NOEXEC);
  if (!cond_true) js->flags |= F_NOEXEC;
  jsval_t blk = js_block_or_stmt(js);
  if (cond_true) res = blk;
  if (exe && !cond_true) js->flags &= (uint8_t) ~F_NOEXEC;
  
  if (lookahead(js) == TOK_ELSE) {
    js->consumed = 1;
    next(js);
    js->consumed = 1;
    if (cond_true) js->flags |= F_NOEXEC;
    blk = js_block_or_stmt(js);
    if (!cond_true) res = blk;
    if (cond_true && exe) js->flags &= (uint8_t) ~F_NOEXEC;
  }
  
  return res;
}

static inline bool expect(struct js *js, uint8_t tok, jsval_t *res) {
  if (next(js) != tok) {
    *res = js_mkerr_typed(js, JS_ERR_SYNTAX, "parse error");
    return false;
  } else { js->consumed = 1; return true; }
}

static inline bool is_err2(jsval_t *v, jsval_t *res) {
  bool r = is_err(*v);
  if (r) { *res = *v; } return r;
}

typedef struct {
  jsoff_t body_start;
  jsoff_t body_end;
  jsoff_t var_name_off;
  jsoff_t var_name_len;
  bool is_const_var;
  uint8_t flags;
  bool has_destructure;
  jsoff_t destructure_off;
  jsoff_t destructure_len;
  int marker_index;
  loop_block_ctx_t loop_ctx;
} for_iter_ctx_t;

static jsval_t for_iter_bind_var(struct js *js, for_iter_ctx_t *ctx, jsval_t value) {
  loop_block_clear(js, &ctx->loop_ctx);
  if (ctx->has_destructure) {
    return bind_destruct_pattern(js, &js->code[ctx->destructure_off], ctx->destructure_len, value, js->scope);
  }
  const char *var_name = &js->code[ctx->var_name_off];
  jsoff_t existing = lkp_scope(js, js->scope, var_name, ctx->var_name_len);
  if (existing > 0) {
    saveval(js, existing + sizeof(jsoff_t) * 2, value);
    return js_mkundef();
  }
  return mkprop(js, js->scope, js_mkstr(js, var_name, ctx->var_name_len), value, ctx->is_const_var ? CONSTMASK : 0);
}

static jsval_t for_iter_exec_body(struct js *js, for_iter_ctx_t *ctx) {
  js->pos = ctx->body_start;
  js->consumed = 1;
  js->flags = (ctx->flags & ~F_NOEXEC) | F_LOOP;
  return loop_block_exec(js, &ctx->loop_ctx);
}

static inline bool for_iter_handle_continue(struct js *js, for_iter_ctx_t *ctx) {
  if (!(label_flags & F_CONTINUE_LABEL)) return false;
  if (is_this_loop_continue_target(ctx->marker_index)) {
    clear_continue_label();
    js->flags &= ~(F_BREAK | F_NOEXEC);
    return false;
  }
  js->flags |= F_BREAK;
  return true;
}

static int for_iter_step(struct js *js, for_iter_ctx_t *ctx, jsval_t key_str, jsval_t *out) {
  jsval_t err = for_iter_bind_var(js, ctx, key_str);
  if (is_err(err)) { *out = err; return 2; }
  
  jsval_t v = for_iter_exec_body(js, ctx);
  if (is_err(v)) { *out = v; return 2; }
  if (for_iter_handle_continue(js, ctx)) return 1;
  if (js->flags & F_BREAK) return 1;
  if (js->flags & F_RETURN) { *out = v; return 2; }
  
  return 0;
}

static jsval_t for_iter_string_indices(struct js *js, for_iter_ctx_t *ctx, jsval_t str) {
  jsoff_t slen = vstrlen(js, str);
  for (jsoff_t i = 0; i < slen; i++) {
    char idx[16];
    snprintf(idx, sizeof(idx), "%u", (unsigned)i);
    jsval_t key_str = js_mkstr(js, idx, strlen(idx));
    
    jsval_t out;
    int rc = for_iter_step(js, ctx, key_str, &out);
    if (rc) return (rc == 2) ? out : js_mkundef();
  }
  return js_mkundef();
}

static jsval_t for_in_iter_object(struct js *js, for_iter_ctx_t *ctx, jsval_t obj) {
  uint8_t obj_type = vtype(obj);
  if (obj_type == T_NULL || obj_type == T_UNDEF) return js_mkundef();
  if (obj_type == T_STR) return for_iter_string_indices(js, ctx, obj);
  if (obj_type != T_OBJ && obj_type != T_ARR && obj_type != T_FUNC)
    return js_mkerr(js, "for-in requires object");
  
  jsval_t iter_obj = (obj_type == T_FUNC) ? mkval(T_OBJ, vdata(obj)) : obj;
  jsoff_t iter_obj_off = (jsoff_t)vdata(iter_obj);
  jsoff_t prop_off = loadoff(js, iter_obj_off) & ~(3U | FLAGMASK);
  
  jsval_t prim = get_slot(js, obj, SLOT_PRIMITIVE);
  if (vtype(prim) == T_STR) return for_iter_string_indices(js, ctx, prim);
  
  const char *tag_sym_key = get_toStringTag_sym_key();
  size_t tag_sym_len = tag_sym_key ? strlen(tag_sym_key) : 0;
  
  while (prop_off < js->brk && prop_off != 0) {
    jsoff_t header = loadoff(js, prop_off);
    if (is_slot_prop(header)) { prop_off = next_prop(header); continue; }
    
    jsoff_t koff = loadoff(js, prop_off + (jsoff_t)sizeof(prop_off));
    jsoff_t klen = offtolen(loadoff(js, koff));
    const char *key = (char *)&js->mem[koff + sizeof(koff)];
    
    bool skip = streq(key, klen, STR_PROTO, STR_PROTO_LEN);
    if (!skip && tag_sym_key) skip = streq(key, klen, tag_sym_key, tag_sym_len);
    
    if (!skip) {
      descriptor_entry_t *desc = lookup_descriptor(iter_obj_off, key, klen);
      if (desc && !desc->enumerable) skip = true;
    }
    
    if (!skip) {
      jsval_t out;
      int rc = for_iter_step(js, ctx, js_mkstr(js, key, klen), &out);
      if (rc) return (rc == 2) ? out : js_mkundef();
    }
    
    prop_off = next_prop(header);
  }
  
  return js_mkundef();
}

static jsval_t for_of_iter_array(struct js *js, for_iter_ctx_t *ctx, jsval_t iterable) {
  jshdl_t h_iterable = js_root(js, iterable);
  
  jsoff_t length = 0; {
    jsval_t arr = js_deref(js, h_iterable);
    jsoff_t next_prop_off = loadoff(js, (jsoff_t) vdata(arr)) & ~(3U | FLAGMASK);
    jsoff_t scan = next_prop_off;
    while (scan < js->brk && scan != 0) {
      jsoff_t header = loadoff(js, scan);
      if (is_slot_prop(header)) { scan = next_prop(header); continue; }
      const char *key; jsoff_t klen;
      get_prop_key(js, scan, &key, &klen);
      if (streq(key, klen, "length", 6)) {
        jsval_t val = get_prop_val(js, scan);
        if (vtype(val) == T_NUM) length = (jsoff_t) tod(val);
        break;
      }
      scan = next_prop(header);
    }
  }
  
  for (jsoff_t i = 0; i < length; i++) {
    jsval_t arr = js_deref(js, h_iterable);
    jsoff_t next_prop_off = loadoff(js, (jsoff_t) vdata(arr)) & ~(3U | FLAGMASK);
    
    char idx[16];
    snprintf(idx, sizeof(idx), "%u", (unsigned) i);
    jsoff_t idxlen = (jsoff_t) strlen(idx);
    jsoff_t prop = next_prop_off;
    jsval_t val = js_mkundef();
    
    while (prop < js->brk && prop != 0) {
      jsoff_t header = loadoff(js, prop);
      if (is_slot_prop(header)) { prop = next_prop(header); continue; }
      const char *key; jsoff_t klen;
      get_prop_key(js, prop, &key, &klen);
      if (streq(key, klen, idx, idxlen)) { val = get_prop_val(js, prop); break; }
      prop = next_prop(header);
    }
    
    jsval_t err = for_iter_bind_var(js, ctx, val);
    if (is_err(err)) { js_unroot(js, h_iterable); return err; }
    
    jsval_t v = for_iter_exec_body(js, ctx);
    if (is_err(v)) { js_unroot(js, h_iterable); return v; }
    if (for_iter_handle_continue(js, ctx)) break;
    if (js->flags & F_BREAK) break;
    if (js->flags & F_RETURN) { js_unroot(js, h_iterable); return v; }
  }
  
  js_unroot(js, h_iterable);
  return js_mkundef();
}

static void init_ascii_cache(struct js *js) {
  if (js->ascii_cache_init) return;
  for (int i = 0; i < 128; i++) {
    char c = (char)i;
    js->ascii_char_cache[i] = js_mkstr(js, &c, 1);
  }
  js->ascii_cache_init = true;
}

static jsval_t for_of_iter_string(struct js *js, for_iter_ctx_t *ctx, jsval_t iterable) {
  jshdl_t h_iterable = js_root(js, iterable);
  size_t byte_pos = 0;
  
  if (!js->ascii_cache_init) init_ascii_cache(js);
  
  for (;;) {
    jsval_t cur = js_deref(js, h_iterable);
    jsoff_t cur_byte_len;
    jsoff_t cur_soff = vstr(js, cur, &cur_byte_len);
    
    if (byte_pos >= cur_byte_len) break;
    
    const char *cur_str = (char *) &js->mem[cur_soff];
    unsigned char c = (unsigned char)cur_str[byte_pos];
    size_t char_bytes;
    jsval_t char_str;
    
    if (c < 0x80) { char_bytes = 1; char_str = js->ascii_char_cache[c]; } else {
      if ((c & 0xE0) == 0xC0) char_bytes = 2;
      else if ((c & 0xF0) == 0xE0) char_bytes = 3;
      else if ((c & 0xF8) == 0xF0) char_bytes = 4;
      else char_bytes = 1;
      if (byte_pos + char_bytes > cur_byte_len) char_bytes = cur_byte_len - byte_pos;
      char_str = js_mkstr(js, cur_str + byte_pos, char_bytes);
    } byte_pos += char_bytes;
    
    jsval_t err = for_iter_bind_var(js, ctx, char_str);
    if (is_err(err)) { js_unroot(js, h_iterable); return err; }
    
    jsval_t v = for_iter_exec_body(js, ctx);
    if (is_err(v)) { js_unroot(js, h_iterable); return v; }
    if (for_iter_handle_continue(js, ctx)) break;
    if (js->flags & F_BREAK) break;
    if (js->flags & F_RETURN) { js_unroot(js, h_iterable); return v; }
  }
  
  js_unroot(js, h_iterable);
  return js_mkundef();
}

typedef enum { ITER_CONTINUE, ITER_BREAK, ITER_ERROR } iter_action_t;
typedef iter_action_t (*iter_callback_t)(struct js *js, jsval_t value, void *ctx, jsval_t *out);

static jsval_t iter_foreach(struct js *js, jsval_t iterable, iter_callback_t cb, void *ctx) {
  const char *iter_key = get_iterator_sym_key();
  jsoff_t iter_prop = iter_key ? lkp_proto(js, iterable, iter_key, strlen(iter_key)) : 0;
  if (iter_prop == 0) return js_mkerr(js, "not iterable");
  
  js_parse_state_t saved_state;
  JS_SAVE_STATE(js, saved_state);
  uint8_t saved_flags = js->flags;
  
  jsval_t iter_method = loadval(js, iter_prop + sizeof(jsoff_t) * 2);
  push_this(iterable);
  jsval_t iterator = call_js_with_args(js, iter_method, NULL, 0);
  pop_this();
  JS_RESTORE_STATE(js, saved_state);
  js->flags = saved_flags;
  
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
    push_this(cur_iter);
    jsval_t result = call_js_with_args(js, next_method, NULL, 0);
    pop_this();
    JS_RESTORE_STATE(js, saved_state);
    js->flags = saved_flags;
    
    if (is_err(result)) { js_unroot(js, h_iterator); return result; }
    
    jsoff_t done_off = lkp(js, result, "done", 4);
    jsval_t done_val = done_off ? loadval(js, done_off + sizeof(jsoff_t) * 2) : js_mkundef();
    if (js_truthy(js, done_val)) break;
    
    jsoff_t value_off = lkp(js, result, "value", 5);
    jsval_t value = value_off ? loadval(js, value_off + sizeof(jsoff_t) * 2) : js_mkundef();
    
    iter_action_t action = cb(js, value, ctx, &out);
    if (action == ITER_BREAK) break;
    if (action == ITER_ERROR) { js_unroot(js, h_iterator); return out; }
  }
  
  js_unroot(js, h_iterator);
  return out;
}

static iter_action_t for_of_iter_cb(struct js *js, jsval_t value, void *ctx, jsval_t *out) {
  for_iter_ctx_t *fctx = (for_iter_ctx_t *)ctx;
  
  jsval_t err = for_iter_bind_var(js, fctx, value);
  if (is_err(err)) { *out = err; return ITER_ERROR; }
  
  jsval_t v = for_iter_exec_body(js, fctx);
  if (is_err(v)) { *out = v; return ITER_ERROR; }
  if (for_iter_handle_continue(js, fctx)) return ITER_BREAK;
  if (js->flags & F_BREAK) return ITER_BREAK;
  if (js->flags & F_RETURN) { *out = v; return ITER_BREAK; }
  
  return ITER_CONTINUE;
}

static jsval_t for_of_iter_object(struct js *js, for_iter_ctx_t *ctx, jsval_t iterable) {
  jsval_t result = iter_foreach(js, iterable, for_of_iter_cb, ctx);
  if (is_err(result) && strcmp(js->errmsg, "not iterable") == 0) {
    return js_mkerr(js, "for-of requires iterable");
  }
  return result;
}

static jsval_t js_for(struct js *js) {
  uint8_t flags = js->flags, exe = !(flags & F_NOEXEC);
  jsval_t v, res = js_mkundef();
  jsoff_t pos1 = 0, pos2 = 0, pos3 = 0, pos4 = 0;
  
  bool use_label_stack = label_stack && utarray_len(label_stack) > 0;
  int marker_index = 0;
  if (use_label_stack) {
    label_entry_t marker = { .name = NULL, .name_len = 0, .is_loop = true, .is_block = false };
    utarray_push_back(label_stack, &marker);
    marker_index = utarray_len(label_stack) - 1;
  }
  
  if (exe) mkscope(js);
  if (!expect(js, TOK_FOR, &res)) goto done;
  if (!expect(js, TOK_LPAREN, &res)) goto done;
  
  bool is_for_in = false;
  bool is_for_of = false;
  
  bool is_var_decl = false;
  bool is_const_var = false;
  bool is_let_loop = false;
  
  jsoff_t var_name_off = 0;
  jsoff_t var_name_len = 0;
  jsoff_t let_var_off = 0;
  jsoff_t let_var_len = 0;
  
  bool has_destructure = false;
  jsoff_t destructure_off = 0;
  jsoff_t destructure_len = 0;
  
  if (next(js) == TOK_LET || next(js) == TOK_CONST || next(js) == TOK_VAR) {
    if (js->tok == TOK_VAR) {
      is_var_decl = true;
      if ((js->flags & F_STRICT) && !js->var_warning_shown) {
        fprintf(stderr, "Warning: 'var' is deprecated, use 'let' or 'const' instead\n");
        js->var_warning_shown = true;
      }
    } else if (js->tok == TOK_LET) {
      is_let_loop = true;
    }
    is_const_var = (js->tok == TOK_CONST);
    js->consumed = 1;
    
    if (next(js) == TOK_LBRACKET || next(js) == TOK_LBRACE) {
      has_destructure = true;
      destructure_off = js->toff;
      uint8_t open_tok = js->tok;
      uint8_t close_tok = (open_tok == TOK_LBRACKET) ? TOK_RBRACKET : TOK_RBRACE;
      int depth = 1;
      js->consumed = 1;
      while (depth > 0 && next(js) != TOK_EOF) {
        if (js->tok == open_tok) depth++;
        else if (js->tok == close_tok) depth--;
        js->consumed = 1;
      }
      destructure_len = js->pos - destructure_off;
      if (next(js) == TOK_IN) {
        is_for_in = true;
        js->consumed = 1;
      } else if (next(js) == TOK_OF) {
        is_for_of = true;
        js->consumed = 1;
      } else {
        res = js_mkerr_typed(js, JS_ERR_SYNTAX, "expected 'in' or 'of' after destructuring pattern");
        goto done;
      }
    } else if (next(js) == TOK_IDENTIFIER) {
      var_name_off = js->toff;
      var_name_len = js->tlen;
      if (is_let_loop) {
        let_var_off = var_name_off;
        let_var_len = var_name_len;
      }
      js->consumed = 1;
      if (next(js) == TOK_IN) {
        is_for_in = true;
        js->consumed = 1;
      } else if (next(js) == TOK_OF) {
        is_for_of = true;
        js->consumed = 1;
      } else {
        js->pos = var_name_off;
        js->consumed = 1;
        if (is_const_var) {
          v = js_const(js);
        } else if (is_var_decl) {
          v = js_var_decl(js);
        } else {
          v = js_let(js);
        }
        if (is_err2(&v, &res)) goto done;
      }
    }
  } else if (next(js) == TOK_IDENTIFIER) {
    var_name_off = js->toff;
    var_name_len = js->tlen;
    js->consumed = 1;
    if (next(js) == TOK_IN) {
      is_for_in = true;
      js->consumed = 1;
    } else if (next(js) == TOK_OF) {
      is_for_of = true;
      js->consumed = 1;
    } else {
      js->pos = var_name_off;
      js->consumed = 1;
      v = js_expr_comma(js);
      if (is_err2(&v, &res)) goto done;
    }
  } else if (next(js) == TOK_SEMICOLON) {
  } else {
    v = js_expr_comma(js);
    if (is_err2(&v, &res)) goto done;
  }
  
  if (is_for_in) {
    jsval_t obj_expr = js_expr(js);
    if (is_err2(&obj_expr, &res)) goto done;
    if (!expect(js, TOK_RPAREN, &res)) goto done;
    
    jsoff_t body_start = js->pos;
    loop_block_ctx_t forin_loop_ctx = {0};
    if (exe) loop_block_init(js, &forin_loop_ctx);
    js->flags |= F_NOEXEC;
    v = js_block_or_stmt(js);
    if (is_err2(&v, &res)) goto done;
    jsoff_t body_end = js->pos;
    
    if (exe) {
      jsval_t obj = resolveprop(js, obj_expr);
      for_iter_ctx_t ctx = { 
        body_start, body_end,
        var_name_off, var_name_len,
        is_const_var, flags,
        has_destructure, destructure_off,
        destructure_len, marker_index,
        forin_loop_ctx
      };
      res = for_in_iter_object(js, &ctx, obj);
      loop_block_cleanup(js, &forin_loop_ctx);
      if (is_err(res)) goto done;
      if (js->flags & F_RETURN) goto done;
    }
    
    js->pos = body_end;
    js->tok = TOK_SEMICOLON;
    js->consumed = 0;
    goto done;
  }
  
  if (is_for_of) {
    jsval_t iter_expr = js_expr(js);
    if (is_err2(&iter_expr, &res)) goto done;
    if (!expect(js, TOK_RPAREN, &res)) goto done;
    
    jsoff_t body_start = js->pos;
    loop_block_ctx_t forof_loop_ctx = {0};
    if (exe) loop_block_init(js, &forof_loop_ctx);
    js->flags |= F_NOEXEC;
    v = js_block_or_stmt(js);
    if (is_err2(&v, &res)) goto done;
    jsoff_t body_end = js->pos;
    
    if (exe) {
      jsval_t iterable = resolveprop(js, iter_expr);
      uint8_t itype = vtype(iterable);
      for_iter_ctx_t ctx = { 
        body_start, body_end,
        var_name_off, var_name_len,
        is_const_var, flags,
        has_destructure, destructure_off,
        destructure_len, marker_index,
        forof_loop_ctx
      };
      
      if (itype == T_ARR) res = for_of_iter_array(js, &ctx, iterable);
      else if (itype == T_STR) res = for_of_iter_string(js, &ctx, iterable);
      else if (itype == T_OBJ) res = for_of_iter_object(js, &ctx, iterable);
      else res = js_mkerr(js, "for-of requires iterable");
      
      loop_block_cleanup(js, &forof_loop_ctx);
      if (is_err(res)) goto done;
      if (js->flags & F_RETURN) goto done;
    }
    
    js->pos = body_end;
    js->tok = TOK_SEMICOLON;
    js->consumed = 0;
    goto done;
  }
  
  if (!expect(js, TOK_SEMICOLON, &res)) goto done;
  js->flags |= F_NOEXEC;
  pos1 = js->pos;
  if (next(js) != TOK_SEMICOLON) {
    v = js_expr(js);
    if (is_err2(&v, &res)) goto done;
  }
  if (!expect(js, TOK_SEMICOLON, &res)) goto done;
  pos2 = js->pos;
  if (next(js) != TOK_RPAREN) {
    v = js_expr_comma(js);
    if (is_err2(&v, &res)) goto done;
  }
  if (!expect(js, TOK_RPAREN, &res)) goto done;
  pos3 = js->pos;
  
  jsoff_t iter_var_prop_off = 0;
  if (is_let_loop && let_var_len > 0 && exe) {
    js->flags = flags;
    mkscope(js);
    jsval_t let_var_key = js_mkstr(js, &js->code[let_var_off], let_var_len);
    jsoff_t outer_off = lkp_scope(js, upper(js, js->scope), &js->code[let_var_off], let_var_len);
    jsval_t init_val = outer_off ? resolveprop(js, mkval(T_PROP, outer_off)) : js_mkundef();
    mkprop(js, js->scope, let_var_key, init_val, 0);
    iter_var_prop_off = lkp(js, js->scope, &js->code[let_var_off], let_var_len);
    const char *var_interned = intern_string(&js->code[let_var_off], let_var_len);
    for_let_push(js, var_interned, let_var_len, iter_var_prop_off, js_mkundef());
  }
  
  loop_block_ctx_t loop_ctx = {0};
  if (exe) {
    loop_block_init(js, &loop_ctx);
    if (is_let_loop && let_var_len > 0 && loop_ctx.needs_scope) for_let_set_body_scope(js, loop_ctx.loop_scope);
  }
  
  js->flags |= F_NOEXEC;
  v = js_block_or_stmt(js);
  if (exe) js->flags = flags;
  if (is_err2(&v, &res)) goto done;
  pos4 = js->pos;
  
  while (!(flags & F_NOEXEC)) {
    js->flags = flags, js->pos = pos1, js->consumed = 1;
    if (next(js) != TOK_SEMICOLON) {
      v = resolveprop(js, js_expr(js));
      if (is_err2(&v, &res)) goto done;
      if (!js_truthy(js, v)) break;
    }
    
    js->flags |= F_LOOP;
    js->pos = pos3;
    js->consumed = 1;
    
    if (is_let_loop && let_var_len > 0 && loop_ctx.needs_scope) {
      loop_block_sync_scope(js, &loop_ctx);
    }
    
    loop_block_clear(js, &loop_ctx);
    v = loop_block_exec(js, &loop_ctx);
    if (is_err2(&v, &res)) {
      loop_block_cleanup(js, &loop_ctx);
      if (is_let_loop && let_var_len > 0) {
        for_let_pop(js); delscope(js);
      }
      goto done;
    }
    
    if (label_flags & F_CONTINUE_LABEL) {
      if (is_this_loop_continue_target(marker_index)) {
        clear_continue_label();
        js->flags &= ~(F_BREAK | F_NOEXEC);
        js->flags = flags;
        js->pos = pos2, js->consumed = 1;
        if (next(js) != TOK_RPAREN) {
          v = js_expr_comma(js);
          if (is_err2(&v, &res)) goto done;
        } continue;
      }
    }
    
    if (js->flags & F_BREAK) break;
    if (js->flags & F_RETURN) { res = v; break; }
    
    js->flags = flags, js->pos = pos2, js->consumed = 1;
    if (next(js) != TOK_RPAREN) {
      v = js_expr_comma(js);
      if (is_err2(&v, &res)) goto done;
    }
  }
  if (exe) loop_block_cleanup(js, &loop_ctx);
  if (is_let_loop && let_var_len > 0 && exe) {
    for_let_pop(js); delscope(js);
  }
  js->pos = pos4, js->tok = TOK_SEMICOLON, js->consumed = 0;
done:
  if (use_label_stack && label_stack && utarray_len(label_stack) > 0) {
    utarray_pop_back(label_stack);
  }
  
  if (exe) delscope(js);
  uint8_t preserve = 0;
  if (js->flags & F_RETURN) {
    preserve = js->flags & (F_RETURN | F_NOEXEC);
  }
  if ((js->flags & F_BREAK) && (label_flags & F_BREAK_LABEL)) {
    preserve |= (js->flags & (F_BREAK | F_NOEXEC));
  }
  if (label_flags & F_CONTINUE_LABEL) {
    preserve |= F_BREAK | F_NOEXEC;
  }
  js->flags = flags | preserve;
  return res;
}

static jsval_t js_while(struct js *js) {
  uint8_t flags = js->flags, exe = !(flags & F_NOEXEC);
  jsval_t res = js_mkundef(), v;
  loop_block_ctx_t loop_ctx = {0};
  
  bool use_label_stack = label_stack && utarray_len(label_stack) > 0;
  int marker_index = 0;
  if (use_label_stack) {
    label_entry_t marker = { .name = NULL, .name_len = 0, .is_loop = true, .is_block = false };
    utarray_push_back(label_stack, &marker);
    marker_index = utarray_len(label_stack) - 1;
  }
  
  js->consumed = 1;
  if (!expect(js, TOK_LPAREN, &res)) goto done;
  
  jsoff_t cond_start = js->pos;
  js->flags |= F_NOEXEC;
  v = js_expr(js);
  if (is_err(v)) { res = v; goto done; }
  
  if (!expect(js, TOK_RPAREN, &res)) goto done;
  
  jsoff_t body_start = js->pos;
  if (exe) {
    js->flags = flags;
    loop_block_init(js, &loop_ctx);
    js->flags |= F_NOEXEC;
  }
  
  v = js_block_or_stmt(js);
  if (is_err(v)) { res = v; goto done; }
  jsoff_t body_end = js->pos;
  
  if (exe) {
    while (true) {
      js->flags = flags;
      js->pos = cond_start;
      js->consumed = 1;
      
      v = resolveprop(js, js_expr(js));
      if (is_err(v)) { res = v; break; }
      
      if (!js_truthy(js, v)) break;
      
      js->pos = body_start;
      js->consumed = 1;
      js->flags = (flags & ~F_NOEXEC) | F_LOOP;
      
      loop_block_clear(js, &loop_ctx);
      v = loop_block_exec(js, &loop_ctx);
      if (is_err(v)) { res = v; break; }
      
      if (label_flags & F_CONTINUE_LABEL) {
        if (is_this_loop_continue_target(marker_index)) {
          clear_continue_label();
          js->flags &= ~(F_BREAK | F_NOEXEC);
          continue;
        }
      }
      
      if (js->flags & F_BREAK) break;
      if (js->flags & F_RETURN) { res = v; break; }
    }
    loop_block_cleanup(js, &loop_ctx);
  }
  
  js->pos = body_end;
  js->tok = TOK_SEMICOLON;
  js->consumed = 0;
  
done:
  if (use_label_stack && label_stack && utarray_len(label_stack) > 0) {
    utarray_pop_back(label_stack);
  }
  
  uint8_t preserve = 0;
  if (js->flags & F_RETURN) {
    preserve = js->flags & (F_RETURN | F_NOEXEC);
  }
  if ((js->flags & F_BREAK) && (label_flags & F_BREAK_LABEL)) {
    preserve |= (js->flags & (F_BREAK | F_NOEXEC));
  }
  if (label_flags & F_CONTINUE_LABEL) {
    preserve |= F_BREAK | F_NOEXEC;
  }
  js->flags = flags | preserve;
  
  return res;
}

static jsval_t js_do_while(struct js *js) {
  uint8_t flags = js->flags, exe = !(flags & F_NOEXEC);
  jsval_t res = js_mkundef(), v;
  loop_block_ctx_t loop_ctx = {0};
  
  bool use_label_stack = label_stack && utarray_len(label_stack) > 0;
  int marker_index = 0;
  if (use_label_stack) {
    label_entry_t marker = { .name = NULL, .name_len = 0, .is_loop = true, .is_block = false };
    utarray_push_back(label_stack, &marker);
    marker_index = utarray_len(label_stack) - 1;
  }
  
  js->consumed = 1;
  
  jsoff_t body_start = js->pos;
  bool is_block = (next(js) == TOK_LBRACE);
  if (exe) loop_block_init(js, &loop_ctx);
  
  js->flags |= F_NOEXEC;
  v = js_block_or_stmt(js);
  if (is_err(v)) { res = v; goto done; }
  
  if (is_block && next(js) == TOK_RBRACE) {
    js->consumed = 1;
  }
  (void) js->pos;
  
  if (!expect(js, TOK_WHILE, &res)) goto done;
  if (!expect(js, TOK_LPAREN, &res)) goto done;
  
  jsoff_t cond_start = js->pos;
  v = js_expr(js);
  if (is_err(v)) { res = v; goto done; }
  
  if (!expect(js, TOK_RPAREN, &res)) goto done;
  jsoff_t cond_end = js->pos;
  
  if (exe) {
    do {
      js->pos = body_start;
      js->consumed = 1;
      js->flags = (flags & ~F_NOEXEC) | F_LOOP;
      
      loop_block_clear(js, &loop_ctx);
      v = loop_block_exec(js, &loop_ctx);
      if (is_err(v)) {
        res = v; break;
      }
      
      if (label_flags & F_CONTINUE_LABEL) {
        if (is_this_loop_continue_target(marker_index)) {
          clear_continue_label();
          js->flags &= ~(F_BREAK | F_NOEXEC);
        } else { break; }
      }
      
      if (js->flags & F_BREAK) {
        break;
      }
      
      if (js->flags & F_RETURN) {
        res = v;
        break;
      }
      
      js->flags = flags;
      js->pos = cond_start;
      js->consumed = 1;
      
      v = resolveprop(js, js_expr(js));
      if (is_err(v)) {
        res = v;
        break;
      }
    } while (js_truthy(js, v));
    loop_block_cleanup(js, &loop_ctx);
  }
  
  js->pos = cond_end;
  js->consumed = 1;
  
done:
  if (use_label_stack && label_stack && utarray_len(label_stack) > 0) {
    utarray_pop_back(label_stack);
  }
  
  uint8_t preserve = 0;
  if (js->flags & F_RETURN) {
    preserve = js->flags & (F_RETURN | F_NOEXEC);
  }
  if ((js->flags & F_BREAK) && (label_flags & F_BREAK_LABEL)) {
    preserve |= (js->flags & (F_BREAK | F_NOEXEC));
  }
  if (label_flags & F_CONTINUE_LABEL) {
    preserve |= F_BREAK | F_NOEXEC;
  }
  js->flags = flags | preserve;
  
  return res;
}

static jsval_t js_try(struct js *js) {
  uint8_t flags = js->flags, exe = !(flags & F_NOEXEC);
  jsval_t res = js_mkundef();
  jsval_t try_result = js_mkundef();
  jsval_t catch_result = js_mkundef();
  jsval_t finally_result = js_mkundef();
  
  bool had_exception = false;
  char saved_errmsg[256] = {0};
  jsval_t exception_value = js_mkundef();
  
  js->consumed = 1;
  
  if (next(js) != TOK_LBRACE) {
    return js_mkerr(js, "{ expected after try");
  }
  
  jsoff_t try_start = js->pos;
  js->flags |= F_NOEXEC;
  js->consumed = 1;
  
  while (next(js) != TOK_EOF && next(js) != TOK_RBRACE) {
    jsval_t v = js_stmt(js);
    if (is_err(v)) break;
  }
  if (next(js) == TOK_RBRACE) js->consumed = 1;
  jsoff_t try_end = js->pos;
  
  bool has_catch = false;
  bool has_finally = false;
  jsoff_t catch_start = 0, catch_end = 0;
  jsoff_t finally_start = 0, finally_end = 0;
  jsoff_t catch_param_off = 0, catch_param_len = 0;
  
  if (lookahead(js) == TOK_CATCH) {
    has_catch = true;
    js->consumed = 1;
    next(js);
    js->consumed = 1;
    
    if (next(js) == TOK_LPAREN) {
      js->consumed = 1;
      if (next(js) == TOK_IDENTIFIER) {
        catch_param_off = js->toff;
        catch_param_len = js->tlen;
        if ((js->flags & F_STRICT) && is_strict_restricted(&js->code[catch_param_off], catch_param_len)) {
          return js_mkerr_typed(js, JS_ERR_SYNTAX, "cannot use '%.*s' as catch parameter in strict mode", (int) catch_param_len, &js->code[catch_param_off]);
        }
        js->consumed = 1;
      }
      if (next(js) != TOK_RPAREN) return js_mkerr(js, ") expected in catch");
      js->consumed = 1;
    }
    
    if (next(js) != TOK_LBRACE) {
      return js_mkerr(js, "{ expected after catch");
    }
    
    catch_start = js->pos;
    js->consumed = 1;
    
    while (next(js) != TOK_EOF && next(js) != TOK_RBRACE) {
      jsval_t v = js_stmt(js);
      if (is_err(v)) break;
    }
    if (next(js) == TOK_RBRACE) js->consumed = 1;
    catch_end = js->pos;
  }
  
  if (lookahead(js) == TOK_FINALLY) {
    has_finally = true;
    js->consumed = 1;
    next(js);
    js->consumed = 1;
    
    if (next(js) != TOK_LBRACE) {
      return js_mkerr(js, "{ expected after finally");
    }
    
    finally_start = js->pos;
    js->consumed = 1;
    
    while (next(js) != TOK_EOF && next(js) != TOK_RBRACE) {
      jsval_t v = js_stmt(js);
      if (is_err(v)) break;
    }
    if (next(js) == TOK_RBRACE) js->consumed = 1;
    finally_end = js->pos;
  }
  
  if (!has_catch && !has_finally) {
    return js_mkerr(js, "try requires catch or finally");
  }
  
  jsoff_t end_pos = has_finally ? finally_end : (has_catch ? catch_end : try_end);
  
  if (exe) {
    bool try_returned = false;
    jsval_t try_return_value = js_mkundef();
    
    js->flags = flags & (uint8_t)~F_NOEXEC;
    js->pos = try_start;
    js->consumed = 1;
    
    mkscope(js);
    while (next(js) != TOK_EOF && next(js) != TOK_RBRACE && !(js->flags & (F_RETURN | F_THROW | F_BREAK))) {
      try_result = js_stmt(js);
      if (is_err(try_result)) { had_exception = true; break; }
    } delscope(js);
    
    if (js->flags & F_RETURN) {
      try_returned = true;
      try_return_value = try_result;
      js->flags &= (uint8_t)~(F_RETURN | F_NOEXEC);
    }
    
    if (js->flags & F_THROW) {
      had_exception = true;
      js->flags &= (uint8_t)~F_THROW;
      strncpy(saved_errmsg, js->errmsg, sizeof(saved_errmsg) - 1);
      saved_errmsg[sizeof(saved_errmsg) - 1] = '\0';
      
      exception_value = js->thrown_value;
      js->thrown_value = js_mkundef();
      js->errmsg[0] = '\0';
    }
    
    if (next(js) == TOK_RBRACE) js->consumed = 1;
    
    bool exception_handled = false;
    bool catch_returned = false;
    jsval_t catch_return_value = js_mkundef();
    
    if (had_exception && has_catch) {
      exception_handled = true;
      mkscope(js);
      
      if (catch_param_len > 0) {
        jsval_t key = js_mkstr(js, &js->code[catch_param_off], catch_param_len);
        mkprop(js, js->scope, key, exception_value, 0);
      }
      
      js->flags = flags & (uint8_t)~F_NOEXEC;
      js->pos = catch_start;
      js->consumed = 1;
      
      while (next(js) != TOK_EOF && next(js) != TOK_RBRACE && !(js->flags & (F_RETURN | F_THROW | F_BREAK))) {
        catch_result = js_stmt(js);
        if (is_err(catch_result)) break;
      }
      
      if (js->flags & F_RETURN) {
        catch_returned = true;
        catch_return_value = catch_result;
        js->flags &= (uint8_t)~(F_RETURN | F_NOEXEC);
      }
      
      if (next(js) == TOK_RBRACE) js->consumed = 1;
      delscope(js);
      
      if (js->flags & F_THROW) {
        exception_handled = false;
        strncpy(saved_errmsg, js->errmsg, sizeof(saved_errmsg) - 1);
        saved_errmsg[sizeof(saved_errmsg) - 1] = '\0';
        exception_value = js->thrown_value;
        js->thrown_value = js_mkundef();
        js->flags &= (uint8_t)~F_THROW;
        js->errmsg[0] = '\0';
      } else {
        res = catch_result;
      }
    }
    
    if (has_finally) {
      uint8_t pre_finally_flags = js->flags;
      bool had_pre_finally_exception = (js->flags & F_THROW) != 0;
      char pre_finally_errmsg[256] = {0};
      if (had_pre_finally_exception) {
        strncpy(pre_finally_errmsg, js->errmsg, sizeof(pre_finally_errmsg) - 1);
        js->flags &= (uint8_t)~F_THROW;
        js->errmsg[0] = '\0';
      }
      
      js->flags = flags & (uint8_t)~F_NOEXEC;
      js->pos = finally_start;
      js->consumed = 1;
      
      while (next(js) != TOK_EOF && next(js) != TOK_RBRACE && !(js->flags & (F_RETURN | F_THROW | F_BREAK))) {
        finally_result = js_stmt(js);
        if (is_err(finally_result)) break;
      }
      
      if (next(js) == TOK_RBRACE) js->consumed = 1;
      
      if (!(js->flags & (F_RETURN | F_THROW))) {
        if (had_pre_finally_exception) {
          js->flags = pre_finally_flags;
          strncpy(js->errmsg, pre_finally_errmsg, sizeof(js->errmsg) - 1);
        } else if (had_exception && !exception_handled) {
          js->flags |= F_THROW;
          strncpy(js->errmsg, saved_errmsg, sizeof(js->errmsg) - 1);
          js->thrown_value = exception_value;
        } else if (catch_returned) {
          js->flags |= F_RETURN;
          res = catch_return_value;
        } else if (try_returned) {
          js->flags |= F_RETURN;
          res = try_return_value;
        }
      }
    } else if (had_exception && !exception_handled) {
      js->flags |= F_THROW;
      strncpy(js->errmsg, saved_errmsg, sizeof(js->errmsg) - 1);
      js->thrown_value = exception_value;
      res = mkval(T_ERR, 0);
    } else if (catch_returned) {
      js->flags |= F_RETURN;
      res = catch_return_value;
    } else if (try_returned) {
      js->flags |= F_RETURN;
      res = try_return_value;
    }
    
    if (!had_exception && !try_returned && !(js->flags & (F_RETURN | F_THROW))) {
      res = try_result;
    }
  }
  
  js->pos = end_pos;
  js->tok = TOK_SEMICOLON;
  js->consumed = 0;
  
  return res;
}

static bool label_exists(const char *name, jsoff_t len, bool check_loop) {
  if (!label_stack) return false;
  unsigned int depth = utarray_len(label_stack);
  for (int i = (int)depth - 1; i >= 0; i--) {
    label_entry_t *entry = (label_entry_t *)utarray_eltptr(label_stack, (unsigned int)i);
    if (entry && entry->name_len == len && 
        memcmp(entry->name, name, len) == 0) {
      if (check_loop && !entry->is_loop) {
        return false;
      }
      return true;
    }
  }
  return false;
}

static bool is_this_loop_continue_target(int marker_index) {
  if (!(label_flags & F_CONTINUE_LABEL)) return false;
  if (!label_stack || !continue_target_label) return false;
  if (marker_index <= 0) return false;
  
  label_entry_t *entry = (label_entry_t *)utarray_eltptr(label_stack, (unsigned int)(marker_index - 1));
  if (!entry) return false;
  if (entry->name == NULL) return false;
  if (!entry->is_loop) return false;
  
  if (entry->name_len == continue_target_label_len &&
      memcmp(entry->name, continue_target_label, continue_target_label_len) == 0) {
    return true;
  }
  return false;
}

static jsval_t js_break(struct js *js) {
  js->consumed = 1;
  
  uint8_t nxt = next(js);
  if (nxt == TOK_IDENTIFIER && !js->had_newline) {
    const char *label = &js->code[js->toff];
    jsoff_t label_len = js->tlen;
    js->consumed = 1;
    
    if (js->flags & F_NOEXEC) {
      return js_mkundef();
    }
    
    if (!label_exists(label, label_len, false)) {
      return js_mkerr_typed(js, JS_ERR_SYNTAX, "undefined label '%.*s'", (int)label_len, label);
    }
    
    break_target_label = label;
    break_target_label_len = label_len;
    label_flags |= F_BREAK_LABEL;
    js->flags |= F_BREAK | F_NOEXEC;
    return js_mkundef();
  }
  
  if (js->flags & F_NOEXEC) {
    return js_mkundef();
  }
  
  if (!(js->flags & (F_LOOP | F_SWITCH))) {
    bool in_labeled_block = false;
    if (label_stack) {
      unsigned int depth = utarray_len(label_stack);
      for (int i = (int)depth - 1; i >= 0; i--) {
        label_entry_t *entry = (label_entry_t *)utarray_eltptr(label_stack, (unsigned int)i);
        if (entry && entry->is_block) {
          in_labeled_block = true;
          break;
        }
      }
    }
    if (!in_labeled_block) {
      return js_mkerr(js, "not in loop or switch");
    }
  }
  
  js->flags |= F_BREAK | F_NOEXEC;
  return js_mkundef();
}

static jsval_t js_continue(struct js *js) {
  js->consumed = 1;
  
  uint8_t nxt = next(js);
  if (nxt == TOK_IDENTIFIER && !js->had_newline) {
    const char *label = &js->code[js->toff];
    jsoff_t label_len = js->tlen;
    js->consumed = 1;
    
    if (js->flags & F_NOEXEC) {
      return js_mkundef();
    }
    
    if (!label_exists(label, label_len, true)) {
      return js_mkerr_typed(js, JS_ERR_SYNTAX, "undefined label '%.*s' or not a loop", (int)label_len, label);
    }
    
    continue_target_label = label;
    continue_target_label_len = label_len;
    label_flags |= F_CONTINUE_LABEL;
    js->flags |= F_BREAK | F_NOEXEC;
    return js_mkundef();
  }
  
  if (js->flags & F_NOEXEC) {
    return js_mkundef();
  }
  
  if (!(js->flags & F_LOOP)) {
    return js_mkerr(js, "not in loop");
  }
  
  js->flags |= F_NOEXEC;
  return js_mkundef();
}

static jsval_t js_return(struct js *js) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  uint8_t in_func = js->flags & F_CALL;
  js->consumed = 1;
  jsval_t res = js_mkundef();
  
  uint8_t nxt = next(js);
  if (nxt != TOK_SEMICOLON && nxt != TOK_RBRACE && nxt != TOK_EOF && !js->had_newline) {
    res = resolveprop(js, js_expr_comma(js));
  }
  
  if (exe && !in_func) return js_mkundef();
  
  if (exe) {
    js->pos = js->clen;
    js->flags |= F_RETURN | F_NOEXEC;
  }
  
  return res;
}

static jsval_t js_switch(struct js *js) {
  uint8_t flags = js->flags, exe = !(flags & F_NOEXEC);
  jsval_t res = js_mkundef();
  
  js->consumed = 1;
  if (!expect(js, TOK_LPAREN, &res)) return res;
  
  jsoff_t switch_expr_start = js->pos;
  uint8_t saved_flags = js->flags;
  js->flags |= F_NOEXEC;
  jsval_t switch_expr = js_expr(js);
  js->flags = saved_flags;
  
  if (is_err(switch_expr)) return switch_expr;
  
  if (!expect(js, TOK_RPAREN, &res)) return res;
  if (!expect(js, TOK_LBRACE, &res)) return res;
  
  typedef struct {
    jsoff_t case_expr_start;
    jsoff_t case_expr_end;
    jsoff_t body_start;
    bool is_default;
  } CaseInfo;
  
  CaseInfo cases[64];
  int case_count = 0;
  
  js->flags |= F_NOEXEC;
  
  while (next(js) != TOK_RBRACE && next(js) != TOK_EOF && case_count < 64) {
    if (next(js) == TOK_CASE) {
      js->consumed = 1;
      
      cases[case_count].is_default = false;
      cases[case_count].case_expr_start = js->pos;
      
      jsval_t case_val = js_expr(js);
      if (is_err(case_val)) {
        js->flags = flags;
        return case_val;
      }
      
      cases[case_count].case_expr_end = js->pos;
      
      if (!expect(js, TOK_COLON, &res)) {
        js->flags = flags;
        return res;
      }
      
      cases[case_count].body_start = js->pos;
      case_count++;
      
      while (next(js) != TOK_EOF && next(js) != TOK_CASE && next(js) != TOK_DEFAULT && next(js) != TOK_RBRACE) {
        jsval_t stmt = js_stmt(js);
        if (is_err(stmt)) {
          js->flags = flags;
          return stmt;
        }
      }
      
    } else if (next(js) == TOK_DEFAULT) {
      js->consumed = 1;
      
      cases[case_count].is_default = true;
      cases[case_count].case_expr_start = 0;
      cases[case_count].case_expr_end = 0;
      
      if (!expect(js, TOK_COLON, &res)) {
        js->flags = flags;
        return res;
      }
      
      cases[case_count].body_start = js->pos;
      case_count++;
      
      while (next(js) != TOK_EOF && next(js) != TOK_CASE && next(js) != TOK_DEFAULT && next(js) != TOK_RBRACE) {
        jsval_t stmt = js_stmt(js);
        if (is_err(stmt)) {
          js->flags = flags;
          return stmt;
        }
      }
      
    } else {
      break;
    }
  }
  
  if (!expect(js, TOK_RBRACE, &res)) {
    js->flags = flags;
    return res;
  }
  
  jsoff_t end_pos = js->pos;
  
  if (exe) {
    js->pos = switch_expr_start;
    js->consumed = 1;
    js->flags = flags;
    jsval_t switch_val = resolveprop(js, js_expr(js));
    
    if (is_err(switch_val)) {
      js->pos = end_pos;
      js->flags = flags;
      return switch_val;
    }
    
    int matching_case = -1;
    int default_case = -1;
    
    for (int i = 0; i < case_count; i++) {
      if (cases[i].is_default) {
        default_case = i;
        continue;
      }
      
      js->pos = cases[i].case_expr_start;
      js->consumed = 1;
      js->flags = flags;
      jsval_t case_val = resolveprop(js, js_expr(js));
      
      if (is_err(case_val)) {
        js->pos = end_pos;
        js->flags = flags;
        return case_val;
      }
      
      bool matches = false;
      if (vtype(switch_val) == vtype(case_val)) {
        if (vtype(switch_val) == T_NUM) {
          matches = tod(switch_val) == tod(case_val);
        } else if (vtype(switch_val) == T_STR) {
          jsoff_t n1, off1 = vstr(js, switch_val, &n1);
          jsoff_t n2, off2 = vstr(js, case_val, &n2);
          matches = n1 == n2 && memcmp(&js->mem[off1], &js->mem[off2], n1) == 0;
        } else if (vtype(switch_val) == T_BOOL) {
          matches = vdata(switch_val) == vdata(case_val);
        } else {
          matches = vdata(switch_val) == vdata(case_val);
        }
      }
      
      if (matches) {
        matching_case = i;
        break;
      }
    }
    
    if (matching_case < 0 && default_case >= 0) matching_case = default_case;
    
    if (matching_case >= 0) {
      js->flags = (flags & ~F_NOEXEC) | F_SWITCH;
      
      for (int i = matching_case; i < case_count; i++) {
        js->pos = cases[i].body_start;
        js->consumed = 1;
        
        while (next(js) != TOK_EOF && next(js) != TOK_CASE && 
               next(js) != TOK_DEFAULT && next(js) != TOK_RBRACE &&
               !(js->flags & (F_BREAK | F_RETURN | F_THROW))) {
          res = js_stmt(js);
          if (is_err(res)) {
            js->pos = end_pos;
            uint8_t preserve = 0;
            if (js->flags & F_RETURN) {
              preserve = js->flags & (F_RETURN | F_NOEXEC);
            }
            if (js->flags & F_THROW) {
              preserve = js->flags & (F_THROW | F_NOEXEC);
            }
            js->flags = flags | preserve;
            return res;
          }
        }
        
        if (js->flags & F_BREAK) { js->flags &= ~F_BREAK; break; }
        if (js->flags & (F_RETURN | F_THROW)) break;
      }
    }
  }
  
  js->pos = end_pos;
  js->tok = TOK_SEMICOLON;
  js->consumed = 0;
  
  uint8_t preserve = 0;
  if (js->flags & F_RETURN) {
    preserve = js->flags & (F_RETURN | F_NOEXEC);
  }
  if (js->flags & F_THROW) {
    preserve = js->flags & (F_THROW | F_NOEXEC);
  }
  js->flags = (flags & ~F_SWITCH) | preserve;
  
  return res;
}

static jsval_t js_with(struct js *js) {
  uint8_t flags = js->flags, exe = !(flags & F_NOEXEC);
  jsval_t res = js_mkundef();
  
  if (flags & F_STRICT) {
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "with statement not allowed in strict mode");
  }
  
  js->consumed = 1;
  if (!expect(js, TOK_LPAREN, &res)) return res;
  
  jsval_t obj_expr = js_expr(js);
  if (is_err(obj_expr)) return obj_expr;
  
  if (!expect(js, TOK_RPAREN, &res)) return res;
  
  if (exe) {
    jsval_t obj = resolveprop(js, obj_expr);
    if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR && vtype(obj) != T_FUNC) {
      return js_mkerr(js, "with requires object");
    }
    
    jsval_t with_obj = obj;
    if (vtype(obj) == T_FUNC) {
      with_obj = mkval(T_OBJ, vdata(obj));
    }
    
    jsoff_t parent_scope_offset = (jsoff_t) vdata(js->scope);
    if (global_scope_stack == NULL) utarray_new(global_scope_stack, &jsoff_icd);
    utarray_push_back(global_scope_stack, &parent_scope_offset);
    
    jsval_t with_scope = mkobj(js, parent_scope_offset);
    set_slot(js, with_scope, SLOT_WITH, with_obj);
    
    jsval_t saved_scope = js->scope;
    js->scope = with_scope;
    
    res = js_block_or_stmt(js);
    if (global_scope_stack && utarray_len(global_scope_stack) > 0) utarray_pop_back(global_scope_stack);
    js->scope = saved_scope;
  } else res = js_block_or_stmt(js);
  
  js->flags = flags;
  return res;
}

static jsval_t js_class_expr(struct js *js, bool is_expression);
static jsval_t js_class_decl(struct js *js) { return js_class_expr(js, false); }

static jsval_t js_class_expr(struct js *js, bool is_expression) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  js->consumed = 1;
  
  jsoff_t class_name_off = 0, class_name_len = 0;
  char *class_name = NULL;
  
  if (next(js) == TOK_IDENTIFIER) {
    if (!(js->tlen == 7 && streq(&js->code[js->toff], js->tlen, "extends", 7))) {
      class_name_off = js->toff;
      class_name_len = js->tlen;
      class_name = (char *) &js->code[class_name_off];
      js->consumed = 1;
    }
  } else if (!is_expression) {
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "class name expected");
  }
  
  jsoff_t super_off = 0, super_len = 0;
  if (next(js) == TOK_IDENTIFIER && js->tlen == 7 &&
      streq(&js->code[js->toff], js->tlen, "extends", 7)) {
    js->consumed = 1;
    EXPECT_IDENT();
    super_off = js->toff;
    super_len = js->tlen;
    js->consumed = 1;
  }
  
  EXPECT(TOK_LBRACE);
  jsoff_t constructor_params_start = 0;
  jsoff_t constructor_body_start = 0, constructor_body_end = 0;
  uint8_t save_flags = js->flags;
  js->flags |= F_NOEXEC;
  
  typedef struct { 
    jsoff_t name_off, name_len, fn_start, fn_end; 
    bool is_async;
    bool is_static;
    bool is_field;
    bool is_private;
    bool is_getter;
    bool is_setter;
    jsoff_t field_start, field_end; 
    jsoff_t param_start;
  } MethodInfo;
  
  static const UT_icd method_info_icd = {
    .sz = sizeof(MethodInfo),
    .init = NULL,
    .copy = NULL,
    .dtor = NULL,
  };
  
  UT_array *methods = NULL;
  utarray_new(methods, &method_info_icd);
  
  uint8_t class_tok;
  while ((class_tok = next(js)) != TOK_RBRACE && class_tok != TOK_EOF) {
    bool is_async_method = false;
    bool is_static_member = false;
    bool is_getter_method = false;
    bool is_setter_method = false;
    
    if (next(js) == TOK_STATIC) {
      is_static_member = true;
      js->consumed = 1;
    }
    if (next(js) == TOK_ASYNC) {
      is_async_method = true;
      js->consumed = 1;
    }
    
    bool is_private_member = false;
    if (next(js) == TOK_HASH) {
      js->consumed = 1;
      if (next(js) == TOK_IDENTIFIER) { is_private_member = true; } else {
        js->flags = save_flags;
        return js_mkerr_typed(js, JS_ERR_SYNTAX, "private field name expected");
      }
    }
    
    if (next(js) == TOK_IDENTIFIER) {
      bool is_get = (js->tlen == 3 && memcmp(&js->code[js->toff], "get", 3) == 0);
      bool is_set = (js->tlen == 3 && memcmp(&js->code[js->toff], "set", 3) == 0);
      if (!is_private_member && (is_get || is_set)) {
        jsoff_t saved_pos = js->pos;
        jsoff_t saved_toff = js->toff;
        jsoff_t saved_tlen = js->tlen;
        uint8_t saved_tok = js->tok;
        js->consumed = 1;
        if (next(js) == TOK_IDENTIFIER) {
          is_getter_method = is_get;
          is_setter_method = is_set;
        } else {
          js->pos = saved_pos;
          js->toff = saved_toff;
          js->tlen = saved_tlen;
          js->tok = saved_tok;
          js->consumed = 0;
        }
      }
    }
    
    if (next(js) != TOK_IDENTIFIER && (next(js) < TOK_ASYNC || next(js) > TOK_STATIC)) {
      js->flags = save_flags;
      return js_mkerr_typed(js, JS_ERR_SYNTAX, "method name expected");
    }
    jsoff_t method_name_off = js->toff, method_name_len = js->tlen;
    js->consumed = 1;
    
    if (next(js) == TOK_ASSIGN) {
      js->consumed = 1;
      jsoff_t field_start = js->pos;
      int depth = 0;
      bool done = false;
      while (!done && next(js) != TOK_EOF) {
        uint8_t tok = next(js);
        if (depth == 0 && (tok == TOK_SEMICOLON || tok == TOK_RBRACE || 
            (tok == TOK_IDENTIFIER && js->pos > field_start + 1))) {
          if (tok != TOK_SEMICOLON && tok != TOK_RBRACE) {
            js->consumed = 0;
          }
          done = true;
        } else if (tok == TOK_LPAREN || tok == TOK_LBRACKET || tok == TOK_LBRACE) {
          depth++;
          js->consumed = 1;
        } else if (tok == TOK_RPAREN || tok == TOK_RBRACKET || tok == TOK_RBRACE) {
          if (depth == 0) {
            done = true;
          } else {
            depth--;
            js->consumed = 1;
          }
        } else {
          js->consumed = 1;
        }
      }
      jsoff_t field_end = js->pos;
      if (next(js) == TOK_SEMICOLON) js->consumed = 1;
      
      MethodInfo field_method = {
        .name_off = method_name_off,
        .name_len = method_name_len,
        .is_static = is_static_member,
        .is_async = false,
        .is_field = true,
        .is_private = is_private_member,
        .field_start = field_start,
        .field_end = field_end,
        .fn_start = 0,
        .fn_end = 0,
      };
      utarray_push_back(methods, &field_method);
      continue;
    }
    
    if (next(js) == TOK_SEMICOLON || (next(js) != TOK_LPAREN && next(js) == TOK_IDENTIFIER)) {
      if (next(js) == TOK_SEMICOLON) js->consumed = 1;
      MethodInfo bare_method = {
        .name_off = method_name_off,
        .name_len = method_name_len,
        .is_static = is_static_member,
        .is_async = false,
        .is_field = true,
        .is_private = is_private_member,
        .field_start = 0,
        .field_end = 0,
        .fn_start = 0,
        .fn_end = 0,
      };
      utarray_push_back(methods, &bare_method);
      continue;
    }
    
    EXPECT(TOK_LPAREN, js->flags = save_flags);
    jsoff_t method_params_start = js->pos - 1;
    if (!parse_func_params(js, &save_flags, NULL)) {
      js->flags = save_flags;
      return js_mkerr_typed(js, JS_ERR_SYNTAX, "invalid method parameters");
    }
    EXPECT(TOK_RPAREN, js->flags = save_flags);
    EXPECT(TOK_LBRACE, js->flags = save_flags);
    jsoff_t method_body_start = js->pos - 1;
    js->consumed = 0;
    jsval_t blk = js_block(js, false);
    if (is_err(blk)) {
      js->flags = save_flags;
      return blk;
    }
    jsoff_t method_body_end = js->pos;
    if (streq(&js->code[method_name_off], method_name_len, "constructor", 11)) {
      constructor_params_start = method_params_start;
      constructor_body_start = method_body_start + 1;
      constructor_body_end = method_body_end;
    } else {
      MethodInfo func_method = {
        .name_off = method_name_off,
        .name_len = method_name_len,
        .fn_start = method_params_start,
        .fn_end = method_body_end,
        .param_start = method_params_start,
        .is_async = is_async_method,
        .is_static = is_static_member,
        .is_field = false,
        .is_private = is_private_member,
        .is_getter = is_getter_method,
        .is_setter = is_setter_method,
        .field_start = 0,
        .field_end = 0,
      };
      utarray_push_back(methods, &func_method);
     }
    js->consumed = 1;
  }
  
  EXPECT(TOK_RBRACE, js->flags = save_flags);
  js->flags = save_flags;
  
  if (exe) {
    jsval_t super_constructor = js_mkundef();
    jsval_t super_proto = js_mknull();
    if (super_len > 0) {
      jsval_t super_val = lookup(js, &js->code[super_off], super_len);
      if (is_err(super_val)) return super_val;
      super_constructor = resolveprop(js, super_val);
      if (vtype(super_constructor) != T_FUNC && vtype(super_constructor) != T_CFUNC) {
        return js_mkerr(js, "super class must be a constructor");
      }
      jsval_t super_obj = mkval(T_OBJ, vdata(super_constructor));
      jsoff_t super_proto_off = lkp_interned(js, super_obj, INTERN_PROTOTYPE, 9);
      if (super_proto_off != 0) {
        super_proto = resolveprop(js, mkval(T_PROP, super_proto_off));
      }
    }
    
    jsval_t proto = js_mkobj(js);
    if (is_err(proto)) return proto;
    
    if (vtype(super_proto) == T_OBJ) {
      set_proto(js, proto, super_proto);
    } else {
      jsval_t object_proto = get_ctor_proto(js, "Object", 6);
      if (vtype(object_proto) == T_OBJ) set_proto(js, proto, object_proto);
    }
    
    jsval_t func_scope = mkobj(js, (jsoff_t) vdata(js->scope));
    
    for (unsigned int i = 0; i < utarray_len(methods); i++) {
      MethodInfo *m = (MethodInfo *)utarray_eltptr(methods, i);
      if (m->is_static) continue;
      if (m->is_field) continue;
      
      jsval_t method_name = js_mkstr(js, &js->code[m->name_off], m->name_len);
      if (is_err(method_name)) return method_name;
      
      jsoff_t mlen = m->fn_end - m->fn_start;
      
      jsval_t method_obj = mkobj(js, 0);
      if (is_err(method_obj)) return method_obj;
      
      set_func_code(js, method_obj, &js->code[m->fn_start], mlen);
      set_slot(js, method_obj, SLOT_SCOPE, func_scope);
      
      if (m->is_async) {
        set_slot(js, method_obj, SLOT_ASYNC, js_true);
        jsval_t async_proto = get_slot(js, js_glob(js), SLOT_ASYNC_PROTO);
        if (vtype(async_proto) == T_FUNC) set_proto(js, method_obj, async_proto);
      } else {
        jsval_t func_proto = get_slot(js, js_glob(js), SLOT_FUNC_PROTO);
        if (vtype(func_proto) == T_FUNC) set_proto(js, method_obj, func_proto);
      }
      
      if (super_len > 0) set_slot(js, method_obj, SLOT_SUPER, super_constructor);
      jsval_t method_func = mkval(T_FUNC, (unsigned long) vdata(method_obj));
      
      if (m->is_getter || m->is_setter) {
        jsoff_t name_len;
        const char *name_str = (const char *)&js->mem[vstr(js, method_name, &name_len)];
        
        if (m->is_getter) {
          js_set_getter_desc(js, proto, name_str, name_len, method_func, JS_DESC_C);
        } else {
          js_set_setter_desc(js, proto, name_str, name_len, method_func, JS_DESC_C);
        }
      } else {
        jsval_t set_res = setprop(js, proto, method_name, method_func);
        if (is_err(set_res)) return set_res;
      }
    }
    
    jsval_t func_obj = mkobj(js, 0);
    if (is_err(func_obj)) return func_obj;
    
    if (constructor_params_start > 0 && constructor_body_start > 0) {
      jsoff_t code_len = constructor_body_end - constructor_params_start;
      set_func_code(js, func_obj, &js->code[constructor_params_start], code_len);
    } else {
      set_func_code_ptr(js, func_obj, "(){}", 4);
      if (super_len > 0) set_slot(js, func_obj, SLOT_DEFAULT_CTOR, js_true);
    }
    
    int instance_field_count = 0;
    for (unsigned int i = 0; i < utarray_len(methods); i++) {
      MethodInfo *m = (MethodInfo *)utarray_eltptr(methods, i);
      if (m->is_static) continue;
      if (!m->is_field) continue;
      instance_field_count++;
    }
    
    if (instance_field_count > 0) {
      size_t metadata_size = instance_field_count * sizeof(jsoff_t) * 4;
      jsoff_t meta_len = (jsoff_t) (metadata_size + 1);
      jsoff_t meta_header = (jsoff_t) ((meta_len << 3) | T_STR);
      jsoff_t meta_off = js_alloc(js, meta_len + sizeof(meta_header));
      if (meta_off == (jsoff_t) ~0) return js_mkerr(js, "oom");
      
      memcpy(&js->mem[meta_off], &meta_header, sizeof(meta_header));
      jsoff_t *metadata = (jsoff_t *)(&js->mem[meta_off + sizeof(meta_header)]);
      
      int meta_idx = 0;
      for (unsigned int i = 0; i < utarray_len(methods); i++) {
        MethodInfo *m = (MethodInfo *)utarray_eltptr(methods, i);
        if (m->is_static) continue;
        if (!m->is_field) continue;
        
        metadata[meta_idx * 4 + 0] = m->name_off;
        metadata[meta_idx * 4 + 1] = m->name_len;
        metadata[meta_idx * 4 + 2] = m->field_start;
        metadata[meta_idx * 4 + 3] = m->field_end;
        meta_idx++;
      }
      
      js->mem[meta_off + sizeof(meta_header) + metadata_size] = 0;
      jsval_t fields_meta = mkval(T_STR, meta_off);
      
      set_slot(js, func_obj, SLOT_FIELD_COUNT, tov((double)instance_field_count));
      set_slot(js, func_obj, SLOT_FIELDS, fields_meta);
      
      const char *arena_src = code_arena_alloc(js->code, js->clen);
      if (arena_src) set_slot(js, func_obj, SLOT_SOURCE, mkval(T_CFUNC, (size_t)arena_src));
    }
    
    set_slot(js, func_obj, SLOT_SCOPE, func_scope);
    if (super_len > 0) set_slot(js, func_obj, SLOT_SUPER, super_constructor);
    
    jsval_t func_proto = get_slot(js, js_glob(js), SLOT_FUNC_PROTO);
    if (vtype(func_proto) == T_FUNC) set_proto(js, func_obj, func_proto);
    
    jsval_t name_key = js_mkstr(js, "name", 4);
    if (is_err(name_key)) return name_key;
    jsval_t name_val = class_name_len > 0 ? js_mkstr(js, class_name, class_name_len) : js_mkstr(js, "", 0);
    if (is_err(name_val)) return name_val;
    jsval_t res_name = setprop(js, func_obj, name_key, name_val);
    if (is_err(res_name)) return res_name;
    
    jsval_t proto_key = js_mkstr(js, "prototype", 9);
    if (is_err(proto_key)) return proto_key;
    jsval_t proto_res = setprop(js, func_obj, proto_key, proto);
    if (is_err(proto_res)) return proto_res;
    
    jsval_t constructor = mkval(T_FUNC, (unsigned long) vdata(func_obj));
    
    jsval_t ctor_key = js_mkstr(js, "constructor", 11);
    if (is_err(ctor_key)) return ctor_key;
    jsval_t ctor_res = setprop(js, proto, ctor_key, constructor);
    if (is_err(ctor_res)) return ctor_res;
    js_set_descriptor(js, proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
    
    if (class_name_len > 0) {
      if (lkp_scope(js, js->scope, class_name, class_name_len) > 0) {
        return js_mkerr(js, "'%.*s' already declared", (int) class_name_len, class_name);
      }
      jsval_t x = mkprop(js, js->scope, js_mkstr(js, class_name, class_name_len), constructor, 0);
      if (is_err(x)) return x;
    }
    
    for (unsigned int i = 0; i < utarray_len(methods); i++) {
      MethodInfo *m = (MethodInfo *)utarray_eltptr(methods, i);
      if (!m->is_static) continue;
      
      jsval_t member_name = js_mkstr(js, &js->code[m->name_off], m->name_len);
      if (is_err(member_name)) return member_name;
      
      if (m->is_field) {
        jsval_t field_val = js_mkundef();
        if (m->field_start > 0 && m->field_end > m->field_start) {
          field_val = js_eval_slice(js, m->field_start, m->field_end - m->field_start);
          field_val = resolveprop(js, field_val);
        }
        jsval_t set_res = setprop(js, func_obj, member_name, field_val);
        if (is_err(set_res)) return set_res;
      } else {
        jsoff_t mlen = m->fn_end - m->fn_start;
        
        jsval_t method_obj = mkobj(js, 0);
        if (is_err(method_obj)) return method_obj;
        
        set_func_code(js, method_obj, &js->code[m->fn_start], mlen);
        set_slot(js, method_obj, SLOT_SCOPE, func_scope);
        if (super_len > 0) set_slot(js, method_obj, SLOT_SUPER, super_constructor);
        
        jsval_t method_func_proto = get_slot(js, js_glob(js), SLOT_FUNC_PROTO);
        if (vtype(method_func_proto) == T_FUNC) set_proto(js, method_obj, method_func_proto);
        
        jsval_t method_func = mkval(T_FUNC, (unsigned long) vdata(method_obj));
        jsval_t set_res = setprop(js, func_obj, member_name, method_func);
        if (is_err(set_res)) return set_res;
      }
    }
    
    utarray_free(methods);
    return constructor;
  }
  
  utarray_free(methods);
  return js_mkundef();
}

static void js_throw_handle(struct js *js, jsval_t *res) {
  js->consumed = 1;
  jsval_t throw_val = js_expr(js);
  if (js->flags & F_NOEXEC) *res = js_mkundef(); else {
    throw_val = resolveprop(js, throw_val);
    if (is_err(throw_val)) *res = throw_val;
    else *res = js_throw(js, throw_val);
  }
}

static jsval_t find_var_scope(struct js *js) {
  jsval_t scope = js->scope;
  
  jsval_t eval_marker = get_slot(js, scope, SLOT_STRICT_EVAL_SCOPE);
  if (vtype(eval_marker) != T_UNDEF) return scope;
  
  jsval_t module_marker = get_slot(js, scope, SLOT_MODULE_SCOPE);
  if (vtype(module_marker) != T_UNDEF) return scope;
  
  if ((js->flags & F_CALL) && global_scope_stack && utarray_len(global_scope_stack) > 0) {
    jsoff_t *scope_off = (jsoff_t *)utarray_back(global_scope_stack);
    if (scope_off && *scope_off != 0) return mkval(T_OBJ, *scope_off);
  }
  
  while (vdata(upper(js, scope)) != 0) {
    jsval_t parent = upper(js, scope);
    
    jsval_t parent_eval_marker = get_slot(js, parent, SLOT_STRICT_EVAL_SCOPE);
    if (vtype(parent_eval_marker) != T_UNDEF) return scope;
    
    jsval_t parent_module_marker = get_slot(js, parent, SLOT_MODULE_SCOPE);
    if (vtype(parent_module_marker) != T_UNDEF) return scope;
    
    scope = parent;
  }
  
  return scope;
}

static jsval_t js_var_decl(struct js *js) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  jsval_t var_scope = find_var_scope(js);
  
  js->consumed = 1;
  for (;;) {
    EXPECT_IDENT();
    js->consumed = 0;
    jsoff_t noff = js->toff, nlen = js->tlen;
    char *name = (char *) &js->code[noff];
    
    if (exe && (js->flags & F_STRICT) && is_strict_restricted(name, nlen)) {
      return js_mkerr_typed(js, JS_ERR_SYNTAX, "cannot use '%.*s' as variable name in strict mode", (int) nlen, name);
    }
    
    if (exe && (js->flags & F_STRICT) && is_strict_reserved(name, nlen)) {
      return js_mkerr_typed(js, JS_ERR_SYNTAX, "'%.*s' is reserved in strict mode", (int) nlen, name);
    }
    
    jsval_t v = js_mkundef();
    bool has_initializer = false;
    js->consumed = 1;
    if (next(js) == TOK_ASSIGN) {
      js->consumed = 1;
      v = js_expr(js);
      if (is_err(v)) return v;
      has_initializer = true;
    }
    
    if (exe) {
      char decoded_name[256];
      size_t decoded_len = decode_ident_escapes(name, nlen, decoded_name, sizeof(decoded_name));
      
      jsoff_t existing_off = lkp(js, var_scope, decoded_name, decoded_len);
      if (existing_off > 0) {
        if (has_initializer && !is_err(v)) {
          jsval_t key_val = js_mkstr(js, decoded_name, decoded_len);
          setprop(js, var_scope, key_val, resolveprop(js, v));
        }
      } else {
        jsval_t x = mkprop(js, var_scope, js_mkstr(js, decoded_name, decoded_len), resolveprop(js, v), 0);
        if (is_err(x)) return x;
      }
    }
    
    uint8_t var_next = next(js);
    if (var_next == TOK_SEMICOLON || var_next == TOK_EOF || var_next == TOK_RBRACE || js->had_newline) break;
    EXPECT(TOK_COMMA);
  }
  return js_mkundef();
}

static void js_var(struct js *js, jsval_t *res) {
  if ((js->flags & F_STRICT) && !js->var_warning_shown) {
    fprintf(stderr, "Warning: 'var' is deprecated, use 'let' or 'const' instead\n");
    js->var_warning_shown = true;
  }
  
  *res = js_var_decl(js);
}

static void js_async(struct js *js, jsval_t *res) {
  js->consumed = 1;
  uint8_t next_tok = next(js);
  if (next_tok == TOK_FUNC) {
    *res = js_func_decl_async(js);
    return;
  }
  if (next_tok == TOK_LPAREN) {
    *res = js_async_arrow_paren(js);
    return;
  }
  *res = js_mkerr_typed(js, JS_ERR_SYNTAX, "async must be followed by function");
}

static jsval_t js_stmt(struct js *js);

static bool is_break_target(const char *name, jsoff_t len) {
  if (!(label_flags & F_BREAK_LABEL)) return false;
  if (break_target_label_len != len) return false;
  return memcmp(break_target_label, name, len) == 0;
}

static bool is_continue_target(const char *name, jsoff_t len) {
  if (!(label_flags & F_CONTINUE_LABEL)) return false;
  if (continue_target_label_len != len) return false;
  return memcmp(continue_target_label, name, len) == 0;
}

static jsval_t js_labeled_stmt(struct js *js, const char *label, jsoff_t label_len) {
  uint8_t flags = js->flags;
  jsval_t res = js_mkundef();
  
  if (!label_stack) {
    utarray_new(label_stack, &label_entry_icd);
  }
  
  uint8_t next_tok = next(js);
  bool is_loop = (next_tok == TOK_WHILE || next_tok == TOK_DO || next_tok == TOK_FOR);
  bool is_block = (next_tok == TOK_LBRACE);
  
  label_entry_t entry = {
    .name = label,
    .name_len = label_len,
    .is_loop = is_loop,
    .is_block = is_block || !is_loop
  };
  utarray_push_back(label_stack, &entry);
  
  if (is_loop && !(flags & F_NOEXEC)) {
    res = js_stmt_impl(js);
    
    if ((js->flags & F_BREAK) && is_break_target(label, label_len)) {
      js->flags &= ~(F_BREAK | F_NOEXEC);
      clear_break_label();
      res = js_mkundef();
    }
    
    if ((label_flags & F_CONTINUE_LABEL) && is_continue_target(label, label_len)) {
      clear_continue_label();
    }
  } else if (is_loop && (flags & F_NOEXEC)) {
    res = js_stmt_impl(js);
  } else {
    res = js_stmt_impl(js);
    
    if ((js->flags & F_BREAK) && is_break_target(label, label_len)) {
      js->flags &= ~(F_BREAK | F_NOEXEC);
      js->flags |= (flags & F_NOEXEC);
      clear_break_label();
      res = js_mkundef();
    }
  }
  
  if (label_stack && utarray_len(label_stack) > 0) {
    utarray_pop_back(label_stack);
  }
  
  return res;
}

static jsval_t js_stmt_impl(struct js *js) {
  jsval_t res;
  uint8_t stmt_tok = next(js);
  
  switch (stmt_tok) {
    case TOK_SEMICOLON:
      res = js_mkundef();
      break;
    case TOK_CASE: case TOK_CATCH:
    case TOK_DEFAULT: case TOK_FINALLY:
      res = js_mkerr(js, "SyntaxError '%.*s'", (int) js->tlen, js->code + js->toff);
      break;
    case TOK_YIELD:
      res = js_mkerr(js, " '%.*s' not implemented", (int) js->tlen, js->code + js->toff);
      break;
    case TOK_IMPORT:    res = js_import_stmt(js); break;
    case TOK_EXPORT:    res = js_export_stmt(js); break;
    case TOK_THROW:     js_throw_handle(js, &res); break;
    case TOK_VAR:       js_var(js, &res); break;
    case TOK_ASYNC:     js_async(js, &res); break;
    case TOK_WITH:      res = js_with(js); break;
    case TOK_SWITCH:    res = js_switch(js); break;
    case TOK_WHILE:     res = js_while(js); break;
    case TOK_DO:        res = js_do_while(js); break;
    case TOK_DEBUGGER:  js->consumed = 1; res = js_mkundef(); break;
    case TOK_CONTINUE:  res = js_continue(js); break;
    case TOK_BREAK:     res = js_break(js); break;
    case TOK_LET:       res = js_let(js); break;
    case TOK_CONST:     res = js_const(js); break;
    case TOK_FUNC:      res = js_func_decl(js); break;
    case TOK_CLASS:     res = js_class_decl(js); break;
    case TOK_IF:        res = js_if(js); break;
    case TOK_LBRACE:    res = js_block(js, !(js->flags & F_NOEXEC)); break;
    case TOK_FOR:       res = js_for(js); break;
    case TOK_RETURN:    res = js_return(js); break;
    case TOK_TRY:       res = js_try(js); break;
    default:
      res = resolveprop(js, js_expr(js));
      while (next(js) == TOK_COMMA) {
        js->consumed = 1;
        res = resolveprop(js, js_expr(js));
      }
      break;
  }
  
  bool is_block_statement = (
    stmt_tok == TOK_FUNC || stmt_tok == TOK_CLASS || 
    stmt_tok == TOK_EXPORT || stmt_tok == TOK_IMPORT ||
    stmt_tok == TOK_IF || stmt_tok == TOK_WHILE || 
    stmt_tok == TOK_DO || stmt_tok == TOK_FOR || 
    stmt_tok == TOK_SWITCH || stmt_tok == TOK_TRY || 
    stmt_tok == TOK_LBRACE || stmt_tok == TOK_ASYNC
  );
  
  if (is_err(res)) return res;
  
  if (!is_block_statement) {
    int next_tok = next(js);
    bool asi_applies = js->had_newline || next_tok == TOK_EOF || next_tok == TOK_RBRACE;
    bool missing_semicolon = next_tok != TOK_SEMICOLON && !asi_applies;
    if (missing_semicolon) return js_mkerr_typed(js, JS_ERR_SYNTAX, "; expected");
    if (next_tok == TOK_SEMICOLON) js->consumed = 1;
  }
  
  return res;
}

static jsval_t js_stmt(struct js *js) {
  uint8_t tok = next(js);
  
  if (tok == TOK_IDENTIFIER) {
    js_parse_state_t saved_state;
    JS_SAVE_STATE(js, saved_state);
    jsoff_t saved_toff = js->toff;
    jsoff_t saved_tlen = js->tlen;
    
    const char *potential_label = &js->code[js->toff];
    jsoff_t potential_label_len = js->tlen;
    
    js->consumed = 1;
    
    if (next(js) == TOK_COLON) {
      js->consumed = 1;
      return js_labeled_stmt(js, potential_label, potential_label_len);
    }
    
    JS_RESTORE_STATE(js, saved_state);
    js->toff = saved_toff;
    js->tlen = saved_tlen;
  }
  
  return js_stmt_impl(js);
}

static jsval_t builtin_String(struct js *js, jsval_t *args, int nargs) {
  jsval_t sval;
  if (nargs == 0) {
    sval = js_mkstr(js, "", 0);
  } else {
    jsval_t arg = args[0];
    if (vtype(arg) == T_STR) {
      sval = arg;
    } else {
      const char *str = js_str(js, arg);
      sval = js_mkstr(js, str, strlen(str));
    }
  }
  jsval_t string_proto = js_get_ctor_proto(js, "String", 6);
  if (is_unboxed_obj(js, js->this_val, string_proto)) {
    set_slot(js, js->this_val, SLOT_PRIMITIVE, sval);
    jsoff_t slen;
    vstr(js, sval, &slen);
    setprop(js, js->this_val, js_mkstr(js, "length", 6), tov((double)slen));
    js_set_descriptor(js, js->this_val, "length", 6, 0);
  }
  return sval;
}

static jsval_t builtin_Number_isNaN(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkval(T_BOOL, 0);
  jsval_t arg = args[0];
  
  if (vtype(arg) != T_NUM) return mkval(T_BOOL, 0);
  
  double val = tod(arg);
  return mkval(T_BOOL, isnan(val) ? 1 : 0);
}

static jsval_t builtin_Number_isFinite(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkval(T_BOOL, 0);
  jsval_t arg = args[0];
  
  if (vtype(arg) != T_NUM) return mkval(T_BOOL, 0);
  
  double val = tod(arg);
  return mkval(T_BOOL, isfinite(val) ? 1 : 0);
}

static jsval_t builtin_global_isNaN(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkval(T_BOOL, 1);
  double val = js_to_number(js, args[0]);
  return mkval(T_BOOL, isnan(val) ? 1 : 0);
}

static jsval_t builtin_global_isFinite(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkval(T_BOOL, 0);
  double val = js_to_number(js, args[0]);
  return mkval(T_BOOL, isfinite(val) ? 1 : 0);
}

static jsval_t builtin_Number_isInteger(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkval(T_BOOL, 0);
  jsval_t arg = args[0];
  
  if (vtype(arg) != T_NUM) return mkval(T_BOOL, 0);
  
  double val = tod(arg);
  if (!isfinite(val)) return mkval(T_BOOL, 0);
  return mkval(T_BOOL, (val == floor(val)) ? 1 : 0);
}

static jsval_t builtin_Number_isSafeInteger(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkval(T_BOOL, 0);
  jsval_t arg = args[0];
  
  if (vtype(arg) != T_NUM) return mkval(T_BOOL, 0);
  
  double val = tod(arg);
  if (!isfinite(val)) return mkval(T_BOOL, 0);
  if (val != floor(val)) return mkval(T_BOOL, 0);
  
  return mkval(T_BOOL, (val >= -9007199254740991.0 && val <= 9007199254740991.0) ? 1 : 0);
}

static jsval_t builtin_Number(struct js *js, jsval_t *args, int nargs) {
  jsval_t nval = tov(nargs > 0 ? js_to_number(js, args[0]) : 0.0);
  jsval_t number_proto = js_get_ctor_proto(js, "Number", 6);
  if (is_unboxed_obj(js, js->this_val, number_proto)) {
    set_slot(js, js->this_val, SLOT_PRIMITIVE, nval);
  }
  return nval;
}

static jsval_t builtin_Boolean(struct js *js, jsval_t *args, int nargs) {
  jsval_t bval = mkval(T_BOOL, nargs > 0 && js_truthy(js, args[0]) ? 1 : 0);
  jsval_t boolean_proto = js_get_ctor_proto(js, "Boolean", 7);
  if (is_unboxed_obj(js, js->this_val, boolean_proto)) {
    set_slot(js, js->this_val, SLOT_PRIMITIVE, bval);
  }
  return bval;
}

static jsval_t builtin_Object(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t js_eval_inherit_strict(struct js *js, const char *buf, size_t len, bool inherit_strict);

static jsval_t builtin_eval(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_mkundef();
  
  jsval_t code_arg = args[0];
  if (vtype(code_arg) != T_STR) return code_arg;
  
  jsoff_t code_len, code_off = vstr(js, code_arg, &code_len);
  const char *code_str = (const char *)&js->mem[code_off];
  
  js_parse_state_t saved;
  JS_SAVE_STATE(js, saved);
  uint8_t saved_flags = js->flags;
  bool caller_strict = (js->flags & F_STRICT) != 0;
  
  jsval_t result = js_eval_inherit_strict(js, code_str, code_len, caller_strict);
  bool had_throw = (js->flags & F_THROW) != 0;
  
  JS_RESTORE_STATE(js, saved);
  js->flags = saved_flags;
  
  if (is_err(result) || had_throw) {
    js->flags |= F_THROW;
  }
  
  return result;
}

static jsval_t builtin_Function(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) {
    jsval_t func_obj = mkobj(js, 0);
    if (is_err(func_obj)) return func_obj;
    
    set_func_code_ptr(js, func_obj, "(){}", 4);
    set_slot(js, func_obj, SLOT_SCOPE, js_glob(js));
    
    jsval_t func = mkval(T_FUNC, (unsigned long) vdata(func_obj));
    
    jsval_t proto_setup = setup_func_prototype(js, func);
    if (is_err(proto_setup)) return proto_setup;
    
    return func;
  }
  
  size_t total_len = 1;
  
  for (int i = 0; i < nargs - 1; i++) {
    if (vtype(args[i]) != T_STR) {
      const char *str = js_str(js, args[i]);
      args[i] = js_mkstr(js, str, strlen(str));
      if (is_err(args[i])) return args[i];
    }
    total_len += vstrlen(js, args[i]);
    if (i < nargs - 2) total_len += 1;
  }
  
  total_len += 2;
  
  jsval_t body = args[nargs - 1];
  if (vtype(body) != T_STR) {
    const char *str = js_str(js, body);
    body = js_mkstr(js, str, strlen(str));
    if (is_err(body)) return body;
  }
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
    if (i < nargs - 2) {
      code_buf[pos++] = ',';
    }
  }
  
  code_buf[pos++] = ')';
  code_buf[pos++] = '{';
  
  jsoff_t body_len, body_off = vstr(js, body, &body_len);
  memcpy(code_buf + pos, &js->mem[body_off], body_len);
  pos += body_len;
  
  code_buf[pos++] = '}';
  code_buf[pos] = '\0';
  
  bool is_strict_body = is_strict_function_body((const char *)&js->mem[body_off], body_len);
  if (is_strict_body && nargs > 1) {
    int i = 0, j;
    jsoff_t param_len_i, param_off_i;
    const char *param_i;
    
check_param:
    if (i >= nargs - 1) goto params_done;
    
    param_off_i = vstr(js, args[i], &param_len_i);
    param_i = (const char *)&js->mem[param_off_i];
    
    if (is_strict_restricted(param_i, param_len_i)) {
      free(code_buf);
      return js_mkerr_typed(js, JS_ERR_SYNTAX, "cannot use '%.*s' as parameter name in strict mode", (int)param_len_i, param_i);
    }
    
    j = i + 1;
    
check_dup:
    if (j >= nargs - 1) { i++; goto check_param; }
    
    jsoff_t param_len_j, param_off_j = vstr(js, args[j], &param_len_j);
    const char *param_j = (const char *)&js->mem[param_off_j];
    
    if (param_len_i == param_len_j && memcmp(param_i, param_j, param_len_i) == 0) {
      free(code_buf);
      return js_mkerr_typed(js, JS_ERR_SYNTAX, "duplicate parameter name '%.*s' in strict mode", (int)param_len_i, param_i);
    }
    
    j++;
    goto check_dup;
    
params_done:;
  }

  jsval_t func_obj = mkobj(js, 0);
  if (is_err(func_obj)) { free(code_buf); return func_obj; }
  
  set_func_code(js, func_obj, code_buf, pos);
  free(code_buf);
  set_slot(js, func_obj, SLOT_SCOPE, js_glob(js));
  
  jsval_t func = mkval(T_FUNC, (unsigned long) vdata(func_obj));
  jsval_t proto_setup = setup_func_prototype(js, func);
  if (is_err(proto_setup)) return proto_setup;
  
  return func;
}

static jsval_t builtin_AsyncFunction(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) {
    jsval_t func_obj = mkobj(js, 0);
    if (is_err(func_obj)) return func_obj;
    
    set_func_code_ptr(js, func_obj, "(){}", 4);
    set_slot(js, func_obj, SLOT_SCOPE, js_glob(js));
    set_slot(js, func_obj, SLOT_ASYNC, js_true);
    
    jsval_t async_proto = get_slot(js, js_glob(js), SLOT_ASYNC_PROTO);
    if (vtype(async_proto) == T_FUNC) set_proto(js, func_obj, async_proto);
    
    jsval_t func = mkval(T_FUNC, (unsigned long)vdata(func_obj));
    jsval_t proto_setup = setup_func_prototype(js, func);
    
    if (is_err(proto_setup)) return proto_setup;
    return func;
  }
  
  size_t total_len = 1;
  
  for (int i = 0; i < nargs - 1; i++) {
    if (vtype(args[i]) != T_STR) {
      const char *str = js_str(js, args[i]);
      args[i] = js_mkstr(js, str, strlen(str));
      if (is_err(args[i])) return args[i];
    }
    total_len += vstrlen(js, args[i]);
    if (i < nargs - 2) total_len += 1;
  }
  
  total_len += 2;
  
  jsval_t body = args[nargs - 1];
  if (vtype(body) != T_STR) {
    const char *str = js_str(js, body);
    body = js_mkstr(js, str, strlen(str));
    if (is_err(body)) return body;
  }
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
  
  set_func_code(js, func_obj, code_buf, pos);
  free(code_buf);
  set_slot(js, func_obj, SLOT_SCOPE, js_glob(js));
  set_slot(js, func_obj, SLOT_ASYNC, js_true);
  
  jsval_t async_proto = get_slot(js, js_glob(js), SLOT_ASYNC_PROTO);
  if (vtype(async_proto) == T_FUNC) set_proto(js, func_obj, async_proto);
  
  jsval_t func = mkval(T_FUNC, (unsigned long) vdata(func_obj));
  jsval_t proto_setup = setup_func_prototype(js, func);
  
  if (is_err(proto_setup)) return proto_setup;
  return func;
}

static jsval_t builtin_function_empty(struct js *js, jsval_t *args, int nargs) {
  (void)js; (void)args; (void)nargs;
  return js_mkundef();
}

static jsval_t builtin_function_call(struct js *js, jsval_t *args, int nargs) {
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
  
  jsval_t saved_this = js->this_val;
  push_this(this_arg);
  js->this_val = this_arg;
  
  jsval_t result = call_js_with_args(js, func, call_args, call_nargs);
  
  pop_this();
  js->this_val = saved_this;
  
  return result;
}

static int extract_array_args(struct js *js, jsval_t arr, jsval_t **out_args) {
  jsoff_t len_off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  if (len_off == 0) return 0;
  
  jsval_t len_val = resolveprop(js, mkval(T_PROP, len_off));
  if (vtype(len_val) != T_NUM) return 0;
  
  int len = (int) tod(len_val);
  if (len <= 0) return 0;
  
  jsval_t *args_out = (jsval_t *)ant_calloc(sizeof(jsval_t) * len);
  if (!args_out) return 0;
  
  for (int i = 0; i < len; i++) {
    char idx[16];
    snprintf(idx, sizeof(idx), "%d", i);
    jsoff_t prop_off = lkp(js, arr, idx, strlen(idx));
    args_out[i] = (prop_off != 0) ? resolveprop(js, mkval(T_PROP, prop_off)) : js_mkundef();
  }
  
  *out_args = args_out;
  return len;
}

static jsval_t builtin_function_toString(struct js *js, jsval_t *args, int nargs) {
  jsval_t func = js->this_val;
  uint8_t t = vtype(func);
  
  if (t != T_FUNC && t != T_CFUNC) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Function.prototype.toString requires that 'this' be a Function");
  }
  
  if (t == T_CFUNC) return ANT_STRING("function() { [native code] }");
  
  jsval_t func_obj = mkval(T_OBJ, vdata(func));
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
        const char *brace = memchr(code, '{', code_len);
        if (!brace) goto fallback_arrow;
        
        size_t params_len = brace - code;
        const char *body_start = brace + 1;
        size_t body_len = code_len - params_len - 1;
        bool is_expr = (body_len >= 7 && memcmp(body_start, "return ", 7) == 0);
        
        size_t total = (is_async ? 6 : 0) + params_len + 4 + body_len + 2;
        char *buf = ant_calloc(total + 1);
        size_t n = 0;
        
        if (is_async) n += cpy(buf + n, total - n, "async ", 6);
        n += cpy(buf + n, total - n, code, params_len);
        n += cpy(buf + n, total - n, " => ", 4);
        
        if (is_expr) {
          const char *expr = body_start + 7;
          size_t expr_len = body_len - 7;
          while (expr_len > 0 && (expr[expr_len-1] == ' ' || expr[expr_len-1] == '}' || expr[expr_len-1] == ')')) expr_len--;
          n += cpy(buf + n, total - n, expr, expr_len);
        } else {
          size_t block_len = code_len - params_len;
          while (block_len > 0 && brace[block_len-1] == ')') block_len--;
          n += cpy(buf + n, total - n, brace, block_len);
        }
        
        jsval_t result = js_mkstr(js, buf, n);
        free(buf);
        return result;
        fallback_arrow:;
      }
      
      jsoff_t name_len = 0;
      const char *name = get_func_name(js, func, &name_len);
      size_t total = (is_async ? 6 : 0) + 9 + name_len + code_len + 1;
      char *buf = ant_calloc(total + 1);
      size_t n = 0;
      
      if (is_async) n += cpy(buf + n, total - n, "async ", 6);
      n += cpy(buf + n, total - n, "function ", 9);
      if (name && name_len > 0) n += cpy(buf + n, total - n, name, name_len);
      n += cpy(buf + n, total - n, code, code_len);
      n += cpy(buf + n, total - n, "}", 1);
      
      jsval_t result = js_mkstr(js, buf, n);
      free(buf);
      
      return result;
    }
  }
  
  char buf[256];
  size_t len = strfunc(js, func, buf, sizeof(buf));
  return js_mkstr(js, buf, len);
}

static jsval_t builtin_function_apply(struct js *js, jsval_t *args, int nargs) {
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
  
  jsval_t saved_this = js->this_val;
  push_this(this_arg);
  
  js->this_val = this_arg;
  jsval_t result = call_js_with_args(js, func, call_args, call_nargs);

  pop_this();
  js->this_val = saved_this;

  if (call_args) free(call_args);

  return result;
}

static jsval_t builtin_function_bind(struct js *js, jsval_t *args, int nargs) {
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
    target_func_obj = mkval(T_OBJ, vdata(func));
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
      for (int i = 0; i < bound_argc; i++) {
        char idx[16];
        snprintf(idx, sizeof(idx), "%d", i);
        setprop(js, bound_arr, js_mkstr(js, idx, strlen(idx)), bound_args[i]);
      }
      setprop(js, bound_arr, js_mkstr(js, "length", 6), tov((double) bound_argc));
      set_slot(js, bound_func, SLOT_BOUND_ARGS, bound_arr);
    }
    
    jsval_t func_proto = get_slot(js, js_glob(js), SLOT_FUNC_PROTO);
    if (vtype(func_proto) == T_FUNC) set_proto(js, bound_func, func_proto);
    
    jsval_t bound = mkval(T_FUNC, (unsigned long) vdata(bound_func));
    setprop(js, bound_func, js_mkstr(js, "length", 6), tov((double) bound_length));
    
    jsval_t proto_setup = setup_func_prototype(js, bound);
    if (is_err(proto_setup)) return proto_setup;
    
    return bound;
  }

  jsval_t func_obj = mkval(T_OBJ, vdata(func));
  jsval_t bound_func = mkobj(js, 0);
  if (is_err(bound_func)) return bound_func;

  jsval_t code_val = get_slot(js, func_obj, SLOT_CODE);
  if (vtype(code_val) == T_STR || vtype(code_val) == T_CFUNC) {
    set_slot(js, bound_func, SLOT_CODE, code_val);
    set_slot(js, bound_func, SLOT_CODE_LEN, get_slot(js, func_obj, SLOT_CODE_LEN));
  }

  jsval_t cfunc_slot = get_slot(js, func_obj, SLOT_CFUNC);
  if (vtype(cfunc_slot) == T_CFUNC) {
    set_slot(js, bound_func, SLOT_CFUNC, cfunc_slot);
  }

  jsval_t scope_slot = get_slot(js, func_obj, SLOT_SCOPE);
  if (vtype(scope_slot) != T_UNDEF) {
    set_slot(js, bound_func, SLOT_SCOPE, scope_slot);
  }

  jsval_t async_slot = get_slot(js, func_obj, SLOT_ASYNC);
  if (vtype(async_slot) == T_BOOL && vdata(async_slot) == 1) {
    set_slot(js, bound_func, SLOT_ASYNC, js_true);
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
    for (int i = 0; i < bound_argc; i++) {
      char idx[16];
      snprintf(idx, sizeof(idx), "%d", i);
      setprop(js, bound_arr, js_mkstr(js, idx, strlen(idx)), bound_args[i]);
    }
    setprop(js, bound_arr, js_mkstr(js, "length", 6), tov((double) bound_argc));
    set_slot(js, bound_func, SLOT_BOUND_ARGS, bound_arr);
  }
  
  setprop(js, bound_func, js_mkstr(js, "length", 6), tov((double) bound_length));
  
  jsval_t bound = mkval(T_FUNC, (unsigned long) vdata(bound_func));  
  jsval_t proto_setup = setup_func_prototype(js, bound);
  if (is_err(proto_setup)) return proto_setup;
  
  return bound;
}

static jsval_t builtin_Array(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = mkarr(js);
  
  if (nargs == 1 && vtype(args[0]) == T_NUM) {
    jsval_t err = validate_array_length(js, args[0]);
    if (is_err(err)) return err;
    setprop(js, arr, ANT_STRING("length"), tov(tod(args[0])));
  } else if (nargs > 0) {
    for (int i = 0; i < nargs; i++) {
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
      setprop(js, arr, js_mkstr(js, idxstr, idxlen), args[i]);
    }
    setprop(js, arr, ANT_STRING("length"), tov((double)nargs));
  }
  
  return arr;
}

static jsval_t builtin_Error(struct js *js, jsval_t *args, int nargs) {
  bool is_new = (vtype(js->new_target) != T_UNDEF);
  jsval_t this_val = js->this_val;
  
  jsval_t target = is_new ? js->new_target : js->current_func;
  jsval_t name = ANT_STRING("Error");
  
  if (vtype(target) == T_FUNC) {
    jsoff_t off = lkp(js, mkval(T_OBJ, vdata(target)), "name", 4);
    if (off) name = resolveprop(js, mkval(T_PROP, off));
  }

  if (!is_new) {
    this_val = js_mkobj(js);
    jsoff_t proto_off = lkp_interned(js, mkval(T_OBJ, vdata(js->current_func)), INTERN_PROTOTYPE, 9);
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
  
  js_mkprop_fast(js, this_val, "name", 4, name);
  return this_val;
}

static jsval_t builtin_AggregateError(struct js *js, jsval_t *args, int nargs) {
  bool is_new = (vtype(js->new_target) != T_UNDEF);
  jsval_t this_val = js->this_val;
  
  if (!is_new) {
    this_val = js_mkobj(js);
    jsoff_t proto_off = lkp_interned(js, mkval(T_OBJ, vdata(js->current_func)), INTERN_PROTOTYPE, 9);
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
  
  js_mkprop_fast(js, this_val, "name", 4, ANT_STRING("AggregateError"));
  return this_val;
}

static jsval_t builtin_RegExp(struct js *js, jsval_t *args, int nargs) {
  jsval_t regexp_obj = js->this_val;
  bool use_this = (vtype(regexp_obj) == T_OBJ);
  
  if (!use_this) {
    regexp_obj = mkobj(js, 0);
  }
  jsval_t regexp_proto = get_ctor_proto(js, "RegExp", 6);
  if (vtype(regexp_proto) == T_OBJ) set_proto(js, regexp_obj, regexp_proto);

  jsval_t pattern = js_mkstr(js, "", 0);
  if (nargs > 0) {
    if (vtype(args[0]) == T_STR) {
      pattern = args[0];
    } else {
      const char *str = js_str(js, args[0]);
      pattern = js_mkstr(js, str, strlen(str));
    }
  }
  
  jsval_t flags = js_mkstr(js, "", 0);
  if (nargs > 1 && vtype(args[1]) == T_STR) {
    flags = args[1];
  }
  
  jsval_t source_key = js_mkstr(js, "source", 6);
  setprop(js, regexp_obj, source_key, pattern);
  
  jsval_t flags_key = js_mkstr(js, "flags", 5);
  setprop(js, regexp_obj, flags_key, flags);
  
  jsoff_t flags_len, flags_off = vstr(js, flags, &flags_len);
  const char *flags_str = (char *) &js->mem[flags_off];
  
  bool global = false, ignoreCase = false, multiline = false, dotAll = false, sticky = false;
  for (jsoff_t i = 0; i < flags_len; i++) {
    if (flags_str[i] == 'g') global = true;
    if (flags_str[i] == 'i') ignoreCase = true;
    if (flags_str[i] == 'm') multiline = true;
    if (flags_str[i] == 's') dotAll = true;
    if (flags_str[i] == 'y') sticky = true;
  }

  setprop(js, regexp_obj, js_mkstr(js, "global", 6), mkval(T_BOOL, global ? 1 : 0));
  setprop(js, regexp_obj, js_mkstr(js, "ignoreCase", 10), mkval(T_BOOL, ignoreCase ? 1 : 0));
  setprop(js, regexp_obj, js_mkstr(js, "multiline", 9), mkval(T_BOOL, multiline ? 1 : 0));
  setprop(js, regexp_obj, js_mkstr(js, "dotAll", 6), mkval(T_BOOL, dotAll ? 1 : 0));
  setprop(js, regexp_obj, js_mkstr(js, "sticky", 6), mkval(T_BOOL, sticky ? 1 : 0));
  setprop(js, regexp_obj, js_mkstr(js, "lastIndex", 9), tov(0));

  return regexp_obj;
}

static jsval_t builtin_regexp_test(struct js *js, jsval_t *args, int nargs) {
  jsval_t regexp = js->this_val;
  if (vtype(regexp) != T_OBJ) return js_mkerr(js, "test called on non-regexp");
  if (nargs < 1) return mkval(T_BOOL, 0);

  jsval_t str_arg = args[0];
  if (vtype(str_arg) != T_STR) return mkval(T_BOOL, 0);

  jsoff_t source_off = lkp(js, regexp, "source", 6);
  if (source_off == 0) return mkval(T_BOOL, 0);
  jsval_t source_val = resolveprop(js, mkval(T_PROP, source_off));
  if (vtype(source_val) != T_STR) return mkval(T_BOOL, 0);

  jsoff_t plen, poff = vstr(js, source_val, &plen);
  const char *pattern_ptr = (char *) &js->mem[poff];

  bool ignore_case = false, multiline = false;
  jsoff_t flags_off = lkp(js, regexp, "flags", 5);
  if (flags_off != 0) {
    jsval_t flags_val = resolveprop(js, mkval(T_PROP, flags_off));
    if (vtype(flags_val) == T_STR) {
      jsoff_t flen, foff = vstr(js, flags_val, &flen);
      const char *flags_str = (char *) &js->mem[foff];
      for (jsoff_t i = 0; i < flen; i++) {
        if (flags_str[i] == 'i') ignore_case = true;
        if (flags_str[i] == 'm') multiline = true;
      }
    }
  }

  jsoff_t str_len, str_off = vstr(js, str_arg, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];

  char pcre2_pattern[512];
  size_t pcre2_len = js_to_pcre2_pattern(pattern_ptr, plen, pcre2_pattern, sizeof(pcre2_pattern));

  uint32_t options = PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_UNSET_BACKREF;
  if (ignore_case) options |= PCRE2_CASELESS;
  if (multiline) options |= PCRE2_MULTILINE;

  int errcode;
  PCRE2_SIZE erroffset;
  pcre2_code *re = pcre2_compile((PCRE2_SPTR)pcre2_pattern, pcre2_len, options, &errcode, &erroffset, NULL);
  if (re == NULL) return mkval(T_BOOL, 0);

  pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(re, NULL);
  int rc = pcre2_match(re, (PCRE2_SPTR)str_ptr, str_len, 0, 0, match_data, NULL);

  pcre2_match_data_free(match_data);
  pcre2_code_free(re);

  return mkval(T_BOOL, rc >= 0 ? 1 : 0);
}

static jsval_t builtin_regexp_exec(struct js *js, jsval_t *args, int nargs) {
  jsval_t regexp = js->this_val;
  if (vtype(regexp) != T_OBJ) return js_mkerr(js, "exec called on non-regexp");
  if (nargs < 1) return js_mknull();

  jsval_t str_arg = args[0];
  if (vtype(str_arg) != T_STR) return js_mknull();

  jsoff_t source_off = lkp(js, regexp, "source", 6);
  if (source_off == 0) return js_mknull();
  jsval_t source_val = resolveprop(js, mkval(T_PROP, source_off));
  if (vtype(source_val) != T_STR) return js_mknull();

  jsoff_t plen, poff = vstr(js, source_val, &plen);
  const char *pattern_ptr = (char *) &js->mem[poff];

  bool ignore_case = false, multiline = false, global_flag = false;
  jsoff_t flags_off = lkp(js, regexp, "flags", 5);
  if (flags_off != 0) {
    jsval_t flags_val = resolveprop(js, mkval(T_PROP, flags_off));
    if (vtype(flags_val) == T_STR) {
      jsoff_t flen, foff = vstr(js, flags_val, &flen);
      const char *flags_str = (char *) &js->mem[foff];
      for (jsoff_t i = 0; i < flen; i++) {
        if (flags_str[i] == 'i') ignore_case = true;
        if (flags_str[i] == 'm') multiline = true;
        if (flags_str[i] == 'g') global_flag = true;
      }
    }
  }

  jsoff_t str_len, str_off = vstr(js, str_arg, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];

  PCRE2_SIZE start_offset = 0;
  if (global_flag) {
    jsoff_t lastindex_off = lkp(js, regexp, "lastIndex", 9);
    if (lastindex_off != 0) {
      jsval_t li_val = resolveprop(js, mkval(T_PROP, lastindex_off));
      if (vtype(li_val) == T_NUM) {
        double li = tod(li_val);
        if (li >= 0 && li <= D(str_len)) start_offset = (PCRE2_SIZE)li;
      }
    }
  }

  char pcre2_pattern[512];
  size_t pcre2_len = js_to_pcre2_pattern(pattern_ptr, plen, pcre2_pattern, sizeof(pcre2_pattern));

  uint32_t options = PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_UNSET_BACKREF;
  if (ignore_case) options |= PCRE2_CASELESS;
  if (multiline) options |= PCRE2_MULTILINE;

  int errcode;
  PCRE2_SIZE erroffset;
  pcre2_code *re = pcre2_compile((PCRE2_SPTR)pcre2_pattern, pcre2_len, options, &errcode, &erroffset, NULL);
  if (re == NULL) return js_mknull();

  pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(re, NULL);
  int rc = pcre2_match(re, (PCRE2_SPTR)str_ptr, str_len, start_offset, 0, match_data, NULL);

  if (rc < 0) {
    pcre2_match_data_free(match_data);
    pcre2_code_free(re);
    if (global_flag) {
      setprop(js, regexp, js_mkstr(js, "lastIndex", 9), tov(0));
    }
    return js_mknull();
  }

  PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
  uint32_t ovcount = pcre2_get_ovector_count(match_data);

  jsval_t result_arr = js_mkarr(js);
  for (uint32_t i = 0; i < ovcount && i < 32; i++) {
    PCRE2_SIZE start = ovector[2*i];
    PCRE2_SIZE end = ovector[2*i+1];
    if (start == PCRE2_UNSET) {
      js_arr_push(js, result_arr, js_mkundef());
    } else {
      jsval_t match_str = js_mkstr(js, str_ptr + start, end - start);
      js_arr_push(js, result_arr, match_str);
    }
  }

  setprop(js, result_arr, js_mkstr(js, "index", 5), tov((double)ovector[0]));
  setprop(js, result_arr, js_mkstr(js, "input", 5), str_arg);

  if (global_flag) {
    PCRE2_SIZE new_lastindex = ovector[1];
    if (ovector[0] == ovector[1]) new_lastindex++;
    setprop(js, regexp, js_mkstr(js, "lastIndex", 9), tov((double)new_lastindex));
  }

  pcre2_match_data_free(match_data);
  pcre2_code_free(re);
  return result_arr;
}

static jsval_t builtin_regexp_toString(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t regexp = js->this_val;
  if (vtype(regexp) != T_OBJ) return js_mkerr(js, "toString called on non-regexp");

  jsoff_t source_off = lkp(js, regexp, "source", 6);
  if (source_off == 0) return js_mkstr(js, "/undefined/", 11);
  jsval_t source_val = resolveprop(js, mkval(T_PROP, source_off));
  if (vtype(source_val) != T_STR) return js_mkstr(js, "/undefined/", 11);

  jsoff_t src_len;
  jsoff_t src_off = vstr(js, source_val, &src_len);
  const char *src_ptr = (const char *)(js->mem + src_off);

  char flags[8] = {0};
  int fi = 0;
  jsoff_t prop_off;
  
  prop_off = lkp(js, regexp, "global", 6);
  if (prop_off && vdata(resolveprop(js, mkval(T_PROP, prop_off)))) flags[fi++] = 'g';
  
  prop_off = lkp(js, regexp, "ignoreCase", 10);
  if (prop_off && vdata(resolveprop(js, mkval(T_PROP, prop_off)))) flags[fi++] = 'i';
  
  prop_off = lkp(js, regexp, "multiline", 9);
  if (prop_off && vdata(resolveprop(js, mkval(T_PROP, prop_off)))) flags[fi++] = 'm';
  
  prop_off = lkp(js, regexp, "dotAll", 6);
  if (prop_off && vdata(resolveprop(js, mkval(T_PROP, prop_off)))) flags[fi++] = 's';
  
  prop_off = lkp(js, regexp, "sticky", 6);
  if (prop_off && vdata(resolveprop(js, mkval(T_PROP, prop_off)))) flags[fi++] = 'y';

  size_t result_len = 1 + src_len + 1 + fi;
  char *result = (char *)malloc(result_len + 1);
  if (!result) return js_mkerr(js, "out of memory");
  
  result[0] = '/';
  memcpy(result + 1, src_ptr, src_len);
  result[1 + src_len] = '/';
  memcpy(result + 2 + src_len, flags, fi);
  result[result_len] = '\0';
  
  jsval_t ret = js_mkstr(js, result, result_len);
  free(result);
  return ret;
}

static jsval_t builtin_string_search(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_unwrapped = unwrap_primitive(js, js->this_val);
  jsval_t str = js_tostring_val(js, this_unwrapped);
  if (is_err(str)) return str;
  if (nargs < 1) return tov(-1);

  jsval_t pattern = args[0];
  const char *pattern_ptr = NULL;
  jsoff_t pattern_len = 0;
  bool ignore_case = false, multiline = false;

  if (vtype(pattern) == T_OBJ) {
    jsoff_t source_off = lkp(js, pattern, "source", 6);
    if (source_off == 0) return tov(-1);
    jsval_t source_val = resolveprop(js, mkval(T_PROP, source_off));
    if (vtype(source_val) != T_STR) return tov(-1);

    jsoff_t poff;
    poff = vstr(js, source_val, &pattern_len);
    pattern_ptr = (char *) &js->mem[poff];

    jsoff_t flags_off = lkp(js, pattern, "flags", 5);
    if (flags_off != 0) {
      jsval_t flags_val = resolveprop(js, mkval(T_PROP, flags_off));
      if (vtype(flags_val) == T_STR) {
        jsoff_t flen, foff = vstr(js, flags_val, &flen);
        const char *flags_str = (char *) &js->mem[foff];
        for (jsoff_t i = 0; i < flen; i++) {
          if (flags_str[i] == 'i') ignore_case = true;
          if (flags_str[i] == 'm') multiline = true;
        }
      }
    }
  } else if (vtype(pattern) == T_STR) {
    jsoff_t poff;
    poff = vstr(js, pattern, &pattern_len);
    pattern_ptr = (char *) &js->mem[poff];
  } else {
    return tov(-1);
  }

  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];

  char pcre2_pattern[512];
  size_t pcre2_len = js_to_pcre2_pattern(pattern_ptr, pattern_len, pcre2_pattern, sizeof(pcre2_pattern));

  uint32_t options = PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_UNSET_BACKREF;
  if (ignore_case) options |= PCRE2_CASELESS;
  if (multiline) options |= PCRE2_MULTILINE;

  int errcode;
  PCRE2_SIZE erroffset;
  pcre2_code *re = pcre2_compile((PCRE2_SPTR)pcre2_pattern, pcre2_len, options, &errcode, &erroffset, NULL);
  if (re == NULL) return tov(-1);

  pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(re, NULL);
  int rc = pcre2_match(re, (PCRE2_SPTR)str_ptr, str_len, 0, 0, match_data, NULL);

  if (rc < 0) {
    pcre2_match_data_free(match_data);
    pcre2_code_free(re);
    return tov(-1);
  }

  PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
  double result = (double)ovector[0];

  pcre2_match_data_free(match_data);
  pcre2_code_free(re);

  return tov(result);
}

static jsval_t builtin_Date(struct js *js, jsval_t *args, int nargs) {
  jsval_t date_obj = js->this_val;
  
  if (vtype(js->new_target) == T_UNDEF) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    
    time_t t = tv.tv_sec;
    struct tm *tm = localtime(&t);
    
    static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buf[64];
    
    snprintf(
      buf, sizeof(buf), "%s %s %02d %04d %02d:%02d:%02d GMT%+03ld%02ld",
      days[tm->tm_wday], months[tm->tm_mon], tm->tm_mday, tm->tm_year + 1900,
      tm->tm_hour, tm->tm_min, tm->tm_sec, -timezone/3600, (labs(timezone)/60)%60
    );
    
    return js_mkstr(js, buf, strlen(buf));
  }
  
  double timestamp_ms;
  
  if (nargs == 0) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    // NOLINTNEXTLINE(bugprone-integer-division)
    timestamp_ms = (double)tv.tv_sec * 1000.0 + (double)(tv.tv_usec / 1000);
  } else if (nargs == 1) {
    if (vtype(args[0]) == T_NUM) {
      timestamp_ms = tod(args[0]);
    } else if (vtype(args[0]) == T_STR) {
      timestamp_ms = 0;
    } else timestamp_ms = 0;
  } else {
    int year = (int)tod(args[0]);
    int month = nargs >= 2 ? (int)tod(args[1]) : 0;
    int day = nargs >= 3 ? (int)tod(args[2]) : 1;
    int hour = nargs >= 4 ? (int)tod(args[3]) : 0;
    int minute = nargs >= 5 ? (int)tod(args[4]) : 0;
    int sec = nargs >= 6 ? (int)tod(args[5]) : 0;
    int ms = nargs >= 7 ? (int)tod(args[6]) : 0;
    if (year >= 0 && year <= 99) year += 1900;
    struct tm tm_val = {0};
    tm_val.tm_year = year - 1900;
    tm_val.tm_mon = month;
    tm_val.tm_mday = day;
    tm_val.tm_hour = hour;
    tm_val.tm_min = minute;
    tm_val.tm_sec = sec;
    tm_val.tm_isdst = -1;
    time_t t = mktime(&tm_val);
    timestamp_ms = (double)t * 1000.0 + ms;
  }
  
  js_set_slot(js, date_obj, SLOT_DATA, tov(timestamp_ms));
  
  return date_obj;
}

static jsval_t builtin_Date_now(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  
  struct timeval tv;
  gettimeofday(&tv, NULL);
  // NOLINTNEXTLINE(bugprone-integer-division)
  double timestamp_ms = (double)tv.tv_sec * 1000.0 + (double)(tv.tv_usec / 1000);
  
  return tov(timestamp_ms);
}

static jsval_t builtin_Date_UTC(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1) return tov(JS_NAN);
  
  int year = (int)tod(args[0]);
  int month = nargs >= 2 ? (int)tod(args[1]) : 0;
  int day = nargs >= 3 ? (int)tod(args[2]) : 1;
  int hour = nargs >= 4 ? (int)tod(args[3]) : 0;
  int min = nargs >= 5 ? (int)tod(args[4]) : 0;
  int sec = nargs >= 6 ? (int)tod(args[5]) : 0;
  int ms = nargs >= 7 ? (int)tod(args[6]) : 0;
  
  if (year >= 0 && year <= 99) year += 1900;
  
  struct tm tm = {0};
  tm.tm_year = year - 1900;
  tm.tm_mon = month;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = min;
  tm.tm_sec = sec;
  
  time_t t = timegm(&tm);
  return tov((double)t * 1000.0 + ms);
}

static double date_get_time(struct js *js, jsval_t this_val) {
  jsval_t time_val = js_get_slot(js, this_val, SLOT_DATA);
  if (vtype(time_val) != T_NUM) return JS_NAN;
  return tod(time_val);
}

static jsval_t builtin_Date_getTime(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  return tov(date_get_time(js, js->this_val));
}

static jsval_t builtin_Date_getFullYear(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return tov(JS_NAN);
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = localtime(&t);
  return tov((double)(tm->tm_year + 1900));
}

static jsval_t builtin_Date_getMonth(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return tov(JS_NAN);
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = localtime(&t);
  return tov((double)tm->tm_mon);
}

static jsval_t builtin_Date_getDate(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return tov(JS_NAN);
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = localtime(&t);
  return tov((double)tm->tm_mday);
}

static jsval_t builtin_Date_getHours(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return tov(JS_NAN);
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = localtime(&t);
  return tov((double)tm->tm_hour);
}

static jsval_t builtin_Date_getMinutes(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return tov(JS_NAN);
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = localtime(&t);
  return tov((double)tm->tm_min);
}

static jsval_t builtin_Date_getSeconds(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return tov(JS_NAN);
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = localtime(&t);
  return tov((double)tm->tm_sec);
}

static jsval_t builtin_Date_getMilliseconds(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return tov(JS_NAN);
  return tov(fmod(ms, 1000.0));
}

static jsval_t builtin_Date_getDay(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return tov(JS_NAN);
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = localtime(&t);
  return tov((double)tm->tm_wday);
}

static jsval_t builtin_Date_toISOString(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid time value");
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = gmtime(&t);
  int millis = (int)fmod(ms, 1000.0);
  if (millis < 0) millis += 1000;
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
           tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
           tm->tm_hour, tm->tm_min, tm->tm_sec, millis);
  return js_mkstr(js, buf, strlen(buf));
}

static jsval_t builtin_Date_toString(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return js_mkstr(js, "Invalid Date", 12);
  time_t t = (time_t)(ms / 1000.0);
  char *s = ctime(&t);
  size_t len = strlen(s);
  if (len > 0 && s[len - 1] == '\n') len--;
  return js_mkstr(js, s, len);
}

static jsval_t builtin_Date_valueOf(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  return tov(date_get_time(js, js->this_val));
}

static jsval_t builtin_Date_getTimezoneOffset(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return tov(JS_NAN);
  time_t t = (time_t)(ms / 1000.0);
  struct tm *local = localtime(&t);
  struct tm *utc = gmtime(&t);
  int diff = (local->tm_hour - utc->tm_hour) * 60 + (local->tm_min - utc->tm_min);
  if (local->tm_mday != utc->tm_mday) {
    diff += (local->tm_mday > utc->tm_mday || (local->tm_mday == 1 && utc->tm_mday > 1)) ? 1440 : -1440;
  }
  return tov((double)(-diff));
}

static jsval_t builtin_Date_getUTCFullYear(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return tov(JS_NAN);
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = gmtime(&t);
  return tov((double)(tm->tm_year + 1900));
}

static jsval_t builtin_Date_getUTCMonth(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return tov(JS_NAN);
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = gmtime(&t);
  return tov((double)tm->tm_mon);
}

static jsval_t builtin_Date_getUTCDate(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return tov(JS_NAN);
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = gmtime(&t);
  return tov((double)tm->tm_mday);
}

static jsval_t builtin_Date_getUTCHours(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return tov(JS_NAN);
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = gmtime(&t);
  return tov((double)tm->tm_hour);
}

static jsval_t builtin_Date_getUTCMinutes(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return tov(JS_NAN);
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = gmtime(&t);
  return tov((double)tm->tm_min);
}

static jsval_t builtin_Date_getUTCSeconds(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return tov(JS_NAN);
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = gmtime(&t);
  return tov((double)tm->tm_sec);
}

static jsval_t builtin_Date_getUTCMilliseconds(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return tov(JS_NAN);
  return tov(fmod(ms, 1000.0));
}

static jsval_t builtin_Date_getUTCDay(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return tov(JS_NAN);
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = gmtime(&t);
  return tov((double)tm->tm_wday);
}

static void date_set_time(struct js *js, jsval_t date, double ms) {
  if (vtype(date) != T_OBJ) return;
  js_set_slot(js, date, SLOT_DATA, tov(ms));
}

static jsval_t builtin_Date_setTime(struct js *js, jsval_t *args, int nargs) {
  double ms = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  date_set_time(js, js->this_val, ms);
  return tov(ms);
}

static jsval_t builtin_Date_setMilliseconds(struct js *js, jsval_t *args, int nargs) {
  double ms = date_get_time(js, js->this_val);
  double newMs = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(ms) || isnan(newMs)) { date_set_time(js, js->this_val, JS_NAN); return tov(JS_NAN); }
  ms = floor(ms / 1000.0) * 1000.0 + newMs;
  date_set_time(js, js->this_val, ms);
  return tov(ms);
}

static jsval_t builtin_Date_setSeconds(struct js *js, jsval_t *args, int nargs) {
  double ms = date_get_time(js, js->this_val);
  double sec = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(ms) || isnan(sec)) { date_set_time(js, js->this_val, JS_NAN); return tov(JS_NAN); }
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = localtime(&t);
  tm->tm_sec = (int)sec;
  if (nargs >= 2) ms = floor(ms / 1000.0) * 1000.0 + tod(args[1]);
  else ms = floor(ms / 1000.0) * 1000.0 + fmod(ms, 1000.0);
  time_t newt = mktime(tm);
  ms = (double)newt * 1000.0 + fmod(ms, 1000.0);
  date_set_time(js, js->this_val, ms);
  return tov(ms);
}

static jsval_t builtin_Date_setMinutes(struct js *js, jsval_t *args, int nargs) {
  double ms = date_get_time(js, js->this_val);
  double min = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(ms) || isnan(min)) { date_set_time(js, js->this_val, JS_NAN); return tov(JS_NAN); }
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = localtime(&t);
  tm->tm_min = (int)min;
  if (nargs >= 2) tm->tm_sec = (int)tod(args[1]);
  time_t newt = mktime(tm);
  ms = (double)newt * 1000.0 + fmod(ms, 1000.0);
  date_set_time(js, js->this_val, ms);
  return tov(ms);
}

static jsval_t builtin_Date_setHours(struct js *js, jsval_t *args, int nargs) {
  double ms = date_get_time(js, js->this_val);
  double hour = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(ms) || isnan(hour)) { date_set_time(js, js->this_val, JS_NAN); return tov(JS_NAN); }
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = localtime(&t);
  tm->tm_hour = (int)hour;
  if (nargs >= 2) tm->tm_min = (int)tod(args[1]);
  if (nargs >= 3) tm->tm_sec = (int)tod(args[2]);
  time_t newt = mktime(tm);
  ms = (double)newt * 1000.0 + fmod(ms, 1000.0);
  date_set_time(js, js->this_val, ms);
  return tov(ms);
}

static jsval_t builtin_Date_setDate(struct js *js, jsval_t *args, int nargs) {
  double ms = date_get_time(js, js->this_val);
  double day = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(ms) || isnan(day)) { date_set_time(js, js->this_val, JS_NAN); return tov(JS_NAN); }
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = localtime(&t);
  tm->tm_mday = (int)day;
  time_t newt = mktime(tm);
  ms = (double)newt * 1000.0 + fmod(ms, 1000.0);
  date_set_time(js, js->this_val, ms);
  return tov(ms);
}

static jsval_t builtin_Date_setMonth(struct js *js, jsval_t *args, int nargs) {
  double ms = date_get_time(js, js->this_val);
  double mon = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(ms) || isnan(mon)) { date_set_time(js, js->this_val, JS_NAN); return tov(JS_NAN); }
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = localtime(&t);
  tm->tm_mon = (int)mon;
  if (nargs >= 2) tm->tm_mday = (int)tod(args[1]);
  time_t newt = mktime(tm);
  ms = (double)newt * 1000.0 + fmod(ms, 1000.0);
  date_set_time(js, js->this_val, ms);
  return tov(ms);
}

static jsval_t builtin_Date_setFullYear(struct js *js, jsval_t *args, int nargs) {
  double ms = date_get_time(js, js->this_val);
  double year = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(year)) { date_set_time(js, js->this_val, JS_NAN); return tov(JS_NAN); }
  if (isnan(ms)) ms = 0;
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = localtime(&t);
  tm->tm_year = (int)year - 1900;
  if (nargs >= 2) tm->tm_mon = (int)tod(args[1]);
  if (nargs >= 3) tm->tm_mday = (int)tod(args[2]);
  time_t newt = mktime(tm);
  ms = (double)newt * 1000.0 + fmod(ms, 1000.0);
  date_set_time(js, js->this_val, ms);
  return tov(ms);
}

static jsval_t builtin_Date_setUTCMilliseconds(struct js *js, jsval_t *args, int nargs) {
  double ms = date_get_time(js, js->this_val);
  double newMs = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(ms) || isnan(newMs)) { date_set_time(js, js->this_val, JS_NAN); return tov(JS_NAN); }
  ms = floor(ms / 1000.0) * 1000.0 + newMs;
  date_set_time(js, js->this_val, ms);
  return tov(ms);
}

static jsval_t builtin_Date_setUTCSeconds(struct js *js, jsval_t *args, int nargs) {
  double ms = date_get_time(js, js->this_val);
  double sec = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(ms) || isnan(sec)) { date_set_time(js, js->this_val, JS_NAN); return tov(JS_NAN); }
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = gmtime(&t);
  struct tm copy = *tm;
  copy.tm_sec = (int)sec;
  time_t newt = timegm(&copy);
  ms = (double)newt * 1000.0 + fmod(ms, 1000.0);
  date_set_time(js, js->this_val, ms);
  return tov(ms);
}

static jsval_t builtin_Date_setUTCMinutes(struct js *js, jsval_t *args, int nargs) {
  double ms = date_get_time(js, js->this_val);
  double min = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(ms) || isnan(min)) { date_set_time(js, js->this_val, JS_NAN); return tov(JS_NAN); }
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = gmtime(&t);
  struct tm copy = *tm;
  copy.tm_min = (int)min;
  if (nargs >= 2) copy.tm_sec = (int)tod(args[1]);
  time_t newt = timegm(&copy);
  ms = (double)newt * 1000.0 + fmod(ms, 1000.0);
  date_set_time(js, js->this_val, ms);
  return tov(ms);
}

static jsval_t builtin_Date_setUTCHours(struct js *js, jsval_t *args, int nargs) {
  double ms = date_get_time(js, js->this_val);
  double hour = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(ms) || isnan(hour)) { date_set_time(js, js->this_val, JS_NAN); return tov(JS_NAN); }
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = gmtime(&t);
  struct tm copy = *tm;
  copy.tm_hour = (int)hour;
  if (nargs >= 2) copy.tm_min = (int)tod(args[1]);
  if (nargs >= 3) copy.tm_sec = (int)tod(args[2]);
  time_t newt = timegm(&copy);
  ms = (double)newt * 1000.0 + fmod(ms, 1000.0);
  date_set_time(js, js->this_val, ms);
  return tov(ms);
}

static jsval_t builtin_Date_setUTCDate(struct js *js, jsval_t *args, int nargs) {
  double ms = date_get_time(js, js->this_val);
  double day = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(ms) || isnan(day)) { date_set_time(js, js->this_val, JS_NAN); return tov(JS_NAN); }
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = gmtime(&t);
  struct tm copy = *tm;
  copy.tm_mday = (int)day;
  time_t newt = timegm(&copy);
  ms = (double)newt * 1000.0 + fmod(ms, 1000.0);
  date_set_time(js, js->this_val, ms);
  return tov(ms);
}

static jsval_t builtin_Date_setUTCMonth(struct js *js, jsval_t *args, int nargs) {
  double ms = date_get_time(js, js->this_val);
  double mon = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(ms) || isnan(mon)) { date_set_time(js, js->this_val, JS_NAN); return tov(JS_NAN); }
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = gmtime(&t);
  struct tm copy = *tm;
  copy.tm_mon = (int)mon;
  if (nargs >= 2) copy.tm_mday = (int)tod(args[1]);
  time_t newt = timegm(&copy);
  ms = (double)newt * 1000.0 + fmod(ms, 1000.0);
  date_set_time(js, js->this_val, ms);
  return tov(ms);
}

static jsval_t builtin_Date_setUTCFullYear(struct js *js, jsval_t *args, int nargs) {
  double ms = date_get_time(js, js->this_val);
  double year = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(year)) { date_set_time(js, js->this_val, JS_NAN); return tov(JS_NAN); }
  if (isnan(ms)) ms = 0;
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = gmtime(&t);
  struct tm copy = *tm;
  copy.tm_year = (int)year - 1900;
  if (nargs >= 2) copy.tm_mon = (int)tod(args[1]);
  if (nargs >= 3) copy.tm_mday = (int)tod(args[2]);
  time_t newt = timegm(&copy);
  ms = (double)newt * 1000.0 + fmod(ms, 1000.0);
  date_set_time(js, js->this_val, ms);
  return tov(ms);
}

static jsval_t builtin_Date_toUTCString(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return js_mkstr(js, "Invalid Date", 12);
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = gmtime(&t);
  static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char buf[64];
  snprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d GMT",
           days[tm->tm_wday], tm->tm_mday, months[tm->tm_mon],
           tm->tm_year + 1900, tm->tm_hour, tm->tm_min, tm->tm_sec);
  return js_mkstr(js, buf, strlen(buf));
}

static jsval_t builtin_Date_toDateString(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return js_mkstr(js, "Invalid Date", 12);
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = localtime(&t);
  static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char buf[32];
  snprintf(buf, sizeof(buf), "%s %s %02d %04d",
           days[tm->tm_wday], months[tm->tm_mon], tm->tm_mday, tm->tm_year + 1900);
  return js_mkstr(js, buf, strlen(buf));
}

static jsval_t builtin_Date_toTimeString(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return js_mkstr(js, "Invalid Date", 12);
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = localtime(&t);
  char buf[32];
  int offset = (int)(-timezone / 60);
  int offset_hours = offset / 60;
  int offset_mins = abs(offset % 60);
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d GMT%+03d%02d",
           tm->tm_hour, tm->tm_min, tm->tm_sec, offset_hours, offset_mins);
  return js_mkstr(js, buf, strlen(buf));
}

static jsval_t builtin_Date_toLocaleDateString(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return js_mkstr(js, "Invalid Date", 12);
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = localtime(&t);
  char buf[32];
  snprintf(buf, sizeof(buf), "%d/%d/%04d", tm->tm_mon + 1, tm->tm_mday, tm->tm_year + 1900);
  return js_mkstr(js, buf, strlen(buf));
}

static jsval_t builtin_Date_toLocaleTimeString(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return js_mkstr(js, "Invalid Date", 12);
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = localtime(&t);
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
  return js_mkstr(js, buf, strlen(buf));
}

static jsval_t builtin_Date_getYear(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return tov(JS_NAN);
  time_t t = (time_t)(ms / 1000.0);
  struct tm *tm = localtime(&t);
  return tov((double)(tm->tm_year));
}

static jsval_t builtin_Date_setYear(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    date_set_time(js, js->this_val, JS_NAN);
    return tov(JS_NAN);
  }
  double year_arg = tod(args[0]);
  if (isnan(year_arg)) {
    date_set_time(js, js->this_val, JS_NAN);
    return tov(JS_NAN);
  }
  int year = (int)year_arg;
  if (year >= 0 && year <= 99) year += 1900;
  double ms = date_get_time(js, js->this_val);
  time_t t;
  struct tm tm_val;
  if (isnan(ms)) {
    t = 0;
    tm_val = *localtime(&t);
    tm_val.tm_mday = 1;
    tm_val.tm_mon = 0;
    tm_val.tm_hour = 0;
    tm_val.tm_min = 0;
    tm_val.tm_sec = 0;
  } else {
    t = (time_t)(ms / 1000.0);
    tm_val = *localtime(&t);
  }
  tm_val.tm_year = year - 1900;
  time_t new_t = mktime(&tm_val);
  double new_ms = (double)new_t * 1000.0;
  date_set_time(js, js->this_val, new_ms);
  return tov(new_ms);
}

static jsval_t builtin_Date_toJSON(struct js *js, jsval_t *args, int nargs) {
  double ms = date_get_time(js, js->this_val);
  if (isnan(ms)) return js_mknull();
  return builtin_Date_toISOString(js, args, nargs);
}

static jsval_t builtin_Math_abs(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(fabs(x));
}

static jsval_t builtin_Math_acos(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(acos(x));
}

static jsval_t builtin_Math_acosh(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(acosh(x));
}

static jsval_t builtin_Math_asin(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(asin(x));
}

static jsval_t builtin_Math_asinh(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(asinh(x));
}

static jsval_t builtin_Math_atan(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(atan(x));
}

static jsval_t builtin_Math_atanh(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(atanh(x));
}

static jsval_t builtin_Math_atan2(struct js *js, jsval_t *args, int nargs) {
  double y = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  double x = (nargs < 2) ? JS_NAN : js_to_number(js, args[1]);
  if (isnan(y) || isnan(x)) return tov(JS_NAN);
  return tov(atan2(y, x));
}

static jsval_t builtin_Math_cbrt(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(cbrt(x));
}

static jsval_t builtin_Math_ceil(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(ceil(x));
}

static jsval_t builtin_Math_clz32(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return tov(32);
  double x = js_to_number(js, args[0]);
  if (isnan(x) || isinf(x)) return tov(32);
  uint32_t n = (uint32_t) x;
  if (n == 0) return tov(32);
  int count = 0;
  while ((n & 0x80000000U) == 0) { count++; n <<= 1; }
  return tov((double) count);
}

static jsval_t builtin_Math_cos(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(cos(x));
}

static jsval_t builtin_Math_cosh(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(cosh(x));
}

static jsval_t builtin_Math_exp(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(exp(x));
}

static jsval_t builtin_Math_expm1(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(expm1(x));
}

static jsval_t builtin_Math_floor(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(floor(x));
}

static jsval_t builtin_Math_fround(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov((double)(float)x);
}

static jsval_t builtin_Math_hypot(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t builtin_Math_imul(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return tov(0);
  int32_t a = toInt32(js_to_number(js, args[0]));
  int32_t b = toInt32(js_to_number(js, args[1]));
  return tov((double)((int32_t)((uint32_t)a * (uint32_t)b)));
}

static jsval_t builtin_Math_log(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(log(x));
}

static jsval_t builtin_Math_log1p(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(log1p(x));
}

static jsval_t builtin_Math_log10(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(log10(x));
}

static jsval_t builtin_Math_log2(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(log2(x));
}

static jsval_t builtin_Math_max(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return tov(JS_NEG_INF);
  double max_val = JS_NEG_INF;
  for (int i = 0; i < nargs; i++) {
    double v = js_to_number(js, args[i]);
    if (isnan(v)) return tov(JS_NAN);
    if (v > max_val) max_val = v;
  }
  return tov(max_val);
}

static jsval_t builtin_Math_min(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return tov(JS_INF);
  double min_val = JS_INF;
  for (int i = 0; i < nargs; i++) {
    double v = js_to_number(js, args[i]);
    if (isnan(v)) return tov(JS_NAN);
    if (v < min_val) min_val = v;
  }
  return tov(min_val);
}

static jsval_t builtin_Math_pow(struct js *js, jsval_t *args, int nargs) {
  double base = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  double exp = (nargs < 2) ? JS_NAN : js_to_number(js, args[1]);
  if (isnan(base) || isnan(exp)) return tov(JS_NAN);
  return tov(pow(base, exp));
}

static bool random_seeded = false;

static jsval_t builtin_Math_random(struct js *js, jsval_t *args, int nargs) {
  if (!random_seeded) {
    srand((unsigned int) time(NULL));
    random_seeded = true;
  }
  return tov((double) rand() / ((double) RAND_MAX + 1.0));
}

static jsval_t builtin_Math_round(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x) || isinf(x)) return tov(x);
  return tov(floor(x + 0.5));
}

static jsval_t builtin_Math_sign(struct js *js, jsval_t *args, int nargs) {
  double v = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(v)) return tov(JS_NAN);
  if (v > 0) return tov(1.0);
  if (v < 0) return tov(-1.0);
  return tov(v);
}

static jsval_t builtin_Math_sin(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(sin(x));
}

static jsval_t builtin_Math_sinh(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(sinh(x));
}

static jsval_t builtin_Math_sqrt(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(sqrt(x));
}

static jsval_t builtin_Math_tan(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(tan(x));
}

static jsval_t builtin_Math_tanh(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(tanh(x));
}

static jsval_t builtin_Math_trunc(struct js *js, jsval_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(trunc(x));
}

static jsval_t builtin_object_keys(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkarr(js);
  jsval_t obj = args[0];
  
  if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR && vtype(obj) != T_FUNC) return mkarr(js);
  if (vtype(obj) == T_FUNC) obj = mkval(T_OBJ, vdata(obj));
  
  jsval_t arr = mkarr(js);
  jsoff_t idx = 0;
  
  jsoff_t obj_off = (jsoff_t)vdata(obj);
  jsoff_t next = loadoff(js, obj_off) & ~(3U | FLAGMASK);
  
  while (next < js->brk && next != 0) {
    jsoff_t header = loadoff(js, next);
    if (is_slot_prop(header)) { next = next_prop(header); continue; }
    
    jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
    jsoff_t klen = offtolen(loadoff(js, koff));
    const char *key = (char *) &js->mem[koff + sizeof(koff)];
    
    next = next_prop(header);
    if (is_internal_prop(key, klen)) continue;
    
    bool should_include = true;
    descriptor_entry_t *desc = lookup_descriptor(obj_off, key, klen);
    if (desc) should_include = desc->enumerable;
    
    if (should_include) {
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
      jsval_t key_val = js_mkstr(js, key, klen);
      js_mkprop_fast(js, arr, idxstr, idxlen, key_val);
      idx++;
    }
  }
  
  descriptor_entry_t *desc, *tmp;
  HASH_ITER(hh, desc_registry, desc, tmp) {
    if (desc->obj_off != obj_off) continue;
    if (!desc->enumerable) continue;
    if (!desc->has_getter && !desc->has_setter) continue;
    
    char idxstr[16]; size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
    jsval_t key_val = js_mkstr(js, desc->prop_name, desc->prop_len);
    js_mkprop_fast(js, arr, idxstr, idxlen, key_val); idx++;
  }
    
  jsval_t len_val = tov((double) idx);
  js_mkprop_fast(js, arr, "length", 6, len_val);
  
  return mkval(T_ARR, vdata(arr));
}

static jsval_t builtin_object_values(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkarr(js);
  jsval_t obj = args[0];
  
  if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR && vtype(obj) != T_FUNC) return mkarr(js);
  if (vtype(obj) == T_FUNC) obj = mkval(T_OBJ, vdata(obj));
  
  jsval_t arr = mkarr(js);
  jsoff_t idx = 0;
  jsoff_t next = loadoff(js, (jsoff_t) vdata(obj)) & ~(3U | FLAGMASK);
  
  while (next < js->brk && next != 0) {
    jsoff_t header = loadoff(js, next);
    if (is_slot_prop(header)) { next = next_prop(header); continue; }
    
    jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
    jsoff_t klen = offtolen(loadoff(js, koff));
    const char *key = (char *) &js->mem[koff + sizeof(koff)];
    jsval_t val = loadval(js, next + (jsoff_t) (sizeof(next) + sizeof(koff)));
    
    next = next_prop(header);
    if (is_internal_prop(key, klen)) continue;
    
    bool should_include = true;
    jsoff_t obj_off = (jsoff_t)vdata(obj);
    descriptor_entry_t *desc = lookup_descriptor(obj_off, key, klen);
    if (desc) {
      should_include = desc->enumerable;
    }
    
    if (should_include) {
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
      js_mkprop_fast(js, arr, idxstr, idxlen, val);
      idx++;
    }
  }
  
  jsval_t len_val = tov((double) idx);
  js_mkprop_fast(js, arr, "length", 6, len_val);
  
  return mkval(T_ARR, vdata(arr));
}

static jsval_t builtin_object_entries(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkarr(js);
  jsval_t obj = args[0];
  
  if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR && vtype(obj) != T_FUNC) return mkarr(js);
  if (vtype(obj) == T_FUNC) obj = mkval(T_OBJ, vdata(obj));
  
  jsval_t arr = mkarr(js);
  jsoff_t idx = 0;
  jsoff_t next = loadoff(js, (jsoff_t) vdata(obj)) & ~(3U | FLAGMASK);
  
  while (next < js->brk && next != 0) {
    jsoff_t header = loadoff(js, next);
    if (is_slot_prop(header)) { next = next_prop(header); continue; }
    
    jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
    jsoff_t klen = offtolen(loadoff(js, koff));
    const char *key = (char *) &js->mem[koff + sizeof(koff)];
    jsval_t val = loadval(js, next + (jsoff_t) (sizeof(next) + sizeof(koff)));
    
    next = next_prop(header);
    
    if (is_internal_prop(key, klen)) continue;
    
    bool should_include = true;
    jsoff_t obj_off = (jsoff_t)vdata(obj);
    descriptor_entry_t *desc = lookup_descriptor(obj_off, key, klen);
    if (desc) {
      should_include = desc->enumerable;
    }
    
    if (should_include) {
      jsval_t pair = mkarr(js);
      jsval_t key_val = js_mkstr(js, key, klen);
      js_mkprop_fast(js, pair, "0", 1, key_val);
      js_mkprop_fast(js, pair, "1", 1, val);
      js_mkprop_fast(js, pair, "length", 6, tov(2.0));
      
      char idxstr[16]; size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
      js_mkprop_fast(js, arr, idxstr, idxlen, mkval(T_ARR, vdata(pair))); idx++;
    }
  }
  
  jsval_t len_val = tov((double) idx);
  js_mkprop_fast(js, arr, "length", 6, len_val);
  
  return mkval(T_ARR, vdata(arr));
}

static jsval_t builtin_object_getPrototypeOf(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_mkerr(js, "Object.getPrototypeOf requires an argument");
  jsval_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t == T_STR || t == T_NUM || t == T_BOOL || t == T_BIGINT) return get_prototype_for_type(js, t);
  if (t == T_OBJ || t == T_ARR || t == T_FUNC) return get_proto(js, obj);
  
  return js_mknull();
}

static jsval_t builtin_object_setPrototypeOf(struct js *js, jsval_t *args, int nargs) {
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
    if (vdata(cur) == vdata(obj)) return js_mkerr(js, "Cyclic __proto__ value");
  }
  
  set_proto(js, obj, proto);
  return obj;
}

static jsval_t builtin_proto_getter(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t builtin_proto_setter(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t builtin_object_create(struct js *js, jsval_t *args, int nargs) {
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
          setprop(js, obj, key_str, val);
        }
      }
      
      next = next_prop(header);
    }
  }
  
  return obj;
}

static jsval_t builtin_object_hasOwn(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return mkval(T_BOOL, 0);
  
  jsval_t obj = args[0];
  jsval_t key = args[1];
  
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return mkval(T_BOOL, 0);
  if (vtype(key) != T_STR) return mkval(T_BOOL, 0);
  
  jsval_t as_obj = (t == T_OBJ) ? obj : mkval(T_OBJ, vdata(obj));
  jsoff_t key_len, key_off = vstr(js, key, &key_len);
  const char *key_str = (char *) &js->mem[key_off];
  
  jsoff_t off = lkp(js, as_obj, key_str, key_len);
  return mkval(T_BOOL, off != 0 ? 1 : 0);
}

static jsval_t builtin_object_defineProperty(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 3) return js_mkerr(js, "Object.defineProperty requires 3 arguments");
  
  jsval_t obj = args[0];
  jsval_t prop = args[1];
  jsval_t descriptor = args[2];
  
  uint8_t t = vtype(obj);
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) {
    return js_mkerr(js, "Object.defineProperty called on non-object");
  }
  
  if (vtype(prop) == T_SYMBOL) {
    char keybuf[64];
    snprintf(keybuf, sizeof(keybuf), "__sym_%llu__", (unsigned long long)sym_get_id(prop));
    prop = js_mkstr(js, keybuf, strlen(keybuf));
  } else if (vtype(prop) != T_STR) {
    char buf[64];
    size_t len = tostr(js, prop, buf, sizeof(buf));
    prop = js_mkstr(js, buf, len);
  }
  
  if (vtype(descriptor) != T_OBJ) {
    return js_mkerr(js, "Property descriptor must be an object");
  }
  
  jsval_t as_obj = (t == T_OBJ) ? obj : mkval(T_OBJ, vdata(obj));
  
  jsoff_t prop_len, prop_off = vstr(js, prop, &prop_len);
  const char *prop_str = (char *) &js->mem[prop_off];
  
  if (streq(prop_str, prop_len, STR_PROTO, STR_PROTO_LEN)) {
    return js_mkerr(js, "Cannot define " STR_PROTO " property");
  }
  
  bool has_value = false, has_get = false, has_set = false, has_writable = false;
  jsval_t value = js_mkundef();
  bool writable = true, enumerable = false, configurable = false;
  
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
  
  jsoff_t existing_off = lkp(js, as_obj, prop_str, prop_len);
  
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
  } else {
    int desc_flags = 
      (writable ? JS_DESC_W : 0) | 
      (enumerable ? JS_DESC_E : 0) | 
      (configurable ? JS_DESC_C : 0);
      
    js_set_descriptor(js, as_obj, prop_str, prop_len, desc_flags);
    
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
      jsval_t prop_key = js_mkstr(js, prop_str, prop_len);
      jsoff_t flags = (writable ? 0 : CONSTMASK) | (configurable ? 0 : NONCONFIGMASK);
      mkprop(js, as_obj, prop_key, value, flags);
    }
  }
  
  return obj;
}

static jsval_t builtin_object_defineProperties(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t builtin_object_assign(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_mkerr(js, "Object.assign requires at least 1 argument");
  
  jsval_t target = args[0];
  uint8_t t = vtype(target);
  
  if (t == T_NULL || t == T_UNDEF) {
    return js_mkerr(js, "Cannot convert undefined or null to object");
  }
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) {
    target = js_mkobj(js);
  }
  
  jsval_t as_obj = (vtype(target) == T_OBJ) ? target : mkval(T_OBJ, vdata(target));
  
  for (int i = 1; i < nargs; i++) {
    jsval_t source = args[i];
    uint8_t st = vtype(source);
    
    if (st == T_NULL || st == T_UNDEF) continue;
    if (st != T_OBJ && st != T_ARR && st != T_FUNC) continue;
    
    jsval_t src_obj = (st == T_OBJ) ? source : mkval(T_OBJ, vdata(source));
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
      descriptor_entry_t *desc = lookup_descriptor(src_obj_off, key, klen);
      if (desc) {
        should_copy = desc->enumerable;
      }
      
      if (should_copy) {
        jsval_t key_str = js_mkstr(js, key, klen);
        setprop(js, as_obj, key_str, val);
      }
    }
  }
  
  return target;
}

static jsval_t builtin_object_freeze(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_mkundef();
  
  jsval_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return obj;
  jsval_t as_obj = (t == T_OBJ) ? obj : mkval(T_OBJ, vdata(obj));
  
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

static jsval_t builtin_object_isFrozen(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_true;
  
  jsval_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return js_true;
  jsval_t as_obj = (t == T_OBJ) ? obj : mkval(T_OBJ, vdata(obj));
  
  return js_bool(js_truthy(js, get_slot(js, as_obj, SLOT_FROZEN)));
}

static jsval_t builtin_object_seal(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_mkundef();
  
  jsval_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return obj;
  jsval_t as_obj = (t == T_OBJ) ? obj : mkval(T_OBJ, vdata(obj));
  
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

static jsval_t builtin_object_isSealed(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_true;
  
  jsval_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return js_true;
  jsval_t as_obj = (t == T_OBJ) ? obj : mkval(T_OBJ, vdata(obj));
  
  if (js_truthy(js, get_slot(js, as_obj, SLOT_SEALED))) return js_true;
  if (js_truthy(js, get_slot(js, as_obj, SLOT_FROZEN))) return js_true;
  
  return js_false;
}

static jsval_t builtin_object_fromEntries(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_mkerr(js, "Object.fromEntries requires an iterable argument");
  
  jsval_t iterable = args[0];
  uint8_t t = vtype(iterable);
  
  if (t != T_ARR && t != T_OBJ) {
    return js_mkerr(js, "Object.fromEntries requires an iterable");
  }
  
  jsval_t result = js_mkobj(js);
  jsoff_t len_off = lkp_interned(js, iterable, INTERN_LENGTH, 6);
  if (len_off == 0) return result;
  
  jsval_t len_val = resolveprop(js, mkval(T_PROP, len_off));
  if (vtype(len_val) != T_NUM) return result;
  
  jsoff_t len = (jsoff_t) tod(len_val);
  
  for (jsoff_t i = 0; i < len; i++) {
    char idx_str[16];
    snprintf(idx_str, sizeof(idx_str), "%u", (unsigned) i);
    
    jsoff_t entry_off = lkp(js, iterable, idx_str, strlen(idx_str));
    if (entry_off == 0) continue;
    
    jsval_t entry = resolveprop(js, mkval(T_PROP, entry_off));
    if (vtype(entry) != T_ARR && vtype(entry) != T_OBJ) continue;
    
    jsoff_t key_off = lkp(js, entry, "0", 1);
    if (key_off == 0) continue;
    jsval_t key = resolveprop(js, mkval(T_PROP, key_off));
    
    jsoff_t val_off = lkp(js, entry, "1", 1);
    jsval_t val = (val_off != 0) ? resolveprop(js, mkval(T_PROP, val_off)) : js_mkundef();
    
    if (vtype(key) != T_STR) {
      char buf[64];
      size_t n = tostr(js, key, buf, sizeof(buf));
      key = js_mkstr(js, buf, n);
    }
    
    setprop(js, result, key, val);
  }
  
  return result;
}

static jsval_t builtin_object_getOwnPropertyDescriptor(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  
  jsval_t obj = args[0];
  jsval_t key = args[1];
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return js_mkundef();
  if (vtype(key) != T_STR) {
    char buf[64];
    size_t n = tostr(js, key, buf, sizeof(buf));
    key = js_mkstr(js, buf, n);
  }
  
  jsval_t as_obj = (t == T_OBJ) ? obj : mkval(T_OBJ, vdata(obj));
  jsoff_t key_len, key_off = vstr(js, key, &key_len);
  const char *key_str = (char *) &js->mem[key_off];
  
  jsoff_t obj_off = (jsoff_t)vdata(as_obj);
  descriptor_entry_t *desc = lookup_descriptor(obj_off, key_str, key_len);
  
  jsoff_t prop_off = lkp(js, as_obj, key_str, key_len);
  if (prop_off == 0 && !desc) {
    return js_mkundef();
  }
  
  jsval_t result = js_mkobj(js);
  
  if (desc && (desc->has_getter || desc->has_setter)) {
    if (desc->has_getter) {
      setprop(js, result, js_mkstr(js, "get", 3), desc->getter);
    }
    if (desc->has_setter) {
      setprop(js, result, js_mkstr(js, "set", 3), desc->setter);
    }
    setprop(js, result, js_mkstr(js, "enumerable", 10), js_bool(desc->enumerable));
    setprop(js, result, js_mkstr(js, "configurable", 12), js_bool(desc->configurable));
  } else {
    if (prop_off != 0) {
      jsval_t prop_val = resolveprop(js, mkval(T_PROP, prop_off));
      setprop(js, result, js_mkstr(js, "value", 5), prop_val);
    }
    setprop(js, result, js_mkstr(js, "writable", 8), desc ? (js_bool(desc->writable)) : js_true);
    setprop(js, result, js_mkstr(js, "enumerable", 10), desc ? (js_bool(desc->enumerable)) : js_true);
    setprop(js, result, js_mkstr(js, "configurable", 12), desc ? (js_bool(desc->configurable)) : js_true);
  }
  
  return result;
  
  return js_mkundef();
}

static jsval_t builtin_object_getOwnPropertyNames(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkarr(js);
  jsval_t obj = args[0];
  
  if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR && vtype(obj) != T_FUNC) return mkarr(js);
  if (vtype(obj) == T_FUNC) obj = mkval(T_OBJ, vdata(obj));
  
  jsoff_t count = 0;
  jsoff_t next = loadoff(js, (jsoff_t) vdata(obj)) & ~(3U | FLAGMASK);
  
  while (next < js->brk && next != 0) {
    jsoff_t header = loadoff(js, next);
    if (!is_slot_prop(header)) {
      jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
      jsoff_t klen = offtolen(loadoff(js, koff));
      const char *key = (char *) &js->mem[koff + sizeof(koff)];
      if (!is_internal_prop(key, klen)) count++;
    }
    next = next_prop(header);
  }
  
  if (count == 0) {
    jsval_t arr = mkarr(js);
    js_mkprop_fast(js, arr, "length", 6, tov(0));
    return mkval(T_ARR, vdata(arr));
  }
  
  jsval_t *keys = malloc(count * sizeof(jsval_t));
  if (!keys) return js_mkerr(js, "out of memory");
  
  jsoff_t idx = 0;
  next = loadoff(js, (jsoff_t) vdata(obj)) & ~(3U | FLAGMASK);
  while (next < js->brk && next != 0 && idx < count) {
    jsoff_t header = loadoff(js, next);
    if (!is_slot_prop(header)) {
      jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
      jsoff_t klen = offtolen(loadoff(js, koff));
      const char *key = (char *) &js->mem[koff + sizeof(koff)];
      if (!is_internal_prop(key, klen)) {
        keys[idx++] = js_mkstr(js, key, klen);
      }
    }
    next = next_prop(header);
  }
  
  jsval_t arr = mkarr(js);
  for (jsoff_t i = 0; i < idx; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
    js_mkprop_fast(js, arr, idxstr, idxlen, keys[i]);
  }
  
  free(keys);
  js_mkprop_fast(js, arr, "length", 6, tov((double) idx));
  return mkval(T_ARR, vdata(arr));
}

static jsval_t builtin_object_isExtensible(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_true;
  
  jsval_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return js_true;
  jsval_t as_obj = (t == T_OBJ) ? obj : mkval(T_OBJ, vdata(obj));
  
  if (js_truthy(js, get_slot(js, as_obj, SLOT_FROZEN))) return js_false;
  if (js_truthy(js, get_slot(js, as_obj, SLOT_SEALED))) return js_false;
  
  jsval_t ext_slot = get_slot(js, as_obj, SLOT_EXTENSIBLE);
  if (vtype(ext_slot) != T_UNDEF) return js_bool(js_truthy(js, ext_slot));
  
  return js_true;
}

static jsval_t builtin_object_preventExtensions(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_mkundef();
  
  jsval_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return obj;
  jsval_t as_obj = (t == T_OBJ) ? obj : mkval(T_OBJ, vdata(obj));
  
  set_slot(js, as_obj, SLOT_EXTENSIBLE, js_false);
  return obj;
}

static jsval_t builtin_object_hasOwnProperty(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return mkval(T_BOOL, 0);
  
  jsval_t obj = js->this_val;
  jsval_t key = args[0];
  
  obj = resolveprop(js, obj);
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return mkval(T_BOOL, 0);
  if (vtype(key) != T_STR) {
    char buf[64];
    size_t n = tostr(js, key, buf, sizeof(buf));
    key = js_mkstr(js, buf, n);
  }
  
  jsval_t as_obj = (t == T_OBJ) ? obj : mkval(T_OBJ, vdata(obj));
  jsoff_t key_len, key_off = vstr(js, key, &key_len);
  const char *key_str = (char *) &js->mem[key_off];
  
  jsoff_t off = lkp(js, as_obj, key_str, key_len);
  return mkval(T_BOOL, off != 0 ? 1 : 0);
}

static jsval_t builtin_object_isPrototypeOf(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return mkval(T_BOOL, 0);
  
  jsval_t proto_obj = resolveprop(js, js->this_val);
  jsval_t obj = args[0];
  
  uint8_t obj_type = vtype(obj);
  if (obj_type != T_OBJ && obj_type != T_ARR && obj_type != T_FUNC) return mkval(T_BOOL, 0);
  
  uint8_t proto_type = vtype(proto_obj);
  if (proto_type != T_OBJ && proto_type != T_ARR && proto_type != T_FUNC) return mkval(T_BOOL, 0);
  jsoff_t proto_data = (jsoff_t)vdata(proto_obj);
  
  jsval_t current = get_proto(js, obj);
  while (!is_undefined(current) && !is_null(current)) {
    uint8_t cur_type = vtype(current);
    if (cur_type != T_OBJ && cur_type != T_ARR && cur_type != T_FUNC) break;
    if (vdata(current) == proto_data) return mkval(T_BOOL, 1);
    current = get_proto(js, current);
  }
  
  return mkval(T_BOOL, 0);
}

static jsval_t builtin_object_propertyIsEnumerable(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return mkval(T_BOOL, 0);
  
  jsval_t obj = js->this_val;
  jsval_t key = args[0];
  
  obj = resolveprop(js, obj);
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return mkval(T_BOOL, 0);
  if (vtype(key) != T_STR) {
    char buf[64];
    size_t n = tostr(js, key, buf, sizeof(buf));
    key = js_mkstr(js, buf, n);
  }
  
  jsval_t as_obj = (t == T_OBJ) ? obj : mkval(T_OBJ, vdata(obj));
  jsoff_t key_len, key_off = vstr(js, key, &key_len);
  const char *key_str = (char *) &js->mem[key_off];
  
  if (t == T_ARR && streq(key_str, key_len, "length", 6)) {
    return mkval(T_BOOL, 0);
  }
  
  jsoff_t off = lkp(js, as_obj, key_str, key_len);
  if (off == 0) return mkval(T_BOOL, 0);
  
  jsoff_t obj_off = (jsoff_t)vdata(as_obj);
  descriptor_entry_t *desc = lookup_descriptor(obj_off, key_str, key_len);
  if (desc) {
    return mkval(T_BOOL, desc->enumerable ? 1 : 0);
  }
  
  return mkval(T_BOOL, 1);
}

static jsval_t builtin_object_toString(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t obj = js->this_val;
  
  obj = resolveprop(js, obj);
  uint8_t t = vtype(obj);
  
  if (t == T_OBJ || t == T_ARR || t == T_FUNC) {
    jsval_t check_obj = (t == T_FUNC) ? mkval(T_OBJ, vdata(obj)) : obj;
    const char *tostr_tag_key = get_toStringTag_sym_key();
    jsoff_t tag_off = lkp(js, check_obj, tostr_tag_key, strlen(tostr_tag_key));
    if (tag_off == 0) tag_off = lkp_proto(js, check_obj, tostr_tag_key, strlen(tostr_tag_key));
    if (tag_off != 0) {
      jsval_t tag_val = resolveprop(js, mkval(T_PROP, tag_off));
      if (vtype(tag_val) == T_STR) {
        jsoff_t tag_len, tag_str_off = vstr(js, tag_val, &tag_len);
        const char *tag_str = (const char *)&js->mem[tag_str_off];
        
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "[object %.*s]", (int)tag_len, tag_str);
        return js_mkstr(js, buf, n);
      }
    }
  }
  
  const char *type_name = NULL;
  
  switch (t) {
    case T_UNDEF: type_name = "Undefined"; break;
    case T_NULL:  type_name = "Null"; break;
    case T_BOOL:  type_name = "Boolean"; break;
    case T_NUM:   type_name = "Number"; break;
    case T_STR:   type_name = "String"; break;
    case T_ARR:   type_name = "Array"; break;
    case T_FUNC:  type_name = "Function"; break;
    case T_ERR:   type_name = "Error"; break;
    case T_BIGINT: type_name = "BigInt"; break;
    case T_OBJ:   type_name = "Object"; break;
    default:      type_name = "Unknown"; break;
  }
  
  char buf[256];
  int n = snprintf(buf, sizeof(buf), "[object %s]", type_name);
  return js_mkstr(js, buf, n);
}

static jsval_t builtin_object_valueOf(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  return js->this_val;
}

static jsval_t builtin_object_toLocaleString(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  return js_call_toString(js, js->this_val);
}

static inline bool parse_array_index(const char *key, size_t key_len, jsoff_t len, unsigned *out_idx) {
  if (key_len == 0 || key[0] > '9' || key[0] < '0') return false;
  unsigned parsed_idx = 0;
  for (size_t i = 0; i < key_len; i++) {
    if (key[i] < '0' || key[i] > '9') return false;
    parsed_idx = parsed_idx * 10 + (key[i] - '0');
  }
  if (parsed_idx >= len) return false;
  *out_idx = parsed_idx;
  return true;
}

static inline bool is_callable(jsval_t v) {
  uint8_t t = vtype(v);
  return t == T_FUNC || t == T_CFUNC;
}

static inline jsval_t require_callback(struct js *js, jsval_t *args, int nargs, const char *name) {
  if (nargs == 0 || !is_callable(args[0]))
    return js_mkerr(js, "%s requires a function argument", name);
  return args[0];
}

static jsval_t array_shallow_copy(struct js *js, jsval_t arr, jsoff_t len) {
  jsval_t result = mkarr(js);
  if (is_err(result)) return result;
  
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

static jsval_t builtin_array_push(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  arr = resolveprop(js, arr);
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "push called on non-array");
  }
  
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
    setprop(js, arr, key, args[i]);
    len++;
  }
  
  jsval_t len_key = js_mkstr(js, "length", 6);
  jsval_t len_val = tov((double) len);
  
  setprop(js, arr, len_key, len_val);
  return len_val;
}

void js_arr_push(struct js *js, jsval_t arr, jsval_t val) {
  arr = resolveprop(js, arr);
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) return;
  
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

static jsval_t builtin_array_pop(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  jsval_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "pop called on non-array");
  }
  
  jsoff_t off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  if (len == 0) return js_mkundef();
  len--;
  char idxstr[16];
  size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)len);
  
  jsoff_t elem_off = lkp(js, arr, idxstr, idxlen);
  jsval_t result = js_mkundef();
  if (elem_off != 0) {
    result = resolveprop(js, mkval(T_PROP, elem_off));
  }
  
  jsval_t len_key = js_mkstr(js, "length", 6);
  jsval_t len_val = tov((double) len);
  setprop(js, arr, len_key, len_val);
  js->needs_gc = true;
  
  return result;
}

static jsval_t builtin_array_slice(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "slice called on non-array");
  }
  
  jsoff_t off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  jsoff_t start = 0, end = len;
  double dlen = D(len);
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    double d = tod(args[0]);
    if (d < 0) {
      start = (jsoff_t) (d + dlen < 0 ? 0 : d + dlen);
    } else {
      start = (jsoff_t) (d > dlen ? dlen : d);
    }
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
  jsval_t result = mkarr(js);
  if (is_err(result)) return result;
  jsoff_t result_idx = 0;
  
  for (jsoff_t i = start; i < end; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
    jsoff_t elem_off = lkp(js, arr, idxstr, idxlen);
    if (elem_off != 0) {
      jsval_t elem = resolveprop(js, mkval(T_PROP, elem_off));
      size_t result_idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)result_idx);
      jsval_t key = js_mkstr(js, idxstr, result_idxlen);
      setprop(js, result, key, elem);
    }
    result_idx++;
  }
  
  jsval_t len_key = js_mkstr(js, "length", 6);
  setprop(js, result, len_key, tov((double) result_idx));
  return mkval(T_ARR, vdata(result));
}

static jsval_t builtin_array_join(struct js *js, jsval_t *args, int nargs) {
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
  
  jsoff_t off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  if (len == 0) return js_mkstr(js, "", 0);
  
  size_t capacity = 1024;
  size_t result_len = 0;
  char *result = (char *)ant_calloc(capacity);
  if (!result) return js_mkerr(js, "oom");
  
  for (jsoff_t i = 0; i < len; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
    jsoff_t elem_off = lkp(js, arr, idxstr, idxlen);
    
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
    
    if (elem_off != 0) {
      jsval_t elem = resolveprop(js, mkval(T_PROP, elem_off));
      if (vtype(elem) == T_STR) {
        jsoff_t elem_len, elem_off_str = vstr(js, elem, &elem_len);
        if (result_len + elem_len >= capacity) {
          capacity = (result_len + elem_len + 1) * 2;
          char *new_result = (char *)ant_realloc(result, capacity);
          if (!new_result) return js_mkerr(js, "oom");
          result = new_result;
        }
        memcpy(result + result_len, &js->mem[elem_off_str], elem_len);
        result_len += elem_len;
      } else if (vtype(elem) == T_NUM) {
        char numstr[32];
        snprintf(numstr, sizeof(numstr), "%g", tod(elem));
        size_t num_len = strlen(numstr);
        if (result_len + num_len >= capacity) {
          capacity = (result_len + num_len + 1) * 2;
          char *new_result = (char *)ant_realloc(result, capacity);
          if (!new_result) return js_mkerr(js, "oom");
          result = new_result;
        }
        memcpy(result + result_len, numstr, num_len);
        result_len += num_len;
      } else if (vtype(elem) == T_BOOL) {
        const char *boolstr = vdata(elem) ? "true" : "false";
        size_t bool_len = strlen(boolstr);
        if (result_len + bool_len >= capacity) {
          capacity = (result_len + bool_len + 1) * 2;
          char *new_result = (char *)ant_realloc(result, capacity);
          if (!new_result) return js_mkerr(js, "oom");
          result = new_result;
        }
        memcpy(result + result_len, boolstr, bool_len);
        result_len += bool_len;
      }
    }
  }
  jsval_t ret = js_mkstr(js, result, result_len);
  free(result);
  return ret;
}

static jsval_t builtin_array_includes(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "includes called on non-array");
  
  if (nargs == 0) return mkval(T_BOOL, 0);
  jsval_t search = args[0];
  
  jsoff_t len_off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  
  if (len_off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, len_off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t)tod(len_val);
  }
  if (len == 0) return mkval(T_BOOL, 0);
  
  ant_iter_t iter = js_prop_iter_begin(js, arr);
  const char *key; size_t key_len; jsval_t val;
  
  while (js_prop_iter_next(&iter, &key, &key_len, &val)) {
    unsigned parsed_idx;
    if (!parse_array_index(key, key_len, len, &parsed_idx)) continue;
    
    if (vtype(val) == vtype(search)) {
      bool match = false;
      if (vtype(val) == T_NUM && tod(val) == tod(search)) match = true;
      else if (vtype(val) == T_BOOL && vdata(val) == vdata(search)) match = true;
      else if (vtype(val) == T_STR) {
        jsoff_t vl, vo = vstr(js, val, &vl);
        jsoff_t sl, so = vstr(js, search, &sl);
        if (vl == sl && memcmp(&js->mem[vo], &js->mem[so], vl) == 0) match = true;
      }
      else if ((vtype(val) == T_OBJ || vtype(val) == T_ARR || vtype(val) == T_FUNC) && vdata(val) == vdata(search)) match = true;
      
      if (match) {
        js_prop_iter_end(&iter);
        return mkval(T_BOOL, 1);
      }
    }
  }
  
  js_prop_iter_end(&iter);
  return mkval(T_BOOL, 0);
}

static jsval_t builtin_array_every(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "every called on non-array");
  
  jsval_t callback = require_callback(js, args, nargs, "every");
  if (is_err(callback)) return callback;
  
  jsoff_t len_off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (len_off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, len_off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t)tod(len_val);
  }
  if (len == 0) return mkval(T_BOOL, 1);
  
  ant_iter_t iter = js_prop_iter_begin(js, arr);
  const char *key;
  size_t key_len;
  jsval_t val;
  
  while (js_prop_iter_next(&iter, &key, &key_len, &val)) {
    unsigned parsed_idx;
    if (!parse_array_index(key, key_len, len, &parsed_idx)) continue;
    
    jsval_t call_args[3] = { val, tov((double)parsed_idx), arr };
    jsval_t result = call_js_with_args(js, callback, call_args, 3);
    
    if (is_err(result)) {
      js_prop_iter_end(&iter);
      return result;
    }
    if (!js_truthy(js, result)) {
      js_prop_iter_end(&iter);
      return mkval(T_BOOL, 0);
    }
  }
  js_prop_iter_end(&iter);
  
  return mkval(T_BOOL, 1);
}

static jsval_t builtin_array_forEach(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "forEach called on non-array");
  
  jsval_t callback = require_callback(js, args, nargs, "forEach");
  if (is_err(callback)) return callback;
  
  jsoff_t len_off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (len_off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, len_off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t)tod(len_val);
  }
  if (len == 0) return js_mkundef();
  
  ant_iter_t iter = js_prop_iter_begin(js, arr);
  const char *key;
  size_t key_len;
  jsval_t val;
  
  while (js_prop_iter_next(&iter, &key, &key_len, &val)) {
    unsigned parsed_idx;
    if (!parse_array_index(key, key_len, len, &parsed_idx)) continue;
    
    jsval_t call_args[3] = { val, tov((double)parsed_idx), arr };
    jsval_t result = call_js_with_args(js, callback, call_args, 3);
    
    if (is_err(result)) {
      js_prop_iter_end(&iter);
      return result;
    }
  }
  js_prop_iter_end(&iter);
  
  return js_mkundef();
}

static jsval_t builtin_array_reverse(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "reverse called on non-array");
  
  jsoff_t len_off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (len_off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, len_off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t)tod(len_val);
  }
  if (len <= 1) return arr;
  
  jsval_t *vals = malloc(len * sizeof(jsval_t));
  jsoff_t *offs = malloc(len * sizeof(jsoff_t));
  if (!vals || !offs) { free(vals); free(offs); return js_mkerr(js, "out of memory"); }
  
  jsoff_t count = 0;
  ant_iter_t iter = js_prop_iter_begin(js, arr);
  const char *key;
  size_t key_len;
  jsval_t val;
  
  while (js_prop_iter_next(&iter, &key, &key_len, &val)) {
    unsigned parsed_idx;
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

static jsval_t builtin_array_map(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "map called on non-array");
  
  jsval_t callback = require_callback(js, args, nargs, "map");
  if (is_err(callback)) return callback;
  
  jsoff_t len_off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (len_off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, len_off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t)tod(len_val);
  }
  
  jsval_t result = mkarr(js);
  if (is_err(result)) return result;
  
  if (len == 0) {
    js_mkprop_fast(js, result, "length", 6, tov(0.0));
    return mkval(T_ARR, vdata(result));
  }
  
  ant_iter_t iter = js_prop_iter_begin(js, arr);
  const char *key;
  size_t key_len;
  jsval_t val;
  
  while (js_prop_iter_next(&iter, &key, &key_len, &val)) {
    unsigned parsed_idx;
    if (!parse_array_index(key, key_len, len, &parsed_idx)) continue;
    
    jsval_t call_args[3] = { val, tov((double)parsed_idx), arr };
    jsval_t mapped = call_js_with_args(js, callback, call_args, 3);
    
    if (is_err(mapped)) {
      js_prop_iter_end(&iter);
      return mapped;
    }
    
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), parsed_idx);
    js_mkprop_fast(js, result, idxstr, idxlen, mapped);
  }
  js_prop_iter_end(&iter);
  
  js_mkprop_fast(js, result, "length", 6, tov((double)len));
  return mkval(T_ARR, vdata(result));
}

static jsval_t builtin_array_filter(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "filter called on non-array");
  
  jsval_t callback = require_callback(js, args, nargs, "filter");
  if (is_err(callback)) return callback;
  
  jsoff_t len_off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (len_off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, len_off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t)tod(len_val);
  }
  
  jsval_t result = mkarr(js);
  if (is_err(result)) return result;
  
  if (len == 0) {
    js_mkprop_fast(js, result, "length", 6, tov(0.0));
    return mkval(T_ARR, vdata(result));
  }
  
  jsoff_t result_idx = 0;
  ant_iter_t iter = js_prop_iter_begin(js, arr);
  const char *key;
  size_t key_len;
  jsval_t val;
  
  while (js_prop_iter_next(&iter, &key, &key_len, &val)) {
    unsigned parsed_idx;
    if (!parse_array_index(key, key_len, len, &parsed_idx)) continue;
    
    jsval_t call_args[3] = { val, tov((double)parsed_idx), arr };
    jsval_t test = call_js_with_args(js, callback, call_args, 3);
    
    if (is_err(test)) {
      js_prop_iter_end(&iter);
      return test;
    }
    
    if (js_truthy(js, test)) {
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)result_idx);
      js_mkprop_fast(js, result, idxstr, idxlen, val);
      result_idx++;
    }
  }
  js_prop_iter_end(&iter);
  
  js_mkprop_fast(js, result, "length", 6, tov((double)result_idx));
  return mkval(T_ARR, vdata(result));
}

static jsval_t builtin_array_reduce(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "reduce called on non-array");
  
  jsval_t callback = require_callback(js, args, nargs, "reduce");
  if (is_err(callback)) return callback;
  bool has_initial = (nargs >= 2);
  
  jsoff_t len_off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (len_off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, len_off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t)tod(len_val);
  }
  
  if (len == 0) {
    if (has_initial) return args[1];
    return js_mkerr(js, "reduce of empty array with no initial value");
  }
  
  jsval_t accumulator = has_initial ? args[1] : js_mkundef();
  bool first = !has_initial;
  
  ant_iter_t iter = js_prop_iter_begin(js, arr);
  const char *key;
  size_t key_len;
  jsval_t val;
  
  while (js_prop_iter_next(&iter, &key, &key_len, &val)) {
    unsigned parsed_idx;
    if (!parse_array_index(key, key_len, len, &parsed_idx)) continue;
    
    if (first) {
      accumulator = val;
      first = false;
      continue;
    }
    
    jsval_t call_args[4] = { accumulator, val, tov((double)parsed_idx), arr };
    accumulator = call_js_with_args(js, callback, call_args, 4);
    
    if (is_err(accumulator)) {
      js_prop_iter_end(&iter);
      return accumulator;
    }
  }
  
  js_prop_iter_end(&iter);
  if (first) return js_mkerr(js, "reduce of empty array with no initial value");
  
  return accumulator;
}

static void flat_helper(struct js *js, jsval_t arr, jsval_t result, jsoff_t *result_idx, int depth) {
  jsoff_t len_off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (len_off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, len_off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t)tod(len_val);
  }
  if (len == 0) return;
  
  ant_iter_t iter = js_prop_iter_begin(js, arr);
  const char *key;
  size_t key_len;
  jsval_t val;
  
  while (js_prop_iter_next(&iter, &key, &key_len, &val)) {
    unsigned parsed_idx;
    if (!parse_array_index(key, key_len, len, &parsed_idx)) continue;
    
    if (depth > 0 && (vtype(val) == T_ARR || vtype(val) == T_OBJ)) {
      flat_helper(js, val, result, result_idx, depth - 1);
    } else {
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)*result_idx);
      js_mkprop_fast(js, result, idxstr, idxlen, val);
      (*result_idx)++;
    }
  }
  
  js_prop_iter_end(&iter);
}

static jsval_t builtin_array_flat(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "flat called on non-array");
  }
  
  int depth = 1;
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    depth = (int) tod(args[0]);
    if (depth < 0) depth = 0;
  }
  
  jsval_t result = mkarr(js);
  if (is_err(result)) return result;
  jsoff_t result_idx = 0;
  
  flat_helper(js, arr, result, &result_idx, depth);
  
  jsval_t len_key = js_mkstr(js, "length", 6);
  setprop(js, result, len_key, tov((double) result_idx));
  return mkval(T_ARR, vdata(result));
}

static jsval_t builtin_array_concat(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "concat called on non-array");
  }
  
  jsval_t result = mkarr(js);
  if (is_err(result)) return result;
  jsoff_t result_idx = 0;
  
  jsoff_t off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  for (jsoff_t i = 0; i < len; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
    jsoff_t elem_off = lkp(js, arr, idxstr, idxlen);
    if (elem_off != 0) {
      jsval_t elem = resolveprop(js, mkval(T_PROP, elem_off));
      char res_idx[16];
      size_t res_idx_len = uint_to_str(res_idx, sizeof(res_idx), (unsigned)result_idx);
      js_mkprop_fast(js, result, res_idx, res_idx_len, elem);
    }
    result_idx++;
  }
  
  for (int a = 0; a < nargs; a++) {
    jsval_t arg = args[a];
    if (vtype(arg) == T_ARR || vtype(arg) == T_OBJ) {
      jsoff_t arg_off = lkp_interned(js, arg, INTERN_LENGTH, 6);
      jsoff_t arg_len = 0;
      if (arg_off != 0) {
        jsval_t len_val = resolveprop(js, mkval(T_PROP, arg_off));
        if (vtype(len_val) == T_NUM) arg_len = (jsoff_t) tod(len_val);
      }
      
      for (jsoff_t i = 0; i < arg_len; i++) {
        char idxstr[16];
        size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
        jsoff_t elem_off = lkp(js, arg, idxstr, idxlen);
        if (elem_off != 0) {
          jsval_t elem = resolveprop(js, mkval(T_PROP, elem_off));
          char res_idx[16];
          size_t res_idx_len = uint_to_str(res_idx, sizeof(res_idx), (unsigned)result_idx);
          js_mkprop_fast(js, result, res_idx, res_idx_len, elem);
        }
        result_idx++;
      }
    } else {
      char res_idx[16];
      size_t res_idx_len = uint_to_str(res_idx, sizeof(res_idx), (unsigned)result_idx);
      js_mkprop_fast(js, result, res_idx, res_idx_len, arg);
      result_idx++;
    }
  }
  
  js_mkprop_fast(js, result, "length", 6, tov((double)result_idx));
  return mkval(T_ARR, vdata(result));
}

static jsval_t builtin_array_at(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "at called on non-array");
  }
  
  if (nargs == 0 || vtype(args[0]) != T_NUM) return js_mkundef();
  
  jsoff_t off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  int idx = (int) tod(args[0]);
  if (idx < 0) idx = (int)len + idx;
  if (idx < 0 || (jsoff_t)idx >= len) return js_mkundef();
  
  char idxstr[16];
  size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
  jsoff_t elem_off = lkp(js, arr, idxstr, idxlen);
  return elem_off ? resolveprop(js, mkval(T_PROP, elem_off)) : js_mkundef();
}

static jsval_t builtin_array_fill(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "fill called on non-array");
  }
  
  jsval_t value = nargs >= 1 ? args[0] : js_mkundef();
  
  jsoff_t off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
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
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
    jsval_t key = js_mkstr(js, idxstr, idxlen);
    setprop(js, arr, key, value);
  }
  
  return arr;
}

static jsval_t array_find_impl(struct js *js, jsval_t *args, int nargs, bool return_index, const char *name) {
  jsval_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "%s called on non-array", name);
  
  jsval_t callback = require_callback(js, args, nargs, name);
  if (is_err(callback)) return callback;
  
  jsoff_t len_off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (len_off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, len_off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t)tod(len_val);
  }
  if (len == 0) return return_index ? tov(-1) : js_mkundef();
  
  ant_iter_t iter = js_prop_iter_begin(js, arr);
  const char *key;
  size_t key_len;
  jsval_t val;
  
  while (js_prop_iter_next(&iter, &key, &key_len, &val)) {
    unsigned parsed_idx;
    if (!parse_array_index(key, key_len, len, &parsed_idx)) continue;
    
    jsval_t call_args[3] = { val, tov((double)parsed_idx), arr };
    jsval_t result = call_js_with_args(js, callback, call_args, 3);
    
    if (is_err(result)) {
      js_prop_iter_end(&iter);
      return result;
    }
    if (js_truthy(js, result)) {
      js_prop_iter_end(&iter);
      return return_index ? tov((double)parsed_idx) : val;
    }
  }
  js_prop_iter_end(&iter);
  
  return return_index ? tov(-1) : js_mkundef();
}

static jsval_t builtin_array_find(struct js *js, jsval_t *args, int nargs) {
  return array_find_impl(js, args, nargs, false, "find");
}

static jsval_t builtin_array_findIndex(struct js *js, jsval_t *args, int nargs) {
  return array_find_impl(js, args, nargs, true, "findIndex");
}

static jsval_t array_find_last_impl(struct js *js, jsval_t *args, int nargs, bool return_index, const char *name) {
  jsval_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "%s called on non-array", name);
  
  jsval_t callback = require_callback(js, args, nargs, name);
  if (is_err(callback)) return callback;
  
  jsoff_t len_off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (len_off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, len_off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t)tod(len_val);
  }
  if (len == 0) return return_index ? tov(-1) : js_mkundef();
  
  jsval_t *vals = malloc(len * sizeof(jsval_t));
  unsigned *idxs = malloc(len * sizeof(unsigned));
  if (!vals || !idxs) { free(vals); free(idxs); return js_mkerr(js, "out of memory"); }
  
  jsoff_t count = 0;
  ant_iter_t iter = js_prop_iter_begin(js, arr);
  const char *key; size_t key_len; jsval_t val;
  
  while (js_prop_iter_next(&iter, &key, &key_len, &val)) {
    unsigned parsed_idx;
    if (!parse_array_index(key, key_len, len, &parsed_idx) || count >= len) continue;
    
    vals[count] = val;
    idxs[count] = parsed_idx;
    count++;
  }
  js_prop_iter_end(&iter);
  
  for (jsoff_t i = count; i > 0; i--) {
    jsval_t call_args[3] = { vals[i-1], tov((double)idxs[i-1]), arr };
    jsval_t result = call_js_with_args(js, callback, call_args, 3);
    
    if (is_err(result)) {
      free(vals); free(idxs);
      return result;
    }
    if (js_truthy(js, result)) {
      jsval_t found_val = vals[i-1];
      unsigned found_idx = idxs[i-1];
      free(vals); free(idxs);
      return return_index ? tov((double)found_idx) : found_val;
    }
  }
  
  free(vals); free(idxs);
  return return_index ? tov(-1) : js_mkundef();
}

static jsval_t builtin_array_findLast(struct js *js, jsval_t *args, int nargs) {
  return array_find_last_impl(js, args, nargs, false, "findLast");
}

static jsval_t builtin_array_findLastIndex(struct js *js, jsval_t *args, int nargs) {
  return array_find_last_impl(js, args, nargs, true, "findLastIndex");
}

static jsval_t builtin_array_flatMap(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "flatMap called on non-array");
  }
  if (nargs == 0 || vtype(args[0]) != T_FUNC) {
    return js_mkerr(js, "flatMap requires a function argument");
  }
  
  jsval_t callback = args[0];
  jsoff_t off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  jsval_t result = mkarr(js);
  if (is_err(result)) return result;
  jsoff_t result_idx = 0;
  
  for (jsoff_t i = 0; i < len; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
    jsoff_t elem_off = lkp(js, arr, idxstr, idxlen);
    jsval_t elem = elem_off ? resolveprop(js, mkval(T_PROP, elem_off)) : js_mkundef();
    jsval_t call_args[3] = { elem, tov((double)i), arr };
    jsval_t mapped = call_js_with_args(js, callback, call_args, 3);
    if (is_err(mapped)) return mapped;
    
    if (vtype(mapped) == T_ARR || vtype(mapped) == T_OBJ) {
      jsoff_t m_off = lkp_interned(js, mapped, INTERN_LENGTH, 6);
      jsoff_t m_len = 0;
      if (m_off != 0) {
        jsval_t m_len_val = resolveprop(js, mkval(T_PROP, m_off));
        if (vtype(m_len_val) == T_NUM) m_len = (jsoff_t) tod(m_len_val);
      }
      for (jsoff_t j = 0; j < m_len; j++) {
        char jstr[16];
        snprintf(jstr, sizeof(jstr), "%u", (unsigned) j);
        jsoff_t m_elem_off = lkp(js, mapped, jstr, strlen(jstr));
        if (m_elem_off != 0) {
          jsval_t m_elem = resolveprop(js, mkval(T_PROP, m_elem_off));
          char res_idx[16];
          snprintf(res_idx, sizeof(res_idx), "%u", (unsigned) result_idx);
          jsval_t key = js_mkstr(js, res_idx, strlen(res_idx));
          setprop(js, result, key, m_elem);
        }
        result_idx++;
      }
    } else {
      char res_idx[16];
      snprintf(res_idx, sizeof(res_idx), "%u", (unsigned) result_idx);
      jsval_t key = js_mkstr(js, res_idx, strlen(res_idx));
      setprop(js, result, key, mapped);
      result_idx++;
    }
  }
  
  jsval_t len_key = js_mkstr(js, "length", 6);
  setprop(js, result, len_key, tov((double) result_idx));
  return mkval(T_ARR, vdata(result));
}

static const char *js_tostring(struct js *js, jsval_t v) {
  if (vtype(v) == T_STR) {
    jsoff_t slen, off = vstr(js, v, &slen);
    return (const char *)&js->mem[off];
  }
  return js_str(js, v);
}

static int js_compare_values(struct js *js, jsval_t a, jsval_t b, jsval_t compareFn) {
  uint8_t t = vtype(compareFn);
  if (t == T_FUNC || t == T_CFUNC) {
    jsval_t call_args[2] = { a, b };
    jsval_t result = call_js_with_args(js, compareFn, call_args, 2);
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

static jsval_t builtin_array_indexOf(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "indexOf called on non-array");
  }
  if (nargs == 0) return tov(-1);
  
  jsval_t search = args[0];
  jsoff_t off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  jsoff_t start = 0;
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    int s = (int) tod(args[1]);
    if (s < 0) s = (int)len + s;
    if (s < 0) s = 0;
    start = (jsoff_t) s;
  }
  
  for (jsoff_t i = start; i < len; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
    jsoff_t elem_off = lkp(js, arr, idxstr, idxlen);
    if (elem_off != 0) {
      jsval_t elem = resolveprop(js, mkval(T_PROP, elem_off));
      if (vtype(elem) == vtype(search)) {
        if (vtype(elem) == T_NUM && tod(elem) == tod(search)) return tov((double)i);
        if (vtype(elem) == T_BOOL && vdata(elem) == vdata(search)) return tov((double)i);
        if (vtype(elem) == T_STR) {
          jsoff_t el, eo = vstr(js, elem, &el);
          jsoff_t sl, so = vstr(js, search, &sl);
          if (el == sl && memcmp(&js->mem[eo], &js->mem[so], el) == 0) return tov((double)i);
        }
        if ((vtype(elem) == T_OBJ || vtype(elem) == T_ARR || vtype(elem) == T_FUNC) && vdata(elem) == vdata(search)) return tov((double)i);
      }
    }
  }
  return tov(-1);
}

static jsval_t builtin_array_lastIndexOf(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "lastIndexOf called on non-array");
  }
  if (nargs == 0) return tov(-1);
  
  jsval_t search = args[0];
  jsoff_t off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  int start = (int)len - 1;
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    start = (int) tod(args[1]);
    if (start < 0) start = (int)len + start;
  }
  if (start >= (int)len) start = (int)len - 1;
  
  for (int i = start; i >= 0; i--) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
    jsoff_t elem_off = lkp(js, arr, idxstr, idxlen);
    if (elem_off != 0) {
      jsval_t elem = resolveprop(js, mkval(T_PROP, elem_off));
      if (vtype(elem) == vtype(search)) {
        if (vtype(elem) == T_NUM && tod(elem) == tod(search)) return tov((double)i);
        if (vtype(elem) == T_BOOL && vdata(elem) == vdata(search)) return tov((double)i);
        if (vtype(elem) == T_STR) {
          jsoff_t el, eo = vstr(js, elem, &el);
          jsoff_t sl, so = vstr(js, search, &sl);
          if (el == sl && memcmp(&js->mem[eo], &js->mem[so], el) == 0) return tov((double)i);
        }
        if ((vtype(elem) == T_OBJ || vtype(elem) == T_ARR || vtype(elem) == T_FUNC) && vdata(elem) == vdata(search)) return tov((double)i);
      }
    }
  }
  return tov(-1);
}

static jsval_t builtin_array_reduceRight(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "reduceRight called on non-array");
  }
  if (nargs == 0 || vtype(args[0]) != T_FUNC) {
    return js_mkerr(js, "reduceRight requires a function argument");
  }
  
  jsval_t callback = args[0];
  jsoff_t off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  int start_idx = (int)len - 1;
  jsval_t accumulator;
  
  if (nargs >= 2) {
    accumulator = args[1];
  } else {
    if (len == 0) return js_mkerr(js, "reduceRight of empty array with no initial value");
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)(len - 1));
    jsoff_t elem_off = lkp(js, arr, idxstr, idxlen);
    accumulator = elem_off ? resolveprop(js, mkval(T_PROP, elem_off)) : js_mkundef();
    start_idx = (int)len - 2;
  }
  
  for (int i = start_idx; i >= 0; i--) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
    jsoff_t elem_off = lkp(js, arr, idxstr, idxlen);
    jsval_t elem = elem_off ? resolveprop(js, mkval(T_PROP, elem_off)) : js_mkundef();
    jsval_t call_args[4] = { accumulator, elem, tov((double)i), arr };
    accumulator = call_js_with_args(js, callback, call_args, 4);
    if (is_err(accumulator)) return accumulator;
  }
  
  return accumulator;
}

static jsval_t builtin_array_shift(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "shift called on non-array");
  }
  
  jsoff_t off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  if (len == 0) return js_mkundef();
  
  jsoff_t first_off = lkp(js, arr, "0", 1);
  jsval_t first = first_off ? resolveprop(js, mkval(T_PROP, first_off)) : js_mkundef();
  
  for (jsoff_t i = 1; i < len; i++) {
    char src[16], dst[16];
    snprintf(src, sizeof(src), "%u", (unsigned) i);
    snprintf(dst, sizeof(dst), "%u", (unsigned)(i - 1));
    jsoff_t elem_off = lkp(js, arr, src, strlen(src));
    jsval_t elem = elem_off ? resolveprop(js, mkval(T_PROP, elem_off)) : js_mkundef();
    jsval_t key = js_mkstr(js, dst, strlen(dst));
    setprop(js, arr, key, elem);
  }
  
  jsval_t len_key = js_mkstr(js, "length", 6);
  setprop(js, arr, len_key, tov((double)(len - 1)));
  js->needs_gc = true;
  return first;
}

static jsval_t builtin_array_unshift(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "unshift called on non-array");
  }
  
  jsoff_t off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  for (int i = (int)len - 1; i >= 0; i--) {
    char src[16], dst[16];
    snprintf(src, sizeof(src), "%u", (unsigned) i);
    snprintf(dst, sizeof(dst), "%u", (unsigned)(i + nargs));
    jsoff_t elem_off = lkp(js, arr, src, strlen(src));
    jsval_t elem = elem_off ? resolveprop(js, mkval(T_PROP, elem_off)) : js_mkundef();
    jsval_t key = js_mkstr(js, dst, strlen(dst));
    setprop(js, arr, key, elem);
  }
  
  for (int i = 0; i < nargs; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
    jsval_t key = js_mkstr(js, idxstr, idxlen);
    setprop(js, arr, key, args[i]);
  }
  
  jsval_t len_key = js_mkstr(js, "length", 6);
  jsoff_t new_len = len + nargs;
  setprop(js, arr, len_key, tov((double) new_len));
  return tov((double) new_len);
}

static jsval_t builtin_array_some(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "some called on non-array");
  
  jsval_t callback = require_callback(js, args, nargs, "some");
  if (is_err(callback)) return callback;
  
  jsoff_t len_off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (len_off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, len_off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t)tod(len_val);
  }
  if (len == 0) return mkval(T_BOOL, 0);
  
  ant_iter_t iter = js_prop_iter_begin(js, arr);
  const char *key;
  size_t key_len;
  jsval_t val;
  
  while (js_prop_iter_next(&iter, &key, &key_len, &val)) {
    unsigned parsed_idx;
    if (!parse_array_index(key, key_len, len, &parsed_idx)) continue;
    
    jsval_t call_args[3] = { val, tov((double)parsed_idx), arr };
    jsval_t result = call_js_with_args(js, callback, call_args, 3);
    
    if (is_err(result)) {
      js_prop_iter_end(&iter);
      return result;
    }
    if (js_truthy(js, result)) {
      js_prop_iter_end(&iter);
      return mkval(T_BOOL, 1);
    }
  }
  js_prop_iter_end(&iter);
  
  return mkval(T_BOOL, 0);
}

static jsval_t builtin_array_sort(struct js *js, jsval_t *args, int nargs) {
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
  
  jsoff_t off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t)tod(len_val);
  }
  if (len == 0) return arr;
  
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
  for (jsoff_t i = 0; i < count; i++)
    saveval(js, offs[i] + sizeof(jsoff_t) * 2, vals[i]);
  for (jsoff_t i = 0; i < undef_count; i++)
    saveval(js, offs[count + i] + sizeof(jsoff_t) * 2, js_mkundef());
  
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

static jsval_t builtin_array_splice(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "splice called on non-array");
  }
  
  jsoff_t off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
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
  
  jsval_t removed = mkarr(js);
  if (is_err(removed)) return removed;
  
  for (int i = 0; i < deleteCount; i++) {
    char src[16], dst[16];
    snprintf(src, sizeof(src), "%u", (unsigned)(start + i));
    snprintf(dst, sizeof(dst), "%u", (unsigned) i);
    jsoff_t elem_off = lkp(js, arr, src, strlen(src));
    if (elem_off != 0) {
      jsval_t elem = resolveprop(js, mkval(T_PROP, elem_off));
      jsval_t key = js_mkstr(js, dst, strlen(dst));
      setprop(js, removed, key, elem);
    }
  }
  jsval_t rem_len_key = js_mkstr(js, "length", 6);
  setprop(js, removed, rem_len_key, tov((double) deleteCount));
  
  int shift = insertCount - deleteCount;
  if (shift > 0) {
    for (int i = (int)len - 1; i >= start + deleteCount; i--) {
      char src[16], dst[16];
      snprintf(src, sizeof(src), "%u", (unsigned) i);
      snprintf(dst, sizeof(dst), "%u", (unsigned)(i + shift));
      jsoff_t elem_off = lkp(js, arr, src, strlen(src));
      jsval_t elem = elem_off ? resolveprop(js, mkval(T_PROP, elem_off)) : js_mkundef();
      jsval_t key = js_mkstr(js, dst, strlen(dst));
      setprop(js, arr, key, elem);
    }
  } else if (shift < 0) {
    for (int i = start + deleteCount; i < (int)len; i++) {
      char src[16], dst[16];
      snprintf(src, sizeof(src), "%u", (unsigned) i);
      snprintf(dst, sizeof(dst), "%u", (unsigned)(i + shift));
      jsoff_t elem_off = lkp(js, arr, src, strlen(src));
      jsval_t elem = elem_off ? resolveprop(js, mkval(T_PROP, elem_off)) : js_mkundef();
      jsval_t key = js_mkstr(js, dst, strlen(dst));
      setprop(js, arr, key, elem);
    }
  }
  
  for (int i = 0; i < insertCount; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)(start + i));
    jsval_t key = js_mkstr(js, idxstr, idxlen);
    setprop(js, arr, key, args[2 + i]);
  }
  
  jsval_t len_key = js_mkstr(js, "length", 6);
  setprop(js, arr, len_key, tov((double)((int)len + shift)));
  if (deleteCount > 0) js->needs_gc = true;
  return mkval(T_ARR, vdata(removed));
}

static jsval_t builtin_array_copyWithin(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "copyWithin called on non-array");
  }
  
  jsoff_t off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
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
  
  jsval_t *temp = (jsval_t *)malloc(count * sizeof(jsval_t));
  for (int i = 0; i < count; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)(start + i));
    jsoff_t elem_off = lkp(js, arr, idxstr, idxlen);
    temp[i] = elem_off ? resolveprop(js, mkval(T_PROP, elem_off)) : js_mkundef();
  }
  
  for (int i = 0; i < count; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)(target + i));
    jsval_t key = js_mkstr(js, idxstr, idxlen);
    setprop(js, arr, key, temp[i]);
  }
  
  free(temp);
  return arr;
}

static jsval_t builtin_array_toSorted(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t builtin_array_toReversed(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t builtin_array_toSpliced(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t builtin_array_with(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "with called on non-array");
  }
  
  if (nargs < 2) return js_mkerr(js, "with requires index and value arguments");
  
  jsoff_t off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  int idx = (int) tod(args[0]);
  if (idx < 0) idx = (int)len + idx;
  if (idx < 0 || (jsoff_t)idx >= len) return js_mkerr(js, "Invalid index");
  
  jsval_t result = mkarr(js);
  if (is_err(result)) return result;
  
  for (jsoff_t i = 0; i < len; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
    jsval_t elem;
    if ((jsoff_t)idx == i) {
      elem = args[1];
    } else {
      jsoff_t elem_off = lkp(js, arr, idxstr, idxlen);
      elem = elem_off ? resolveprop(js, mkval(T_PROP, elem_off)) : js_mkundef();
    }
    jsval_t key = js_mkstr(js, idxstr, idxlen);
    setprop(js, result, key, elem);
  }
  
  jsval_t len_key = js_mkstr(js, "length", 6);
  setprop(js, result, len_key, tov((double) len));
  return mkval(T_ARR, vdata(result));
}

static jsval_t builtin_array_keys(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "keys called on non-array");
  }
  
  jsoff_t off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  jsval_t result = mkarr(js);
  if (is_err(result)) return result;
  
  for (jsoff_t i = 0; i < len; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
    jsval_t key = js_mkstr(js, idxstr, idxlen);
    setprop(js, result, key, tov((double) i));
  }
  
  jsval_t len_key = js_mkstr(js, "length", 6);
  setprop(js, result, len_key, tov((double) len));
  return mkval(T_ARR, vdata(result));
}

static jsval_t builtin_array_values(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "values called on non-array");
  }
  
  jsoff_t off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  jsval_t result = mkarr(js);
  if (is_err(result)) return result;
  
  for (jsoff_t i = 0; i < len; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
    jsoff_t elem_off = lkp(js, arr, idxstr, idxlen);
    jsval_t elem = elem_off ? resolveprop(js, mkval(T_PROP, elem_off)) : js_mkundef();
    jsval_t key = js_mkstr(js, idxstr, idxlen);
    setprop(js, result, key, elem);
  }
  
  jsval_t len_key = js_mkstr(js, "length", 6);
  setprop(js, result, len_key, tov((double) len));
  return mkval(T_ARR, vdata(result));
}

static jsval_t builtin_array_entries(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "entries called on non-array");
  }
  
  jsoff_t off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  jsval_t result = mkarr(js);
  if (is_err(result)) return result;
  
  for (jsoff_t i = 0; i < len; i++) {
    jsval_t entry = mkarr(js);
    if (is_err(entry)) return entry;
    
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
    jsoff_t elem_off = lkp(js, arr, idxstr, idxlen);
    jsval_t elem = elem_off ? resolveprop(js, mkval(T_PROP, elem_off)) : js_mkundef();
    
    setprop(js, entry, js_mkstr(js, "0", 1), tov((double) i));
    setprop(js, entry, js_mkstr(js, "1", 1), elem);
    setprop(js, entry, js_mkstr(js, "length", 6), tov(2));
    
    jsval_t key = js_mkstr(js, idxstr, idxlen);
    setprop(js, result, key, mkval(T_ARR, vdata(entry)));
  }
  
  jsval_t len_key = js_mkstr(js, "length", 6);
  setprop(js, result, len_key, tov((double) len));
  return mkval(T_ARR, vdata(result));
}

static jsval_t builtin_array_toString(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  jsval_t arr = js->this_val;
  jsval_t sep_args[1] = { js_mkstr(js, ",", 1) };
  jsval_t old_this = js->this_val;
  js->this_val = arr;
  jsval_t result = builtin_array_join(js, sep_args, 1);
  js->this_val = old_this;
  return result;
}

static jsval_t builtin_array_toLocaleString(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR) return js_mkerr(js, "toLocaleString called on non-array");
  
  jsoff_t len_off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  jsoff_t len = 0;
  if (len_off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, len_off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t)tod(len_val);
  }
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
    
    char idx_str[16];
    snprintf(idx_str, sizeof(idx_str), "%u", (unsigned)i);
    jsoff_t elem_off = lkp(js, arr, idx_str, strlen(idx_str));
    if (elem_off == 0) continue;
    
    jsval_t elem = resolveprop(js, mkval(T_PROP, elem_off));
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

static jsval_t builtin_Array_isArray(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs == 0) return mkval(T_BOOL, 0);
  return mkval(T_BOOL, vtype(args[0]) == T_ARR ? 1 : 0);
}

static jsval_t builtin_Array_from(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkarr(js);
  
  jsval_t src = args[0];
  jsval_t mapFn = (nargs >= 2 && vtype(args[1]) == T_FUNC) ? args[1] : js_mkundef();
  
  jsval_t result = mkarr(js);
  if (is_err(result)) return result;
  
  if (vtype(src) == T_STR) {
    jsoff_t str_len, str_off = vstr(js, src, &str_len);
    const char *str_ptr = (const char *)&js->mem[str_off];
    for (jsoff_t i = 0; i < str_len; i++) {
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
      jsval_t elem = js_mkstr(js, str_ptr + i, 1);
      if (vtype(mapFn) == T_FUNC) {
        jsval_t call_args[2] = { elem, tov((double)i) };
        elem = call_js_with_args(js, mapFn, call_args, 2);
        if (is_err(elem)) return elem;
      }
      jsval_t key = js_mkstr(js, idxstr, idxlen);
      setprop(js, result, key, elem);
    }
    jsval_t len_key = js_mkstr(js, "length", 6);
    setprop(js, result, len_key, tov((double) str_len));
  } else if (vtype(src) == T_ARR || vtype(src) == T_OBJ) {
    jsoff_t off = lkp_interned(js, src, INTERN_LENGTH, 6);
    jsoff_t len = 0;
    if (off != 0) {
      jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
      if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
    }
    for (jsoff_t i = 0; i < len; i++) {
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
      jsoff_t elem_off = lkp(js, src, idxstr, idxlen);
      jsval_t elem = elem_off ? resolveprop(js, mkval(T_PROP, elem_off)) : js_mkundef();
      if (vtype(mapFn) == T_FUNC) {
        jsval_t call_args[2] = { elem, tov((double)i) };
        elem = call_js_with_args(js, mapFn, call_args, 2);
        if (is_err(elem)) return elem;
      }
      jsval_t key = js_mkstr(js, idxstr, idxlen);
      setprop(js, result, key, elem);
    }
    jsval_t len_key = js_mkstr(js, "length", 6);
    setprop(js, result, len_key, tov((double) len));
  }
  
  return mkval(T_ARR, vdata(result));
}

static jsval_t builtin_Array_of(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = mkarr(js);
  if (is_err(arr)) return arr;
  
  for (int i = 0; i < nargs; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
    jsval_t key = js_mkstr(js, idxstr, idxlen);
    setprop(js, arr, key, args[i]);
  }
  jsval_t len_key = js_mkstr(js, "length", 6);
  setprop(js, arr, len_key, tov((double) nargs));
  return mkval(T_ARR, vdata(arr));
}

static jsval_t builtin_string_indexOf(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t builtin_string_substring(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t builtin_string_substr(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t builtin_string_split(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
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
    setprop(js, arr, js_mkstr(js, "length", 6), tov(0));
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
        char idxstr[16];
        size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
        jsval_t key = js_mkstr(js, idxstr, idxlen);
        jsval_t part = js_mkstr(js, str_ptr + i, 1);
        setprop(js, arr, key, part);
        idx++;
      }
      jsval_t len_key = js_mkstr(js, "length", 6);
      setprop(js, arr, len_key, tov((double)idx));
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
        setprop(js, arr, js_mkstr(js, "length", 6), tov(0));
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

      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
      jsval_t key = js_mkstr(js, idxstr, idxlen);
      jsval_t part = js_mkstr(js, str_ptr + segment_start, match_start - segment_start);
      setprop(js, arr, key, part);
      idx++;

      for (uint32_t i = 1; i <= capture_count && idx < limit; i++) {
        PCRE2_SIZE cap_start = ovector[2*i];
        PCRE2_SIZE cap_end = ovector[2*i+1];
        size_t cap_idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
        key = js_mkstr(js, idxstr, cap_idxlen);
        if (cap_start == PCRE2_UNSET) {
          setprop(js, arr, key, js_mkundef());
        } else {
          part = js_mkstr(js, str_ptr + cap_start, cap_end - cap_start);
          setprop(js, arr, key, part);
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
      setprop(js, arr, js_mkstr(js, "0", 1), js_mkstr(js, str_ptr, str_len));
      setprop(js, arr, js_mkstr(js, "length", 6), tov(1));
      return mkval(T_ARR, vdata(arr));
    }

    if (idx < limit) {
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
      jsval_t key = js_mkstr(js, idxstr, idxlen);
      jsval_t part = js_mkstr(js, str_ptr + segment_start, str_len - segment_start);
      setprop(js, arr, key, part);
      idx++;
    }

    pcre2_match_data_free(match_data);
    pcre2_code_free(re);
    jsval_t len_key = js_mkstr(js, "length", 6);
    setprop(js, arr, len_key, tov((double)idx));
    return mkval(T_ARR, vdata(arr));
  }

  if (vtype(sep_arg) != T_STR) goto return_whole;

  jsoff_t sep_len, sep_off = vstr(js, sep_arg, &sep_len);
  const char *sep_ptr = (char *) &js->mem[sep_off];
  jsoff_t idx = 0, start = 0;

  if (sep_len == 0) {
    for (jsoff_t i = 0; i < str_len && idx < limit; i++) {
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
      jsval_t key = js_mkstr(js, idxstr, idxlen);
      jsval_t part = js_mkstr(js, str_ptr + i, 1);
      setprop(js, arr, key, part);
      idx++;
    }
    goto set_length;
  }

  for (jsoff_t i = 0; i + sep_len <= str_len && idx < limit; i++) {
    if (memcmp(str_ptr + i, sep_ptr, sep_len) != 0) continue;
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
    jsval_t key = js_mkstr(js, idxstr, idxlen);
    jsval_t part = js_mkstr(js, str_ptr + start, i - start);
    setprop(js, arr, key, part);
    idx++;
    start = i + sep_len;
    i += sep_len - 1;
  }
  if (idx < limit && start <= str_len) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
    jsval_t key = js_mkstr(js, idxstr, idxlen);
    jsval_t part = js_mkstr(js, str_ptr + start, str_len - start);
    setprop(js, arr, key, part);
    idx++;
  }

set_length:;
  jsval_t len_key = js_mkstr(js, "length", 6);
  setprop(js, arr, len_key, tov((double) idx));
  return mkval(T_ARR, vdata(arr));

return_whole:
  if (limit > 0) {
    setprop(js, arr, js_mkstr(js, "0", 1), str);
    setprop(js, arr, js_mkstr(js, "length", 6), tov(1));
  } else {
    setprop(js, arr, js_mkstr(js, "length", 6), tov(0));
  }
  return mkval(T_ARR, vdata(arr));
}

static jsval_t builtin_string_slice(struct js *js, jsval_t *args, int nargs) {
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
    } else {
      start = (jsoff_t) (d > dstr_len ? dstr_len : d);
    }
  }
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    double d = tod(args[1]);
    if (d < 0) {
      end = (jsoff_t) (d + dstr_len < 0 ? 0 : d + dstr_len);
    } else {
      end = (jsoff_t) (d > dstr_len ? dstr_len : d);
    }
  }
  
  if (start > end) start = end;
  size_t byte_start, byte_end;
  utf16_range_to_byte_range(str_ptr, byte_len, start, end, &byte_start, &byte_end);
  return js_mkstr(js, str_ptr + byte_start, byte_end - byte_start);
}

static jsval_t builtin_string_includes(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "includes called on non-string");
  if (nargs == 0) return mkval(T_BOOL, 0);
  jsval_t search = args[0];
  
  if (vtype(search) != T_STR) return mkval(T_BOOL, 0);
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
    if (memcmp(str_ptr + i, search_ptr, search_len) == 0) {
      return mkval(T_BOOL, 1);
    }
  }
  
  return mkval(T_BOOL, 0);
}

static jsval_t builtin_string_startsWith(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "startsWith called on non-string");
  if (nargs == 0) return mkval(T_BOOL, 0);
  jsval_t search = args[0];
  
  if (vtype(search) != T_STR) return mkval(T_BOOL, 0);
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  jsoff_t search_len, search_off = vstr(js, search, &search_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  const char *search_ptr = (char *) &js->mem[search_off];
  
  if (search_len > str_len) return mkval(T_BOOL, 0);
  if (search_len == 0) return mkval(T_BOOL, 1);
  
  return mkval(T_BOOL, memcmp(str_ptr, search_ptr, search_len) == 0 ? 1 : 0);
}

static jsval_t builtin_string_endsWith(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "endsWith called on non-string");
  if (nargs == 0) return mkval(T_BOOL, 0);
  jsval_t search = args[0];
  
  if (vtype(search) != T_STR) return mkval(T_BOOL, 0);
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  jsoff_t search_len, search_off = vstr(js, search, &search_len);
  
  const char *str_ptr = (char *) &js->mem[str_off];
  const char *search_ptr = (char *) &js->mem[search_off];
  
  if (search_len > str_len) return mkval(T_BOOL, 0);
  if (search_len == 0) return mkval(T_BOOL, 1);
  
  return mkval(T_BOOL, memcmp(str_ptr + str_len - search_len, search_ptr, search_len) == 0 ? 1 : 0);
}

static jsval_t builtin_string_replace(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_unwrapped = unwrap_primitive(js, js->this_val);
  jsval_t str = js_tostring_val(js, this_unwrapped);
  if (is_err(str)) return str;
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

        jsval_t cb_result = js_call(js, replacement, cb_args, nargs_cb);
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
        for (jsoff_t ri = 0; ri < repl_len; ) {
          if (repl_ptr[ri] == '$' && ri + 1 < repl_len) {
            char next = repl_ptr[ri + 1];
            if (next == '$') {
              ENSURE_RESULT_CAP(1);
              result[result_len++] = '$';
              ri += 2;
            } else if (next == '&') {
              PCRE2_SIZE mlen = match_end - match_start;
              ENSURE_RESULT_CAP(mlen);
              memcpy(result + result_len, str_ptr + match_start, mlen);
              result_len += mlen;
              ri += 2;
            } else if (next == '`') {
              ENSURE_RESULT_CAP(match_start);
              memcpy(result + result_len, str_ptr, match_start);
              result_len += match_start;
              ri += 2;
            } else if (next == '\'') {
              PCRE2_SIZE after_len = str_len - match_end;
              ENSURE_RESULT_CAP(after_len);
              memcpy(result + result_len, str_ptr + match_end, after_len);
              result_len += after_len;
              ri += 2;
            } else if (next >= '0' && next <= '9') {
              int group_num = next - '0';
              ri += 2;
              if (ri < repl_len && repl_ptr[ri] >= '0' && repl_ptr[ri] <= '9') {
                int second_digit = repl_ptr[ri] - '0';
                int two_digit = group_num * 10 + second_digit;
                if (two_digit <= (int)capture_count) {
                  group_num = two_digit;
                  ri++;
                }
              }
              if (group_num > 0 && group_num <= (int)capture_count) {
                PCRE2_SIZE cap_start = ovector[2*group_num];
                PCRE2_SIZE cap_end = ovector[2*group_num+1];
                if (cap_start != PCRE2_UNSET) {
                  PCRE2_SIZE cap_len = cap_end - cap_start;
                  ENSURE_RESULT_CAP(cap_len);
                  memcpy(result + result_len, str_ptr + cap_start, cap_len);
                  result_len += cap_len;
                }
              } else {
                ENSURE_RESULT_CAP(2);
                result[result_len++] = '$';
                result[result_len++] = next;
              }
            } else {
              ENSURE_RESULT_CAP(1);
              result[result_len++] = repl_ptr[ri++];
            }
          } else {
            ENSURE_RESULT_CAP(1);
            result[result_len++] = repl_ptr[ri++];
          }
        }
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
          jsval_t cb_result = js_call(js, replacement, cb_args, 1);
          
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

static jsval_t builtin_string_replaceAll(struct js *js, jsval_t *args, int nargs) {
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

static size_t js_to_pcre2_pattern(const char *src, size_t src_len, char *dst, size_t dst_size) {
  size_t di = 0;
  for (size_t si = 0; si < src_len && di < dst_size - 1; si++) {
    if (src[si] == '\\' && si + 1 < src_len) {
      char next = src[si + 1];
      
      if (next == 'u' && si + 5 < src_len) {
        bool valid = true;
        for (int i = 0; i < 4; i++) {
          char c = src[si + 2 + i];
          if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            valid = false;
            break;
          }
        }
        if (valid && di + 8 < dst_size) {
          dst[di++] = '\\';
          dst[di++] = 'x';
          dst[di++] = '{';
          dst[di++] = src[si + 2];
          dst[di++] = src[si + 3];
          dst[di++] = src[si + 4];
          dst[di++] = src[si + 5];
          dst[di++] = '}';
          si += 5;
          continue;
        }
      }
      
      if (next == '0' && (si + 2 >= src_len || src[si + 2] < '0' || src[si + 2] > '9')) {
        if (di + 5 < dst_size) {
          dst[di++] = '\\';
          dst[di++] = 'x';
          dst[di++] = '{';
          dst[di++] = '0';
          dst[di++] = '}';
          si += 1;
          continue;
        }
      }
    }
    dst[di++] = src[si];
  }
  dst[di] = '\0';
  return di;
}

static jsval_t do_regex_match_pcre2(
  struct js *js, const char *pattern_ptr, jsoff_t pattern_len, 
  const char *str_ptr, jsoff_t str_len, 
  bool global_flag, bool ignore_case, bool multiline
) {
  char pcre2_pattern[512];
  size_t pcre2_len = js_to_pcre2_pattern(pattern_ptr, pattern_len, pcre2_pattern, sizeof(pcre2_pattern));

  uint32_t options = PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_UNSET_BACKREF;
  if (ignore_case) options |= PCRE2_CASELESS;
  if (multiline) options |= PCRE2_MULTILINE;

  int errcode;
  PCRE2_SIZE erroffset;
  pcre2_code *re = pcre2_compile((PCRE2_SPTR)pcre2_pattern, pcre2_len, options, &errcode, &erroffset, NULL);
  if (re == NULL) return js_mknull();

  pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(re, NULL);
  uint32_t capture_count;
  pcre2_pattern_info(re, PCRE2_INFO_CAPTURECOUNT, &capture_count);

  jsval_t result_arr = js_mkarr(js);
  if (is_err(result_arr)) {
    pcre2_match_data_free(match_data);
    pcre2_code_free(re);
    return result_arr;
  }

  PCRE2_SIZE pos = 0;
  int match_count = 0;

  while (pos <= str_len) {
    int rc = pcre2_match(re, (PCRE2_SPTR)str_ptr, str_len, pos, 0, match_data, NULL);
    if (rc < 0) break;

    PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
    PCRE2_SIZE match_start = ovector[0];
    PCRE2_SIZE match_end = ovector[1];

    if (global_flag) {
      jsval_t match_str = js_mkstr(js, str_ptr + match_start, match_end - match_start);
      if (is_err(match_str)) {
        pcre2_match_data_free(match_data);
        pcre2_code_free(re);
        return match_str;
      }
      js_arr_push(js, result_arr, match_str);
    } else {
      for (uint32_t i = 0; i <= capture_count; i++) {
        PCRE2_SIZE start = ovector[2*i];
        PCRE2_SIZE end = ovector[2*i+1];
        if (start == PCRE2_UNSET) {
          js_arr_push(js, result_arr, js_mkundef());
        } else {
          jsval_t match_str = js_mkstr(js, str_ptr + start, end - start);
          if (is_err(match_str)) {
            pcre2_match_data_free(match_data);
            pcre2_code_free(re);
            return match_str;
          }
          js_arr_push(js, result_arr, match_str);
        }
      }
      setprop(js, result_arr, js_mkstr(js, "index", 5), tov((double)match_start));
    }
    match_count++;

    if (!global_flag) break;

    if (match_start == match_end) {
      pos = match_end + 1;
    } else { pos = match_end; }
  }

  pcre2_match_data_free(match_data);
  pcre2_code_free(re);

  if (match_count == 0) return js_mknull();
  return result_arr;
}

static jsval_t builtin_string_match(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_unwrapped = unwrap_primitive(js, js->this_val);
  jsval_t str = js_tostring_val(js, this_unwrapped);
  if (is_err(str)) return str;
  if (nargs < 1) return js_mknull();

  jsval_t pattern = args[0];
  const char *pattern_ptr = NULL;
  jsoff_t pattern_len = 0;
  bool global_flag = false;
  bool ignore_case = false;
  bool multiline = false;

  if (vtype(pattern) == T_OBJ) {
    jsoff_t source_off = lkp(js, pattern, "source", 6);
    if (source_off == 0) return js_mknull();

    jsval_t source_val = resolveprop(js, mkval(T_PROP, source_off));
    if (vtype(source_val) != T_STR) return js_mknull();

    jsoff_t poff;
    poff = vstr(js, source_val, &pattern_len);
    pattern_ptr = (char *) &js->mem[poff];

    jsoff_t flags_off = lkp(js, pattern, "flags", 5);
    if (flags_off != 0) {
      jsval_t flags_val = resolveprop(js, mkval(T_PROP, flags_off));
      if (vtype(flags_val) == T_STR) {
        jsoff_t flen, foff = vstr(js, flags_val, &flen);
        const char *flags_str = (char *) &js->mem[foff];
        for (jsoff_t i = 0; i < flen; i++) {
          if (flags_str[i] == 'g') global_flag = true;
          if (flags_str[i] == 'i') ignore_case = true;
          if (flags_str[i] == 'm') multiline = true;
        }
      }
    }
  } else if (vtype(pattern) == T_STR) {
    jsoff_t poff;
    poff = vstr(js, pattern, &pattern_len);
    pattern_ptr = (char *) &js->mem[poff];
  } else {
    return js_mknull();
  }

  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];

  jsval_t result = do_regex_match_pcre2(js, pattern_ptr, pattern_len, str_ptr, str_len, global_flag, ignore_case, multiline);
  
  if (!global_flag && vtype(result) == T_ARR) {
    setprop(js, result, js_mkstr(js, "input", 5), str);
  }

  return result;
}

static jsval_t builtin_string_template(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t builtin_string_charCodeAt(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "charCodeAt called on non-string");
  
  double idx_d = nargs < 1 ? 0.0 : js_to_number(js, args[0]);
  if (isnan(idx_d)) idx_d = 0.0;
  if (isinf(idx_d) || idx_d > (double)LONG_MAX) return tov(JS_NAN);
  
  long idx_l = (long) idx_d;
  if (idx_l < 0) return tov(JS_NAN);
  
  jsoff_t byte_len = offtolen(loadoff(js, (jsoff_t) vdata(str)));
  jsoff_t str_off = (jsoff_t) vdata(str) + sizeof(jsoff_t);
  const char *str_data = (const char *)&js->mem[str_off];
  
  uint32_t code_unit = utf16_code_unit_at(str_data, byte_len, idx_l);
  if (code_unit == 0xFFFFFFFF) return tov(JS_NAN);
  
  return tov((double) code_unit);
}

static jsval_t builtin_string_codePointAt(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "codePointAt called on non-string");
  
  double idx_d = nargs < 1 ? 0.0 : js_to_number(js, args[0]);
  if (isnan(idx_d)) idx_d = 0.0;
  if (isinf(idx_d) || idx_d > (double)LONG_MAX) return js_mkundef();
  
  long idx_l = (long) idx_d;
  if (idx_l < 0) return js_mkundef();
  
  jsoff_t byte_len = offtolen(loadoff(js, (jsoff_t) vdata(str)));
  jsoff_t str_off = (jsoff_t) vdata(str) + sizeof(jsoff_t);
  const char *str_data = (const char *)&js->mem[str_off];
  
  uint32_t cp = utf16_codepoint_at(str_data, byte_len, idx_l);
  if (cp == 0xFFFFFFFF) return js_mkundef();
  
  return tov((double) cp);
}

static jsval_t builtin_string_toLowerCase(struct js *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "toLowerCase called on non-string");
  
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  jsval_t result = js_mkstr(js, NULL, str_len);
  if (is_err(result)) return result;
  
  jsoff_t result_len, result_off = vstr(js, result, &result_len);
  char *result_ptr = (char *) &js->mem[result_off];
  
  for (jsoff_t i = 0; i < str_len; i++) {
    char ch = str_ptr[i];
    result_ptr[i] = (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
  }
  
  return result;
}

static jsval_t builtin_string_toUpperCase(struct js *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "toUpperCase called on non-string");
  
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  jsval_t result = js_mkstr(js, NULL, str_len);
  if (is_err(result)) return result;
  
  jsoff_t result_len, result_off = vstr(js, result, &result_len);
  char *result_ptr = (char *) &js->mem[result_off];
  
  for (jsoff_t i = 0; i < str_len; i++) {
    char ch = str_ptr[i];
    result_ptr[i] = (ch >= 'a' && ch <= 'z') ? ch - 32 : ch;
  }
  
  return result;
}

static jsval_t builtin_string_trim(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t builtin_string_trimStart(struct js *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "trimStart called on non-string");
  
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  
  jsoff_t start = 0;
  while (start < str_len && is_space(str_ptr[start])) start++;
  
  return js_mkstr(js, str_ptr + start, str_len - start);
}

static jsval_t builtin_string_trimEnd(struct js *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "trimEnd called on non-string");
  
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  
  jsoff_t end = str_len;
  while (end > 0 && is_space(str_ptr[end - 1])) end--;
  
  return js_mkstr(js, str_ptr, end);
}

static jsval_t builtin_string_repeat(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t builtin_string_padStart(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "padStart called on non-string");
  if (nargs < 1 || vtype(args[0]) != T_NUM) return str;
  
  jsoff_t target_len = (jsoff_t) tod(args[0]);
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  
  if (target_len <= str_len) return str;
  
  const char *pad_str = " ";
  jsoff_t pad_len = 1;
  if (nargs >= 2 && vtype(args[1]) == T_STR) {
    pad_len = vstr(js, args[1], &pad_len);
    pad_str = (char *) &js->mem[pad_len];
    pad_len = offtolen(loadoff(js, (jsoff_t) vdata(args[1])));
  }
  
  if (pad_len == 0) return str;
  
  jsoff_t fill_len = target_len - str_len;
  jsval_t result = js_mkstr(js, NULL, target_len);
  if (is_err(result)) return result;
  
  jsoff_t result_len, result_off = vstr(js, result, &result_len);
  char *result_ptr = (char *) &js->mem[result_off];
  
  jsoff_t pos = 0;
  while (pos < fill_len) {
    jsoff_t copy_len = (fill_len - pos < pad_len) ? fill_len - pos : pad_len;
    memcpy(result_ptr + pos, pad_str, copy_len);
    pos += copy_len;
  }
  memcpy(result_ptr + fill_len, str_ptr, str_len);
  
  return result;
}

static jsval_t builtin_string_padEnd(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "padEnd called on non-string");
  if (nargs < 1 || vtype(args[0]) != T_NUM) return str;
  
  jsoff_t target_len = (jsoff_t) tod(args[0]);
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  
  if (target_len <= str_len) return str;
  
  const char *pad_str = " ";
  jsoff_t pad_len = 1;
  if (nargs >= 2 && vtype(args[1]) == T_STR) {
    pad_len = vstr(js, args[1], &pad_len);
    pad_str = (char *) &js->mem[pad_len];
    pad_len = offtolen(loadoff(js, (jsoff_t) vdata(args[1])));
  }
  
  if (pad_len == 0) return str;
  
  jsval_t result = js_mkstr(js, NULL, target_len);
  if (is_err(result)) return result;
  
  jsoff_t result_len, result_off = vstr(js, result, &result_len);
  char *result_ptr = (char *) &js->mem[result_off];
  
  memcpy(result_ptr, str_ptr, str_len);
  jsoff_t pos = str_len;
  while (pos < target_len) {
    jsoff_t copy_len = (target_len - pos < pad_len) ? target_len - pos : pad_len;
    memcpy(result_ptr + pos, pad_str, copy_len);
    pos += copy_len;
  }
  
  return result;
}

static jsval_t builtin_string_charAt(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "charAt called on non-string");
  
  double idx_d = nargs < 1 ? 0.0 : js_to_number(js, args[0]);
  if (isnan(idx_d)) idx_d = 0;
  else if (idx_d < 0) idx_d = -floor(-idx_d);
  else idx_d = floor(idx_d);
  if (idx_d < 0 || isinf(idx_d)) return js_mkstr(js, "", 0);
  
  jsoff_t idx = (jsoff_t) idx_d;
  jsoff_t byte_len = offtolen(loadoff(js, (jsoff_t) vdata(str)));
  jsoff_t str_off = (jsoff_t) vdata(str) + sizeof(jsoff_t);
  const char *str_data = (const char *)&js->mem[str_off];
  
  size_t char_bytes;
  int byte_offset = utf16_index_to_byte_offset(str_data, byte_len, idx, &char_bytes);
  if (byte_offset < 0) return js_mkstr(js, "", 0);
  
  return js_mkstr(js, str_data + byte_offset, char_bytes);
}

static jsval_t builtin_string_at(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "at called on non-string");
  
  double idx_d = nargs < 1 ? 0.0 : js_to_number(js, args[0]);
  if (isnan(idx_d) || isinf(idx_d)) return js_mkundef();

  jsoff_t byte_len = offtolen(loadoff(js, (jsoff_t) vdata(str)));
  jsoff_t str_off = (jsoff_t) vdata(str) + sizeof(jsoff_t);
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

static jsval_t builtin_string_localeCompare(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t builtin_string_lastIndexOf(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t builtin_string_concat(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t builtin_string_fromCharCode(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t builtin_string_fromCodePoint(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t builtin_number_toString(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t builtin_number_toFixed(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t builtin_number_toPrecision(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t builtin_number_toExponential(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t builtin_number_valueOf(struct js *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  jsval_t num = unwrap_primitive(js, js->this_val);
  if (vtype(num) != T_NUM) return js_mkerr(js, "valueOf called on non-number");
  return num;
}

static jsval_t builtin_number_toLocaleString(struct js *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  jsval_t num = unwrap_primitive(js, js->this_val);
  if (vtype(num) != T_NUM) return js_mkerr(js, "toLocaleString called on non-number");
  char buf[64];
  strnum(num, buf, sizeof(buf));
  return js_mkstr(js, buf, strlen(buf));
}

static jsval_t builtin_string_valueOf(struct js *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  jsval_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "valueOf called on non-string");
  return str;
}

static jsval_t builtin_string_toString(struct js *js, jsval_t *args, int nargs) {
  return builtin_string_valueOf(js, args, nargs);
}

static jsval_t builtin_boolean_valueOf(struct js *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  jsval_t b = unwrap_primitive(js, js->this_val);
  if (vtype(b) != T_BOOL) return js_mkerr(js, "valueOf called on non-boolean");
  return b;
}

static jsval_t builtin_boolean_toString(struct js *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  jsval_t b = unwrap_primitive(js, js->this_val);
  if (vtype(b) != T_BOOL) return js_mkerr(js, "toString called on non-boolean");
  return vdata(b) ? js_mkstr(js, "true", 4) : js_mkstr(js, "false", 5);
}

static jsval_t builtin_parseInt(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t builtin_parseFloat(struct js *js, jsval_t *args, int nargs) {
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

static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static jsval_t builtin_btoa(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "btoa requires 1 argument");
  
  jsval_t str_val = args[0];
  if (vtype(str_val) != T_STR) {
    const char *str = js_str(js, str_val);
    str_val = js_mkstr(js, str, strlen(str));
  }
  
  jsoff_t str_len, str_off = vstr(js, str_val, &str_len);
  const char *str = (char *) &js->mem[str_off];
  
  for (jsoff_t i = 0; i < str_len; i++) {
    if ((unsigned char)str[i] > 255) {
      return js_mkerr(js, "btoa: character out of range");
    }
  }
  
  size_t out_len = ((str_len + 2) / 3) * 4;
  char *out = (char *)ant_calloc(out_len + 1);
  if (!out) return js_mkerr(js, "out of memory");
  
  size_t i = 0, j = 0;
  while (i < str_len) {
    size_t remaining = str_len - i;
    uint32_t a = (unsigned char)str[i++];
    uint32_t b = (remaining > 1) ? (unsigned char)str[i++] : 0;
    uint32_t c = (remaining > 2) ? (unsigned char)str[i++] : 0;
    uint32_t triple = (a << 16) | (b << 8) | c;
    
    out[j++] = base64_chars[(triple >> 18) & 0x3F];
    out[j++] = base64_chars[(triple >> 12) & 0x3F];
    out[j++] = (remaining > 1) ? base64_chars[(triple >> 6) & 0x3F] : '=';
    out[j++] = (remaining > 2) ? base64_chars[triple & 0x3F] : '=';
  }
  out[j] = '\0';
  
  jsval_t result = js_mkstr(js, out, j);
  free(out);
  return result;
}

static const int8_t decode_table[256] = {
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
  52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
  -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
  15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
  -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
  41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

static jsval_t builtin_atob(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "atob requires 1 argument");
  
  jsval_t str_val = args[0];
  if (vtype(str_val) != T_STR) {
    const char *str = js_str(js, str_val);
    str_val = js_mkstr(js, str, strlen(str));
  }
  
  jsoff_t str_len, str_off = vstr(js, str_val, &str_len);
  const char *str = (char *) &js->mem[str_off];
  
  if (str_len == 0) return js_mkstr(js, "", 0);
  if (str_len % 4 != 0) return js_mkerr(js, "atob: invalid base64 string");
  
  size_t out_len = (str_len / 4) * 3;
  if (str_len > 0 && str[str_len - 1] == '=') out_len--;
  if (str_len > 1 && str[str_len - 2] == '=') out_len--;
  
  char *out = (char *)ant_calloc(out_len + 1);
  if (!out) return js_mkerr(js, "out of memory");
  
  size_t i = 0, j = 0;
  while (i < str_len) {
    int8_t a = decode_table[(unsigned char)str[i++]];
    int8_t b = decode_table[(unsigned char)str[i++]];
    int8_t c = (str[i] == '=') ? 0 : decode_table[(unsigned char)str[i]]; i++;
    int8_t d = (str[i] == '=') ? 0 : decode_table[(unsigned char)str[i]]; i++;
    
    if (a < 0 || b < 0 || (str[i-2] != '=' && c < 0) || (str[i-1] != '=' && d < 0)) {
      free(out);
      return js_mkerr(js, "atob: invalid character in base64 string");
    }
    
    uint32_t triple = ((uint32_t)a << 18) | ((uint32_t)b << 12) | ((uint32_t)c << 6) | (uint32_t)d;
    if (j < out_len) out[j++] = (triple >> 16) & 0xFF;
    if (j < out_len) out[j++] = (triple >> 8) & 0xFF;
    if (j < out_len) out[j++] = triple & 0xFF;
  }
  
  jsval_t result = js_mkstr(js, out, out_len);
  free(out);
  return result;
}

static jsval_t builtin_resolve_internal(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_reject_internal(struct js *js, jsval_t *args, int nargs);
static void resolve_promise(struct js *js, jsval_t p, jsval_t val);
static void reject_promise(struct js *js, jsval_t p, jsval_t val);

static size_t strpromise(struct js *js, jsval_t value, char *buf, size_t len) {
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

static uint32_t get_promise_id(struct js *js, jsval_t p) {
  jsval_t p_obj = mkval(T_OBJ, vdata(p));
  jsval_t pid_val = get_slot(js, p_obj, SLOT_PID);
  if (vtype(pid_val) == T_UNDEF) return 0;
  return (uint32_t)tod(pid_val);
}

static jsval_t mkpromise(struct js *js) {
  jsval_t obj = mkobj(js, 0);
  if (is_err(obj)) return obj;
  
  uint32_t pid = next_promise_id++;
  set_slot(js, obj, SLOT_PID, tov((double)pid));
  promise_data_entry_t *pd = get_promise_data(pid, true);
  if (pd) pd->obj_offset = (jsoff_t)vdata(obj);
  
  return mkval(T_PROMISE, vdata(obj));
}

static inline void trigger_handlers(struct js *js, jsval_t p) {
  uint32_t pid = get_promise_id(js, p);
  queue_promise_trigger(pid);
}

void js_process_promise_handlers(struct js *js, uint32_t pid) {
  promise_data_entry_t *pd = get_promise_data(pid, false);
  if (!pd) return;
  
  int state = pd->state;
  jsval_t val = pd->value;
  
  unsigned int len = utarray_len(pd->handlers);
  for (unsigned int i = 0; i < len; i++) {
    promise_handler_t *h = (promise_handler_t *)utarray_eltptr(pd->handlers, i);
    jsval_t handler = (state == 1) ? h->onFulfilled : h->onRejected;
    
    if (vtype(handler) == T_FUNC || vtype(handler) == T_CFUNC) {
      jsval_t res;
      if (vtype(handler) == T_CFUNC) {
        jsval_t (*fn)(struct js *, jsval_t *, int) = (jsval_t(*)(struct js *, jsval_t *, int)) vdata(handler);
        res = fn(js, &val, 1);
      } else {
        jsval_t call_args[] = { val };
        res = js_call(js, handler, call_args, 1);
      }
       
      if (is_err(res)) {
        jsval_t reject_val = js->thrown_value;
        if (vtype(reject_val) == T_UNDEF) {
          reject_val = js_mkstr(js, js->errmsg, strlen(js->errmsg));
        }
        js->thrown_value = js_mkundef();
        reject_promise(js, h->nextPromise, reject_val);
      } else resolve_promise(js, h->nextPromise, res);
    } else {
      if (state == 1) resolve_promise(js, h->nextPromise, val);
      else reject_promise(js, h->nextPromise, val);
    }
  }

  utarray_clear(pd->handlers);
}

static void resolve_promise(struct js *js, jsval_t p, jsval_t val) {
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
    set_slot(js, res_obj, SLOT_CFUNC, js_mkfun(builtin_resolve_internal));
    set_slot(js, res_obj, SLOT_DATA, p);
    jsval_t res_fn = mkval(T_FUNC, vdata(res_obj));
    
    jsval_t rej_obj = mkobj(js, 0);
    set_slot(js, rej_obj, SLOT_CFUNC, js_mkfun(builtin_reject_internal));
    set_slot(js, rej_obj, SLOT_DATA, p);
    jsval_t rej_fn = mkval(T_FUNC, vdata(rej_obj));
    
    jsval_t call_args[] = { res_fn, rej_fn };
    jsval_t then_prop = js_get(js, val, "then");
    
    if (vtype(then_prop) == T_FUNC || vtype(then_prop) == T_CFUNC) {
      (void)js_call_with_this(js, then_prop, val, call_args, 2); return;
    }
  }

  pd->state = 1;
  pd->value = val;
  trigger_handlers(js, p);
}

static void reject_promise(struct js *js, jsval_t p, jsval_t val) {
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

static jsval_t builtin_resolve_internal(struct js *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t p = get_slot(js, me, SLOT_DATA);
  if (vtype(p) != T_PROMISE) return js_mkundef();
  resolve_promise(js, p, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static jsval_t builtin_reject_internal(struct js *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t p = get_slot(js, me, SLOT_DATA);
  if (vtype(p) != T_PROMISE) return js_mkundef();
  reject_promise(js, p, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static jsval_t builtin_Promise(struct js *js, jsval_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Promise constructor cannot be invoked without 'new'");
  }
  
  if (nargs == 0 || (vtype(args[0]) != T_FUNC && vtype(args[0]) != T_CFUNC)) {
    const char *val_str = nargs == 0 ? "undefined" : js_str(js, args[0]);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Promise resolver %s is not a function", val_str);
  }
  
  jsval_t p = mkpromise(js);
  jsval_t new_target = js->new_target;
  
  if (vtype(new_target) == T_FUNC) {
    jsoff_t proto_off = lkp_interned(js, mkval(T_OBJ, vdata(new_target)), INTERN_PROTOTYPE, 9);
    jsval_t subclass_proto = proto_off ? resolveprop(js, mkval(T_PROP, proto_off)) : js_mkundef();
    
    if (vtype(subclass_proto) == T_OBJ) {
      jsval_t p_obj = mkval(T_OBJ, vdata(p));
      set_slot(js, p_obj, SLOT_PROTO, subclass_proto);
      set_slot(js, p_obj, SLOT_CTOR, new_target);
    }
  }
  
  jsval_t res_obj = mkobj(js, 0);
  
  set_slot(js, res_obj, SLOT_CFUNC, js_mkfun(builtin_resolve_internal));
  set_slot(js, res_obj, SLOT_DATA, p);
  
  jsval_t res_fn = mkval(T_FUNC, vdata(res_obj));
  jsval_t rej_obj = mkobj(js, 0);
  
  set_slot(js, rej_obj, SLOT_CFUNC, js_mkfun(builtin_reject_internal));
  set_slot(js, rej_obj, SLOT_DATA, p);
  
  jsval_t rej_fn = mkval(T_FUNC, vdata(rej_obj));
  jsval_t exec_args[] = { res_fn, rej_fn };
  js_call(js, args[0], exec_args, 2);
  
  return p;
}

static jsval_t builtin_Promise_resolve(struct js *js, jsval_t *args, int nargs) {
  jsval_t val = nargs > 0 ? args[0] : js_mkundef();
  if (vtype(val) == T_PROMISE) return val;
  jsval_t p = mkpromise(js);
  resolve_promise(js, p, val);
  return p;
}

static jsval_t builtin_Promise_reject(struct js *js, jsval_t *args, int nargs) {
  jsval_t val = nargs > 0 ? args[0] : js_mkundef();
  jsval_t p = mkpromise(js);
  reject_promise(js, p, val);
  return p;
}

static jsval_t builtin_promise_then(struct js *js, jsval_t *args, int nargs) {
  jsval_t p = js->this_val;
  if (vtype(p) != T_PROMISE) return js_mkerr(js, "not a promise");
  
  jsval_t nextP = mkpromise(js);
  
  jsval_t p_proto = get_slot(js, mkval(T_OBJ, vdata(p)), SLOT_PROTO);
  if (vtype(p_proto) == T_OBJ) {
    set_slot(js, mkval(T_OBJ, vdata(nextP)), SLOT_PROTO, p_proto);
    jsval_t p_ctor = get_slot(js, mkval(T_OBJ, vdata(p)), SLOT_CTOR);
    if (vtype(p_ctor) == T_FUNC) set_slot(js, mkval(T_OBJ, vdata(nextP)), SLOT_CTOR, p_ctor);
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

static jsval_t builtin_promise_catch(struct js *js, jsval_t *args, int nargs) {
  jsval_t args_then[] = { js_mkundef(), nargs > 0 ? args[0] : js_mkundef() };
  return builtin_promise_then(js, args_then, 2);
}

static jsval_t finally_value_thunk(struct js *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  return get_slot(js, me, SLOT_DATA);
}

static jsval_t finally_thrower(struct js *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t reason = get_slot(js, me, SLOT_DATA);
  jsval_t rejected = js_mkpromise(js);
  js_reject_promise(js, rejected, reason);
  return rejected;
}

static jsval_t finally_identity_reject(struct js *js, jsval_t *args, int nargs) {
  jsval_t reason = nargs > 0 ? args[0] : js_mkundef();
  jsval_t rejected = js_mkpromise(js);
  js_reject_promise(js, rejected, reason);
  return rejected;
}

static jsval_t finally_fulfilled_wrapper(struct js *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t callback = get_slot(js, me, SLOT_DATA);
  jsval_t value = nargs > 0 ? args[0] : js_mkundef();
  
  jsval_t result = js_mkundef();
  if (vtype(callback) == T_FUNC || vtype(callback) == T_CFUNC) {
    result = js_call(js, callback, NULL, 0);
    if (is_err(result)) return result;
  }
  
  if (vtype(result) == T_PROMISE || (vtype(result) == T_OBJ && vtype(js_get(js, result, "then")) == T_FUNC)) {
    jsval_t thunk_obj = mkobj(js, 0);
    set_slot(js, thunk_obj, SLOT_CFUNC, js_mkfun(finally_value_thunk));
    set_slot(js, thunk_obj, SLOT_DATA, value);
    jsval_t thunk_fn = mkval(T_FUNC, vdata(thunk_obj));
    
    jsval_t identity_rej_fn = js_mkfun(finally_identity_reject);
    
    jsval_t then_fn = js_get(js, result, "then");
    jsval_t call_args[] = { thunk_fn, identity_rej_fn };
    return js_call_with_this(js, then_fn, result, call_args, 2);
  }
  
  return value;
}

static jsval_t finally_rejected_wrapper(struct js *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t callback = get_slot(js, me, SLOT_DATA);
  jsval_t reason = nargs > 0 ? args[0] : js_mkundef();
  
  jsval_t result = js_mkundef();
  if (vtype(callback) == T_FUNC || vtype(callback) == T_CFUNC) {
    result = js_call(js, callback, NULL, 0);
    if (is_err(result)) return result;
  }
  
  if (vtype(result) == T_PROMISE || (vtype(result) == T_OBJ && vtype(js_get(js, result, "then")) == T_FUNC)) {
    jsval_t thrower_obj = mkobj(js, 0);
    set_slot(js, thrower_obj, SLOT_CFUNC, js_mkfun(finally_thrower));
    set_slot(js, thrower_obj, SLOT_DATA, reason);
    
    jsval_t thrower_fn = mkval(T_FUNC, vdata(thrower_obj));
    jsval_t identity_rej_fn = js_mkfun(finally_identity_reject);
    
    jsval_t then_prop = js_get(js, result, "then");
    jsval_t call_args[] = { thrower_fn, identity_rej_fn };
    
    return js_call_with_this(js, then_prop, result, call_args, 2);
  }
  
  jsval_t rejected = js_mkpromise(js);
  js_reject_promise(js, rejected, reason);
  return rejected;
}

static jsval_t builtin_promise_finally(struct js *js, jsval_t *args, int nargs) {
  jsval_t callback = nargs > 0 ? args[0] : js_mkundef();
  
  jsval_t fulfilled_obj = mkobj(js, 0);
  set_slot(js, fulfilled_obj, SLOT_CFUNC, js_mkfun(finally_fulfilled_wrapper));
  set_slot(js, fulfilled_obj, SLOT_DATA, callback);
  jsval_t fulfilled_fn = mkval(T_FUNC, vdata(fulfilled_obj));
  
  jsval_t rejected_obj = mkobj(js, 0);
  set_slot(js, rejected_obj, SLOT_CFUNC, js_mkfun(finally_rejected_wrapper));
  set_slot(js, rejected_obj, SLOT_DATA, callback);
  jsval_t rejected_fn = mkval(T_FUNC, vdata(rejected_obj));
  
  jsval_t args_then[] = { fulfilled_fn, rejected_fn };
  return builtin_promise_then(js, args_then, 2);
}

static jsval_t builtin_Promise_try(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return builtin_Promise_resolve(js, args, 0);
  jsval_t fn = args[0];
  jsval_t *call_args = nargs > 1 ? &args[1] : NULL;
  int call_nargs = nargs > 1 ? nargs - 1 : 0;
  jsval_t res = js_call_with_this(js, fn, js_mkundef(), call_args, call_nargs);
  if (is_err(res)) {
    jsval_t reject_val = js->thrown_value;
    if (vtype(reject_val) == T_UNDEF) {
      reject_val = js_mkstr(js, js->errmsg, strlen(js->errmsg));
    }
    js->thrown_value = js_mkundef();
    jsval_t rej_args[] = { reject_val };
    return builtin_Promise_reject(js, rej_args, 1);
  }
  jsval_t res_args[] = { res };
  return builtin_Promise_resolve(js, res_args, 1);
}

static jsval_t builtin_Promise_all_resolve_handler(struct js *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t tracker = js_get(js, me, "tracker");
  jsval_t index_val = js_get(js, me, "index");
  
  int index = (int)tod(index_val);
  jsval_t value = nargs > 0 ? args[0] : js_mkundef();
  
  jsval_t results = js_get(js, tracker, "results");
  char idx[16];
  snprintf(idx, sizeof(idx), "%d", index);
  setprop(js, results, js_mkstr(js, idx, strlen(idx)), value);
  
  jsval_t remaining_val = js_get(js, tracker, "remaining");
  int remaining = (int)tod(remaining_val) - 1;
  setprop(js, tracker, js_mkstr(js, "remaining", 9), tov((double)remaining));
  
  if (remaining == 0) {
    jsval_t result_promise = get_slot(js, tracker, SLOT_DATA);
    resolve_promise(js, result_promise, mkval(T_ARR, vdata(results)));
  }
  
  return js_mkundef();
}

static jsval_t builtin_Promise_all_reject_handler(struct js *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t tracker = js_get(js, me, "tracker");
  jsval_t result_promise = get_slot(js, tracker, SLOT_DATA);
  
  jsval_t reason = nargs > 0 ? args[0] : js_mkundef();
  reject_promise(js, result_promise, reason);
  
  return js_mkundef();
}

static jsval_t builtin_Promise_all(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Promise.all requires an array");
  
  jsval_t arr = args[0];
  if (vtype(arr) != T_ARR) return js_mkerr(js, "Promise.all requires an array");
  
  jsoff_t len_off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  if (len_off == 0) return builtin_Promise_resolve(js, NULL, 0);
  jsval_t len_val = resolveprop(js, mkval(T_PROP, len_off));
  int len = (int)tod(len_val);
  
  if (len == 0) {
    jsval_t empty_arr = mkarr(js);
    setprop(js, empty_arr, js_mkstr(js, "length", 6), tov(0.0));
    jsval_t resolve_args[] = { mkval(T_ARR, vdata(empty_arr)) };
    return builtin_Promise_resolve(js, resolve_args, 1);
  }
  
  jsval_t result_promise = mkpromise(js);
  jsval_t tracker = mkobj(js, 0);
  
  setprop(js, tracker, js_mkstr(js, "remaining", 9), tov((double)len));
  setprop(js, tracker, js_mkstr(js, "results", 7), mkarr(js));
  set_slot(js, tracker, SLOT_DATA, result_promise);
  
  jsval_t results = resolveprop(js, js_get(js, tracker, "results"));
  setprop(js, results, js_mkstr(js, "length", 6), tov((double)len));
  
  for (int i = 0; i < len; i++) {
    char idx[16];
    snprintf(idx, sizeof(idx), "%d", i);
    jsval_t item = resolveprop(js, js_get(js, arr, idx));
    
    if (vtype(item) != T_PROMISE) {
      jsval_t wrap_args[] = { item };
      item = builtin_Promise_resolve(js, wrap_args, 1);
    }
    
    jsval_t resolve_obj = mkobj(js, 0);
    set_slot(js, resolve_obj, SLOT_CFUNC, js_mkfun(builtin_Promise_all_resolve_handler));
    setprop(js, resolve_obj, js_mkstr(js, "index", 5), tov((double)i));
    setprop(js, resolve_obj, js_mkstr(js, "tracker", 7), tracker);
    jsval_t resolve_fn = mkval(T_FUNC, vdata(resolve_obj));
    
    jsval_t reject_obj = mkobj(js, 0);
    set_slot(js, reject_obj, SLOT_CFUNC, js_mkfun(builtin_Promise_all_reject_handler));
    setprop(js, reject_obj, js_mkstr(js, "tracker", 7), tracker);
    jsval_t reject_fn = mkval(T_FUNC, vdata(reject_obj));
    
    jsval_t then_args[] = { resolve_fn, reject_fn };
    jsval_t saved_this = js->this_val;
    js->this_val = item;
    builtin_promise_then(js, then_args, 2);
    js->this_val = saved_this;
  }
  
  return result_promise;
}

static jsval_t builtin_Promise_race(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Promise.race requires an array");
  
  jsval_t arr = args[0];
  if (vtype(arr) != T_ARR) return js_mkerr(js, "Promise.race requires an array");
  
  jsoff_t len_off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  if (len_off == 0) return mkpromise(js);
  jsval_t len_val = resolveprop(js, mkval(T_PROP, len_off));
  int len = (int)tod(len_val);
  
  if (len == 0) return mkpromise(js);
  jsval_t result_promise = mkpromise(js);
  
  jsval_t resolve_obj = mkobj(js, 0);
  set_slot(js, resolve_obj, SLOT_CFUNC, js_mkfun(builtin_resolve_internal));
  set_slot(js, resolve_obj, SLOT_DATA, result_promise);
  jsval_t resolve_fn = mkval(T_FUNC, vdata(resolve_obj));
  
  jsval_t reject_obj = mkobj(js, 0);
  set_slot(js, reject_obj, SLOT_CFUNC, js_mkfun(builtin_reject_internal));
  set_slot(js, reject_obj, SLOT_DATA, result_promise);
  jsval_t reject_fn = mkval(T_FUNC, vdata(reject_obj));
  
  for (int i = 0; i < len; i++) {
    char idx[16];
    snprintf(idx, sizeof(idx), "%d", i);
    jsval_t item = resolveprop(js, js_get(js, arr, idx));
    
    if (vtype(item) != T_PROMISE) {
      resolve_promise(js, result_promise, item);
      return result_promise;
    }
    
    uint32_t item_pid = get_promise_id(js, item);
    promise_data_entry_t *pd = get_promise_data(item_pid, false);
    if (pd) {
      if (pd->state == 1) {
        resolve_promise(js, result_promise, pd->value);
        return result_promise;
      } else if (pd->state == 2) {
        reject_promise(js, result_promise, pd->value);
        return result_promise;
      }
    }
    
    jsval_t then_args[] = { resolve_fn, reject_fn };
    jsval_t saved_this = js->this_val;
    js->this_val = item;
    builtin_promise_then(js, then_args, 2);
    js->this_val = saved_this;
  }
  
  return result_promise;
}

static jsval_t mk_aggregate_error(struct js *js, jsval_t errors) {
  jsval_t args[] = { errors, js_mkstr(js, "All promises were rejected", 26) };
  jsoff_t off = lkp(js, js_glob(js), "AggregateError", 14);
  jsval_t ctor = off ? resolveprop(js, mkval(T_PROP, off)) : js_mkundef();
  return js_call(js, ctor, args, 2);
}

static bool promise_any_try_resolve(struct js *js, jsval_t tracker, jsval_t value) {
  if (js_truthy(js, js_get(js, tracker, "resolved"))) return false;
  js_set(js, tracker, "resolved", js_true);
  resolve_promise(js, get_slot(js, tracker, SLOT_DATA), value);
  return true;
}

static void promise_any_record_rejection(struct js *js, jsval_t tracker, int index, jsval_t reason) {
  jsval_t errors = resolveprop(js, js_get(js, tracker, "errors"));
  char idx[16];
  snprintf(idx, sizeof(idx), "%d", index);
  setprop(js, errors, js_mkstr(js, idx, strlen(idx)), reason);
  
  int remaining = (int)tod(js_get(js, tracker, "remaining")) - 1;
  js_set(js, tracker, "remaining", tov((double)remaining));
  
  if (remaining == 0) reject_promise(js, get_slot(js, tracker, SLOT_DATA), mk_aggregate_error(js, errors));
}

static jsval_t builtin_Promise_any_resolve_handler(struct js *js, jsval_t *args, int nargs) {
  jsval_t tracker = js_get(js, js->this_val, "tracker");
  promise_any_try_resolve(js, tracker, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static jsval_t builtin_Promise_any_reject_handler(struct js *js, jsval_t *args, int nargs) {
  jsval_t tracker = js_get(js, js->this_val, "tracker");
  if (js_truthy(js, js_get(js, tracker, "resolved"))) return js_mkundef();
  
  int index = (int)tod(js_get(js, js->this_val, "index"));
  promise_any_record_rejection(js, tracker, index, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static jsval_t builtin_Promise_any(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Promise.any requires an array");
  
  jsval_t arr = args[0];
  if (vtype(arr) != T_ARR) return js_mkerr(js, "Promise.any requires an array");
  
  jsoff_t len_off = lkp_interned(js, arr, INTERN_LENGTH, 6);
  int len = len_off ? (int)tod(resolveprop(js, mkval(T_PROP, len_off))) : 0;
  
  if (len == 0) {
    jsval_t reject_args[] = { mk_aggregate_error(js, mkarr(js)) };
    return builtin_Promise_reject(js, reject_args, 1);
  }
  
  jsval_t result_promise = mkpromise(js);
  jsval_t tracker = mkobj(js, 0);
  jsval_t errors = mkarr(js);
  
  set_slot(js, tracker, SLOT_DATA, result_promise);

  setprop(js, tracker, js_mkstr(js, "remaining", 9), tov((double)len));
  setprop(js, tracker, js_mkstr(js, "errors", 6), errors);
  setprop(js, tracker, js_mkstr(js, "resolved", 8), js_false);
  setprop(js, errors, js_mkstr(js, "length", 6), tov((double)len));
  
  for (int i = 0; i < len; i++) {
    char idx[16];
    snprintf(idx, sizeof(idx), "%d", i);
    jsval_t item = resolveprop(js, js_get(js, arr, idx));
    
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
    setprop(js, resolve_obj, js_mkstr(js, "tracker", 7), tracker);
    
    jsval_t reject_obj = mkobj(js, 0);
    set_slot(js, reject_obj, SLOT_CFUNC, js_mkfun(builtin_Promise_any_reject_handler));
    setprop(js, reject_obj, js_mkstr(js, "index", 5), tov((double)i));
    setprop(js, reject_obj, js_mkstr(js, "tracker", 7), tracker);
    
    jsval_t then_args[] = { mkval(T_FUNC, vdata(resolve_obj)), mkval(T_FUNC, vdata(reject_obj)) };
    jsval_t saved_this = js->this_val;
    js->this_val = item;
    builtin_promise_then(js, then_args, 2);
    js->this_val = saved_this;
  }
  
  return result_promise;
}

static jsval_t do_instanceof(struct js *js, jsval_t l, jsval_t r) {
  uint8_t ltype = vtype(l);
  uint8_t rtype = vtype(r);
  
  if (rtype != T_FUNC && rtype != T_CFUNC) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Right-hand side of 'instanceof' is not callable");
  }
  
  if (rtype == T_CFUNC) {
    // handle legacy T_CFUNC
    jsval_t (*fn)(struct js *, jsval_t *, int) = (jsval_t(*)(struct js *, jsval_t *, int)) vdata(r);
    if (fn == builtin_Object) {
      return mkval(T_BOOL, ltype == T_OBJ ? 1 : 0);
    } else if (fn == builtin_Function) {
      return mkval(T_BOOL, (ltype == T_FUNC || ltype == T_CFUNC) ? 1 : 0);
    } else if (fn == builtin_String) {
      return mkval(T_BOOL, ltype == T_STR ? 1 : 0);
    } else if (fn == builtin_Number) {
      return mkval(T_BOOL, ltype == T_NUM ? 1 : 0);
    } else if (fn == builtin_Boolean) {
      return mkval(T_BOOL, ltype == T_BOOL ? 1 : 0);
    } else if (fn == builtin_Array) {
      return mkval(T_BOOL, ltype == T_ARR ? 1 : 0);
    } else if (fn == builtin_Promise) {
      return mkval(T_BOOL, ltype == T_PROMISE ? 1 : 0);
    }
    return mkval(T_BOOL, 0);
  }
  
  jsval_t func_obj = mkval(T_OBJ, vdata(r));
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
  
  jsval_t current = get_proto(js, l);
  int depth = 0;
  const int MAX_DEPTH = 32;
  
  while (vtype(current) != T_NULL && depth < MAX_DEPTH) {
    if (vdata(current) == vdata(ctor_proto)) return mkval(T_BOOL, 1);
    current = get_proto(js, current);
    depth++;
  }
  
  return mkval(T_BOOL, 0);
}

static jsval_t do_in(struct js *js, jsval_t l, jsval_t r) {
  jsoff_t prop_len;
  const char *prop_name;
  char num_buf[32];
  
  if (vtype(l) == T_STR) {
    jsoff_t prop_off = vstr(js, l, &prop_len);
    prop_name = (char *) &js->mem[prop_off];
  } else if (vtype(l) == T_NUM) {
    prop_len = (jsoff_t) strnum(l, num_buf, sizeof(num_buf));
    prop_name = num_buf;
  } else {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot use 'in' operator to search for '%s' in non-object", js_str(js, l));
  }
  
  if (vtype(r) != T_OBJ && vtype(r) != T_ARR && vtype(r) != T_FUNC) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot use 'in' operator to search for '%.*s' in non-object", (int)prop_len, prop_name);
  }
  
  if (is_proxy(js, r)) {
    jsval_t result = proxy_has(js, r, prop_name, prop_len);
    if (is_err(result)) return result;
    return js_bool(js_truthy(js, result));
  }
  
  jsoff_t found = lkp_proto(js, r, prop_name, prop_len);
  return mkval(T_BOOL, found != 0 ? 1 : 0);
}

static char *esm_get_extension(const char *path) {
  const char *dot = strrchr(path, '.');
  const char *slash = strrchr(path, '/');
  
  if (dot && (!slash || dot > slash)) {
    return strdup(dot);
  }
  return strdup(".js");
}

static char *esm_try_resolve(const char *dir, const char *spec, const char *suffix) {
  char path[PATH_MAX];
  snprintf(path, PATH_MAX, "%s/%s%s", dir, spec, suffix);
  char *resolved = realpath(path, NULL);
  if (resolved) {
    struct stat st;
    if (stat(resolved, &st) == 0 && S_ISREG(st.st_mode)) return resolved;
    free(resolved);
  }
  return NULL;
}

static bool esm_has_extension(const char *spec) {
  const char *dot = strrchr(spec, '.');
  const char *slash = strrchr(spec, '/');
  return dot && (!slash || dot > slash);
}

static char *esm_resolve_path(const char *specifier, const char *base_path) {
  if (!(specifier[0] == '/' || 
       (specifier[0] == '.' && specifier[1] == '/') || 
       (specifier[0] == '.' && specifier[1] == '.' && specifier[2] == '/'))) {
    return strdup(specifier);
  }
  
  char *base_copy = strdup(base_path);
  char *dir = dirname(base_copy);
  char *result = NULL;
  
  const char *spec = (specifier[0] == '.' && specifier[1] == '/') ? specifier + 2 : specifier;
  bool has_ext = esm_has_extension(spec);
  
  if ((result = esm_try_resolve(dir, spec, ""))) goto cleanup;
  if (has_ext) goto cleanup;
  
  char *base_ext = esm_get_extension(base_path);
  
  if ((result = esm_try_resolve(dir, spec, base_ext))) goto cleanup_ext;
  if (strcmp(base_ext, ".js") != 0 && (result = esm_try_resolve(dir, spec, ".js"))) goto cleanup_ext;
  
  if ((result = esm_try_resolve(dir, spec, ".ts"))) goto cleanup_ext;
  if ((result = esm_try_resolve(dir, spec, ".mts"))) goto cleanup_ext;
  if ((result = esm_try_resolve(dir, spec, ".cts"))) goto cleanup_ext;
  if ((result = esm_try_resolve(dir, spec, ".json"))) goto cleanup_ext;
  
  char idx[PATH_MAX];
  snprintf(idx, PATH_MAX, "%s/index%s", spec, base_ext);
  if ((result = esm_try_resolve(dir, idx, ""))) goto cleanup_ext;
  
  if (strcmp(base_ext, ".js") != 0) {
    snprintf(idx, PATH_MAX, "%s/index.js", spec);
    if ((result = esm_try_resolve(dir, idx, ""))) goto cleanup_ext;
  }
  
  snprintf(idx, PATH_MAX, "%s/index.ts", spec);
  if ((result = esm_try_resolve(dir, idx, ""))) goto cleanup_ext;
  
cleanup_ext:
  free(base_ext);
cleanup:
  free(base_copy);
  return result;
}

static bool esm_has_suffix(const char *path, const char *ext) {
  size_t len = strlen(path);
  size_t elen = strlen(ext);
  return len > elen && strcmp(path + len - elen, ext) == 0;
}

static bool esm_is_json(const char *path) {
  return esm_has_suffix(path, ".json");
}

static bool esm_is_text(const char *path) {
  return esm_has_suffix(path, ".txt")  ||
         esm_has_suffix(path, ".md")   ||
         esm_has_suffix(path, ".html") ||
         esm_has_suffix(path, ".css");
}

static bool esm_is_image(const char *path) {
  return esm_has_suffix(path, ".png")  ||
         esm_has_suffix(path, ".jpg")  ||
         esm_has_suffix(path, ".jpeg") ||
         esm_has_suffix(path, ".gif")  ||
         esm_has_suffix(path, ".svg")  ||
         esm_has_suffix(path, ".webp");
}

static char *esm_canonicalize_path(const char *path) {
  if (!path) return NULL;
  
  char *canonical = strdup(path);
  if (!canonical) return NULL;
  
  char *src = canonical, *dst = canonical;
  
  while (*src) {
    if (*src == '/') {
      *dst++ = '/';
      while (*src == '/') src++;
      
      if (strncmp(src, "./", 2) == 0) {
        src += 2;
      } else if (strncmp(src, "../", 3) == 0) {
        src += 3;
        if (dst > canonical + 1) {
          dst--;
          while (dst > canonical && *(dst - 1) != '/') dst--;
        }
      }
    } else {
      *dst++ = *src++;
    }
  }
  
  *dst = '\0';
  
  if (strlen(canonical) > 1 && canonical[strlen(canonical) - 1] == '/') {
    canonical[strlen(canonical) - 1] = '\0';
  }
  
  return canonical;
}

static esm_module_t *esm_find_module(const char *resolved_path) {
  char *canonical_path = esm_canonicalize_path(resolved_path);
  if (!canonical_path) return NULL;
  
  esm_module_t *mod = NULL;
  HASH_FIND_STR(global_module_cache.modules, canonical_path, mod);
  
  free(canonical_path);
  return mod;
}

static esm_module_t *esm_create_module(const char *path, const char *resolved_path) {
  bool is_url = esm_is_url(resolved_path);
  char *canonical_path = is_url ? strdup(resolved_path) : esm_canonicalize_path(resolved_path);
  if (!canonical_path) return NULL;
  
  esm_module_t *existing_mod = NULL;
  HASH_FIND_STR(global_module_cache.modules, canonical_path, existing_mod);
  if (existing_mod) {
    free(canonical_path);
    return existing_mod;
  }
  
  esm_module_t *mod = (esm_module_t *)malloc(sizeof(esm_module_t));
  if (!mod) {
    free(canonical_path);
    return NULL;
  }
  
  mod->path = strdup(path);
  mod->resolved_path = canonical_path;
  mod->namespace_obj = js_mkundef();
  mod->default_export = js_mkundef();
  mod->is_loaded = false;
  mod->is_loading = false;
  mod->is_json = esm_is_json(resolved_path);
  mod->is_text = esm_is_text(resolved_path);
  mod->is_image = esm_is_image(resolved_path);
  mod->is_url = is_url;
  mod->url_content = NULL;
  mod->url_content_len = 0;
  mod->next = NULL;
  
  HASH_ADD_STR(global_module_cache.modules, resolved_path, mod);
  global_module_cache.count++;
  
  return mod;
}

static void esm_cleanup_module_cache(void) {
  esm_module_t *current, *tmp;
  HASH_ITER(hh, global_module_cache.modules, current, tmp) {
    HASH_DEL(global_module_cache.modules, current);
    if (current->path) free(current->path);
    if (current->resolved_path) free(current->resolved_path);
    if (current->url_content) free(current->url_content);
    free(current);
  }
  global_module_cache.count = 0;
}

typedef struct {
  char *data;
  size_t size;
} esm_file_data_t;

static jsval_t esm_read_file(struct js *js, const char *path, const char *kind, esm_file_data_t *out) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return js_mkerr(js, "Cannot open %s: %s", kind, path);
  
  fseek(fp, 0, SEEK_END);
  long fsize = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  
  char *buf = (char *)malloc((size_t)fsize + 1);
  if (!buf) {
    fclose(fp);
    return js_mkerr(js, "OOM loading %s", kind);
  }
  
  fread(buf, 1, (size_t)fsize, fp);
  fclose(fp);
  buf[fsize] = '\0';
  
  out->data = buf;
  out->size = (size_t)fsize;
  return js_mkundef();
}

static jsval_t esm_load_json(struct js *js, const char *path) {
  esm_file_data_t file;
  jsval_t err = esm_read_file(js, path, "JSON file", &file);
  if (is_err(err)) return err;
  
  jsval_t json_str = js_mkstr(js, file.data, file.size);
  free(file.data);
  return js_json_parse(js, &json_str, 1);
}

static jsval_t esm_load_text(struct js *js, const char *path) {
  esm_file_data_t file;
  jsval_t err = esm_read_file(js, path, "text file", &file);
  if (is_err(err)) return err;
  
  jsval_t result = js_mkstr(js, file.data, file.size);
  free(file.data);
  return result;
}

static jsval_t esm_load_image(struct js *js, const char *path) {
  esm_file_data_t file;
  jsval_t err = esm_read_file(js, path, "image file", &file);
  if (is_err(err)) return err;
  
  unsigned char *content = (unsigned char *)file.data;
  size_t size = file.size;
  
  jsval_t obj = mkobj(js, 0);
  jsval_t data_arr = mkarr(js);
  
  for (size_t i = 0; i < size; i++) {
    char idx[16];
    snprintf(idx, sizeof(idx), "%zu", i);
    setprop(js, data_arr, js_mkstr(js, idx, strlen(idx)), tov((double)content[i]));
  }
  setprop(js, data_arr, js_mkstr(js, "length", 6), tov((double)size));
  
  setprop(js, obj, js_mkstr(js, "data", 4), mkval(T_ARR, vdata(data_arr)));
  setprop(js, obj, js_mkstr(js, "path", 4), js_mkstr(js, path, strlen(path)));
  setprop(js, obj, js_mkstr(js, "size", 4), tov((double)size));
  
  free(file.data);
  return obj;
}

static jsval_t esm_load_module(struct js *js, esm_module_t *mod) {
  if (mod->is_loaded) return mod->namespace_obj;
  if (mod->is_loading) return js_mkerr(js, "Circular dependency detected: %s", mod->path);
  
  mod->is_loading = true;
  
  if (mod->is_json) {
    jsval_t json_val = esm_load_json(js, mod->resolved_path);
    if (is_err(json_val)) {
      mod->is_loading = false;
      return json_val;
    }
    
    mod->namespace_obj = json_val;
    mod->default_export = json_val;
    mod->is_loaded = true;
    mod->is_loading = false;
    return json_val;
  }
  
  if (mod->is_text) {
    jsval_t text_val = esm_load_text(js, mod->resolved_path);
    if (is_err(text_val)) {
      mod->is_loading = false;
      return text_val;
    }
    
    mod->namespace_obj = text_val;
    mod->default_export = text_val;
    mod->is_loaded = true;
    mod->is_loading = false;
    return text_val;
  }
  
  if (mod->is_image) {
    jsval_t img_val = esm_load_image(js, mod->resolved_path);
    if (is_err(img_val)) {
      mod->is_loading = false;
      return img_val;
    }
    
    mod->namespace_obj = img_val;
    mod->default_export = img_val;
    mod->is_loaded = true;
    mod->is_loading = false;
    return img_val;
  }
  
  char *content = NULL;
  size_t size = 0;
  
  if (mod->is_url) {
    if (mod->url_content) {
      content = strdup(mod->url_content);
      size = mod->url_content_len;
    } else {
      char *error = NULL;
      content = esm_fetch_url(mod->resolved_path, &size, &error);
      if (!content) {
        mod->is_loading = false;
        jsval_t err = js_mkerr(js, "Cannot fetch module %s: %s", mod->resolved_path, error ? error : "unknown error");
        if (error) free(error);
        return err;
      }
      mod->url_content = strdup(content);
      mod->url_content_len = size;
    }
  } else {
    esm_file_data_t file;
    jsval_t err = esm_read_file(js, mod->resolved_path, "module", &file);
    if (is_err(err)) {
      mod->is_loading = false;
      return err;
    }
    content = file.data;
    size = file.size;
  }
  content[size] = '\0';
  
  char *js_code = content;
  size_t js_len = size;
  
  if (is_typescript_file(mod->resolved_path)) {
    int result = OXC_strip_types(content, mod->resolved_path, content, size + 1);
    if (result < 0) {
      free(content);
      mod->is_loading = false;
      return js_mkerr(js, "TypeScript error: strip failed (%d)", result);
    }
    js_len = (size_t)result;
  }
  
  jsval_t ns = mkobj(js, 0);
  mod->namespace_obj = ns;
  
  jsval_t prev_module = js->module_ns;
  js->module_ns = ns;
  
  const char *prev_filename = js->filename;
  jsval_t saved_scope = js->scope;
  
  js_set_filename(js, mod->resolved_path);
  mkscope(js); set_slot(js, js->scope, SLOT_MODULE_SCOPE, tov(1));
  
  jsval_t result = js_eval(js, js_code, js_len);
  free(content);
  
  js->scope = saved_scope;
  js_set_filename(js, prev_filename);
  js->module_ns = prev_module;
  
  if (is_err(result)) {
    mod->is_loading = false;
    return result;
  }
  
  jsval_t default_val = js_get_slot(js, ns, SLOT_DEFAULT);
  mod->default_export = vtype(default_val) != T_UNDEF ? default_val : ns;
  
  mod->is_loaded = true;
  mod->is_loading = false;
  
  return ns;
}

static jsval_t esm_get_or_load(struct js *js, const char *specifier, const char *resolved_path) {
  esm_module_t *mod = esm_find_module(resolved_path);
  if (!mod) {
    mod = esm_create_module(specifier, resolved_path);
    if (!mod) return js_mkerr(js, "Cannot create module");
  }
  return esm_load_module(js, mod);
}

typedef struct {
  const char *import_name;
  size_t import_len;
  const char *local_name;
  size_t local_len;
} esm_import_binding_t;

static char *esm_jsval_to_cstr(struct js *js, jsval_t str, jsoff_t *out_len) {
  jsoff_t len;
  jsoff_t off = vstr(js, str, &len);
  if (out_len) *out_len = len;
  return strndup((char *)&js->mem[off], len);
}

static jsval_t esm_resolve_and_load(struct js *js, const char *spec_str, jsoff_t spec_len) {
  ant_library_t *lib = find_library(spec_str, spec_len);
  if (lib) return lib->init_fn(js);
  
  const char *base_path = js->filename ? js->filename : ".";
  char *resolved_path = esm_resolve(spec_str, base_path, esm_resolve_path);
  if (!resolved_path) return js_mkerr(js, "Cannot resolve module: %s", spec_str);
  
  jsval_t ns = esm_get_or_load(js, spec_str, resolved_path);
  free(resolved_path);
  return ns;
}

static jsval_t esm_make_file_url(struct js *js, const char *path) {
  size_t url_len = strlen(path) + 8;
  char *url = malloc(url_len);
  if (!url) return js_mkerr(js, "oom");
  
  snprintf(url, url_len, "file://%s", path);
  jsval_t val = js_mkstr(js, url, strlen(url));
  free(url);
  return val;
}

static int esm_parse_named_imports(struct js *js, esm_import_binding_t *bindings, int max_bindings) {
  int count = 0;
  
  while (next(js) != TOK_RBRACE && count < max_bindings) {
    if (next(js) != TOK_IDENTIFIER && next(js) != TOK_DEFAULT) {
      return -1;
    }
    const char *import_name = &js->code[js->toff];
    size_t import_len = js->tlen;
    js->consumed = 1;
    
    const char *local_name = import_name;
    size_t local_len = import_len;
    
    if (next(js) == TOK_AS) {
      js->consumed = 1;
      if (next(js) != TOK_IDENTIFIER && next(js) != TOK_DEFAULT) {
        return -1;
      }
      local_name = &js->code[js->toff];
      local_len = js->tlen;
      js->consumed = 1;
    }
    
    bindings[count].import_name = import_name;
    bindings[count].import_len = import_len;
    bindings[count].local_name = local_name;
    bindings[count].local_len = local_len;
    count++;
    
    if (next(js) == TOK_COMMA) js->consumed = 1;
  }
  
  if (next(js) != TOK_RBRACE) return -1;
  js->consumed = 1;
  return count;
}

static jsval_t builtin_import(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_STR) {
    return js_mkerr(js, "import() requires a string specifier");
  }
  
  jsoff_t spec_len;
  char *specifier = esm_jsval_to_cstr(js, args[0], &spec_len);
  jsval_t ns = esm_resolve_and_load(js, specifier, spec_len);
  free(specifier);
  
  if (is_err(ns)) return builtin_Promise_reject(js, &ns, 1);
  
  jsval_t promise_args[] = { ns };
  return builtin_Promise_resolve(js, promise_args, 1);
}

static jsval_t builtin_import_meta_resolve(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_STR) {
    return js_mkerr(js, "import.meta.resolve() requires a string specifier");
  }
  
  char *specifier = esm_jsval_to_cstr(js, args[0], NULL);
  
  const char *base_path = js->filename ? js->filename : ".";
  char *resolved_path = esm_resolve(specifier, base_path, esm_resolve_path);
  if (!resolved_path) {
    jsval_t err = js_mkerr(js, "Cannot resolve module: %s", specifier);
    free(specifier); return err;
  }
  free(specifier);
  
  if (esm_is_url(resolved_path)) {
    jsval_t result = js_mkstr(js, resolved_path, strlen(resolved_path));
    free(resolved_path); return result;
  }
  
  jsval_t result = esm_make_file_url(js, resolved_path);
  free(resolved_path);
  return result;
}

void js_setup_import_meta(struct js *js, const char *filename) {
  if (!filename) return;
  
  jsval_t import_meta = mkobj(js, 0);
  if (is_err(import_meta)) return;
  bool is_url = esm_is_url(filename);
  
  jsval_t url_val = is_url ? js_mkstr(js, filename, strlen(filename)) : esm_make_file_url(js, filename);
  if (!is_err(url_val)) setprop(js, import_meta, js_mkstr(js, "url", 3), url_val);
  
  jsval_t filename_val = js_mkstr(js, filename, strlen(filename));
  if (!is_err(filename_val)) setprop(js, import_meta, js_mkstr(js, "filename", 8), filename_val);
  
  if (is_url) {
    char *filename_copy = strdup(filename);
    if (filename_copy) {
      char *last_slash = strrchr(filename_copy, '/');
      char *scheme_end = strstr(filename_copy, "://");
      if (last_slash && scheme_end && last_slash > scheme_end + 2) {
        *last_slash = '\0';
        jsval_t dirname_val = js_mkstr(js, filename_copy, strlen(filename_copy));
        if (!is_err(dirname_val)) setprop(js, import_meta, js_mkstr(js, "dirname", 7), dirname_val);
      }
      free(filename_copy);
    }
  } else {
    char *filename_copy = strdup(filename);
    if (filename_copy) {
      char *dir = dirname(filename_copy);
      if (dir) {
        jsval_t dirname_val = js_mkstr(js, dir, strlen(dir));
        if (!is_err(dirname_val)) setprop(js, import_meta, js_mkstr(js, "dirname", 7), dirname_val);
      }
      free(filename_copy);
    }
  }
  
  setprop(js, import_meta, js_mkstr(js, "main", 4), js_true);
  jsval_t resolve_fn = js_mkfun(builtin_import_meta_resolve);
  setprop(js, import_meta, js_mkstr(js, "resolve", 7), resolve_fn);
  
  jsval_t glob = js_glob(js);
  jsoff_t import_off = lkp(js, glob, "import", 6);
  
  if (import_off != 0) {
    jsval_t import_fn = resolveprop(js, mkval(T_PROP, import_off));
    if (vtype(import_fn) == T_FUNC) {
      jsval_t import_obj = mkval(T_OBJ, vdata(import_fn));
      setprop(js, import_obj, js_mkstr(js, "meta", 4), import_meta);
    }
  }
}

static jsval_t js_import_stmt(struct js *js) {
  js->consumed = 1;
  
  if (next(js) == TOK_LPAREN) {
    js->consumed = 1;
    jsval_t spec = js_expr(js);
    EXPECT(TOK_RPAREN);
    
    if (vtype(spec) != T_STR) {
      return js_mkerr(js, "import() requires string");
    }
    
    jsval_t args[] = { spec };
    return builtin_import(js, args, 1);
  }
  
  if (next(js) == TOK_MUL) {
    js->consumed = 1;
    EXPECT(TOK_AS);
    EXPECT(TOK_IDENTIFIER);
    
    const char *namespace_name = &js->code[js->toff];
    size_t namespace_len = js->tlen;
    js->consumed = 1;
    
    EXPECT(TOK_FROM);
    EXPECT(TOK_STRING);
    
    jsval_t spec = js_str_literal(js);
    jsoff_t spec_len;
    char *spec_str = esm_jsval_to_cstr(js, spec, &spec_len);
    
    js_parse_state_t saved;
    JS_SAVE_STATE(js, saved);
    jsval_t ns = esm_resolve_and_load(js, spec_str, spec_len);
    JS_RESTORE_STATE(js, saved);
    free(spec_str);
    
    js->consumed = 1; next(js); js->consumed = 0;
    if (is_err(ns)) return ns;
    
    if (vtype(ns) != T_OBJ) {
      return js_mkerr_typed(js, JS_ERR_SYNTAX, "Cannot re-export from non-object module");
    }
    
    setprop(js, js->scope, js_mkstr(js, namespace_name, namespace_len), ns);
    return js_mkundef();
  }
  
  if (next(js) == TOK_IDENTIFIER) {
    const char *default_name = &js->code[js->toff];
    size_t default_len = js->tlen;
    js->consumed = 1;
    
    esm_import_binding_t bindings[64];
    int binding_count = 0;
    
    if (next(js) == TOK_COMMA) {
      js->consumed = 1;
      
      if (next(js) == TOK_LBRACE) {
        js->consumed = 1;
        binding_count = esm_parse_named_imports(js, bindings, 64);
        if (binding_count < 0) return js_mkerr(js, "Failed to parse named imports");
      } else if (next(js) == TOK_MUL) {
        js->consumed = 1;
        EXPECT(TOK_AS);
        EXPECT(TOK_IDENTIFIER);
        bindings[binding_count].import_name = NULL;
        bindings[binding_count].import_len = 0;
        bindings[binding_count].local_name = &js->code[js->toff];
        bindings[binding_count].local_len = js->tlen;
        binding_count++;
        js->consumed = 1;
      } else {
        return js_mkerr(js, "Expected '{' or '*' after ',' in import");
      }
    }
    
    EXPECT(TOK_FROM);
    EXPECT(TOK_STRING);
    
    jsval_t spec = js_str_literal(js);
    jsoff_t spec_len;
    char *spec_str = esm_jsval_to_cstr(js, spec, &spec_len);
    
    js_parse_state_t saved;
    JS_SAVE_STATE(js, saved);
    jsval_t ns = esm_resolve_and_load(js, spec_str, spec_len);
    JS_RESTORE_STATE(js, saved);
    free(spec_str);
    
    js->consumed = 1; next(js); js->consumed = 0;
    if (is_err(ns)) return ns;
    
    jsval_t default_val;
    if (vtype(ns) == T_OBJ) {
      jsval_t slot_val = js_get_slot(js, ns, SLOT_DEFAULT);
      default_val = vtype(slot_val) != T_UNDEF ? slot_val : ns;
    } else default_val = ns;
    setprop(js, js->scope, js_mkstr(js, default_name, default_len), default_val);
    
    for (int i = 0; i < binding_count; i++) {
      if (bindings[i].import_name == NULL) {
        setprop(js, js->scope, js_mkstr(js, bindings[i].local_name, bindings[i].local_len), ns);
      } else if (vtype(ns) == T_OBJ) {
        jsoff_t prop_off = lkp(js, ns, bindings[i].import_name, bindings[i].import_len);
        if (prop_off == 0) return js_mkerr_typed(
          js, JS_ERR_SYNTAX, "The requested module does not provide an export named '%.*s'",
          (int)bindings[i].import_len, bindings[i].import_name
        );
        jsval_t imported_val = resolveprop(js, mkval(T_PROP, prop_off));
        setprop(js, js->scope, js_mkstr(js, bindings[i].local_name, bindings[i].local_len), imported_val);
      } else return js_mkerr_typed(js, JS_ERR_SYNTAX, "Cannot use named imports from non-object module");
    }
    
    return js_mkundef();
  }
  
  if (next(js) == TOK_LBRACE) {
    js->consumed = 1;
    
    esm_import_binding_t bindings[64];
    int binding_count = esm_parse_named_imports(js, bindings, 64);
    if (binding_count < 0) return js_mkerr(js, "Failed to parse named imports");
    
    EXPECT(TOK_FROM);
    EXPECT(TOK_STRING);
    
    jsval_t spec = js_str_literal(js);
    jsoff_t spec_len;
    char *spec_str = esm_jsval_to_cstr(js, spec, &spec_len);
    
    js_parse_state_t saved;
    JS_SAVE_STATE(js, saved);
    jsoff_t saved_toff = js->toff, saved_tlen = js->tlen;
    jsval_t saved_scope = js->scope;
    
    jsval_t ns = esm_resolve_and_load(js, spec_str, spec_len);
    
    JS_RESTORE_STATE(js, saved);
    js->toff = saved_toff;
    js->tlen = saved_tlen;
    js->scope = saved_scope;
    free(spec_str);
    
    js->consumed = 1;
    next(js);
    js->consumed = 0;
    
    if (is_err(ns)) return ns;
    if (vtype(ns) != T_OBJ) return js_mkerr(js, "Cannot use named imports from non-object module");
    
    for (int i = 0; i < binding_count; i++) {
      jsoff_t prop_off = lkp(js, ns, bindings[i].import_name, bindings[i].import_len);
      if (prop_off == 0) return js_mkerr_typed(
        js, JS_ERR_SYNTAX, "The requested module does not provide an export named '%.*s'", 
        (int)bindings[i].import_len, bindings[i].import_name
      );
      jsval_t imported_val = resolveprop(js, mkval(T_PROP, prop_off));
      setprop(js, js->scope, js_mkstr(js, bindings[i].local_name, bindings[i].local_len), imported_val);
    }
    
    return js_mkundef();
  }

  if (next(js) == TOK_STRING) {
    jsval_t spec = js_str_literal(js);
    jsoff_t spec_len;
    char *spec_str = esm_jsval_to_cstr(js, spec, &spec_len);

    js_parse_state_t saved;
    JS_SAVE_STATE(js, saved);
    jsval_t ns = esm_resolve_and_load(js, spec_str, spec_len);
    JS_RESTORE_STATE(js, saved);
    free(spec_str);

    js->consumed = 1;
    next(js);
    js->consumed = 0;

    if (is_err(ns)) return ns;
    return js_mkundef();
  }

  return js_mkerr_typed(js, JS_ERR_SYNTAX, "Invalid import statement");
}

static jsval_t js_export_stmt(struct js *js) {
  js->consumed = 1;
  
  if (vtype(js->module_ns) != T_OBJ) {
    js->module_ns = mkobj(js, 0);
  }
  
  if (next(js) == TOK_DEFAULT) {
    js->consumed = 1;
    jsval_t value = js_assignment(js);
    if (is_err(value)) return value;
    
    jsval_t resolved = resolveprop(js, value);
    js_set_slot(js, js->module_ns, SLOT_DEFAULT, resolved);
    js_mkprop_fast(js, js->module_ns, "default", 7, resolved);
    
    return value;
  }
  
  if (next(js) == TOK_CONST || next(js) == TOK_LET || next(js) == TOK_VAR) {
    bool is_const = (next(js) == TOK_CONST);
    js->consumed = 1;
    
    EXPECT(TOK_IDENTIFIER);
    const char *name = &js->code[js->toff];
    size_t name_len = js->tlen;
    js->consumed = 1;
    
    jsval_t value = js_mkundef();
    if (next(js) == TOK_ASSIGN) {
      js->consumed = 1;
      value = js_assignment(js);
      if (is_err(value)) return value;
    }
    
    jsval_t key = js_mkstr(js, name, name_len);
    mkprop(js, js->scope, key, resolveprop(js, value), is_const ? CONSTMASK : 0);
    setprop(js, js->module_ns, key, resolveprop(js, value));
    
    return value;
  }
  
  if (next(js) == TOK_FUNC) {
    jsval_t func = js_func_literal(js, false);
    if (is_err(func)) return func;
    
    jsval_t func_obj = mkval(T_OBJ, vdata(func));
    jsoff_t name_off = lkp(js, func_obj, "name", 4);
    if (name_off != 0) {
      jsval_t name_val = resolveprop(js, mkval(T_PROP, name_off));
      if (vtype(name_val) == T_STR) {
        setprop(js, js->scope, name_val, func);
        setprop(js, js->module_ns, name_val, func);
      }
    }
    
    return func;
  }
  
  if (next(js) == TOK_CLASS) {
    jsval_t cls = js_class_decl(js);
    if (is_err(cls)) return cls;
    
    jsval_t cls_obj = mkval(T_OBJ, vdata(cls));
    jsoff_t name_off = lkp(js, cls_obj, "name", 4);
    if (name_off != 0) {
      jsval_t name_val = resolveprop(js, mkval(T_PROP, name_off));
       if (vtype(name_val) == T_STR) {
         setprop(js, js->scope, name_val, cls);
         setprop(js, js->module_ns, name_val, cls);
       }
    }
    
    return cls;
  }
  
  if (next(js) == TOK_MUL) {
    js->consumed = 1;
    
    const char *alias_name = NULL;
    size_t alias_len = 0;
    
    if (next(js) == TOK_AS) {
      js->consumed = 1;
      EXPECT(TOK_IDENTIFIER);
      alias_name = &js->code[js->toff];
      alias_len = js->tlen;
      js->consumed = 1;
    }
    
    EXPECT(TOK_FROM);
    EXPECT(TOK_STRING);
    
    jsval_t spec = js_str_literal(js);
    jsoff_t spec_len;
    char *spec_str = esm_jsval_to_cstr(js, spec, &spec_len);
    
    js_parse_state_t saved;
    JS_SAVE_STATE(js, saved);
    jsval_t ns = esm_resolve_and_load(js, spec_str, spec_len);
    JS_RESTORE_STATE(js, saved);
    free(spec_str);
    
    js->consumed = 1; next(js); js->consumed = 0;
    if (is_err(ns)) return ns;
    
    if (alias_name) {
      setprop(js, js->module_ns, js_mkstr(js, alias_name, alias_len), ns);
    } else if (vtype(ns) == T_OBJ) {
      ant_iter_t iter = js_prop_iter_begin(js, ns);
      const char *key; size_t key_len; jsval_t value;
      
      while (js_prop_iter_next(&iter, &key, &key_len, &value)) {
        setprop(js, js->module_ns, js_mkstr(js, key, key_len), resolveprop(js, value));
      }
      js_prop_iter_end(&iter);
    }
    
    return js_mkundef();
  }
  
  if (next(js) == TOK_LBRACE) {
    js->consumed = 1;
    
    typedef struct { const char *local; size_t local_len; const char *exported; size_t export_len; } export_spec_t;
    export_spec_t specs[64];
    int spec_count = 0;
    
    while (next(js) != TOK_RBRACE) {
      if (spec_count >= 64) return js_mkerr(js, "too many export specifiers");
      
      if (next(js) != TOK_IDENTIFIER && next(js) != TOK_DEFAULT) {
        return js_mkerr_typed(js, JS_ERR_SYNTAX, "expected identifier or 'default' in export list");
      }
      specs[spec_count].local = &js->code[js->toff];
      specs[spec_count].local_len = js->tlen;
      specs[spec_count].exported = specs[spec_count].local;
      specs[spec_count].export_len = specs[spec_count].local_len;
      js->consumed = 1;
      
      if (next(js) == TOK_AS) {
        js->consumed = 1;
        if (next(js) != TOK_IDENTIFIER && next(js) != TOK_DEFAULT) {
          return js_mkerr_typed(js, JS_ERR_SYNTAX, "expected identifier or 'default' after 'as'");
        }
        specs[spec_count].exported = &js->code[js->toff];
        specs[spec_count].export_len = js->tlen;
        js->consumed = 1;
      }
      
      spec_count++;
      if (next(js) == TOK_COMMA) js->consumed = 1;
    }
    
    EXPECT(TOK_RBRACE);
    
    if (next(js) == TOK_FROM) {
      js->consumed = 1;
      EXPECT(TOK_STRING);
      jsval_t spec = js_str_literal(js);
      jsoff_t spec_len;
      char *spec_str = esm_jsval_to_cstr(js, spec, &spec_len);
      
      js_parse_state_t saved;
      JS_SAVE_STATE(js, saved);
      jsval_t ns = esm_resolve_and_load(js, spec_str, spec_len);
      JS_RESTORE_STATE(js, saved);
      free(spec_str);
      
      js->consumed = 1; next(js); js->consumed = 0;
      if (is_err(ns)) return ns;
      
      for (int i = 0; i < spec_count; i++) {
        jsoff_t prop_off = lkp(js, ns, specs[i].local, specs[i].local_len);
        jsval_t import_val = prop_off != 0 ? resolveprop(js, mkval(T_PROP, prop_off)) : js_mkundef();
        esm_export_binding(js, specs[i].exported, specs[i].export_len, import_val);
      }
    } else {
      for (int i = 0; i < spec_count; i++) {
        jsval_t local_val = lookup(js, specs[i].local, specs[i].local_len);
        if (is_err(local_val)) return local_val;
        jsval_t export_val = resolveprop(js, local_val);
        esm_export_binding(js, specs[i].exported, specs[i].export_len, export_val);
      }
    }
    
    if (next(js) == TOK_SEMICOLON) js->consumed = 1;
    return js_mkundef();
  }
  
  return js_mkerr(js, "Invalid export statement");
}

typedef struct weakmap_entry {
    jsval_t key_obj;
    jsval_t value;
    UT_hash_handle hh;
} weakmap_entry_t;

typedef struct weakset_entry {
    jsval_t value_obj;
    UT_hash_handle hh;
} weakset_entry_t;

static const char* jsval_to_key(struct js *js, jsval_t val) {
  if (vtype(val) == T_STR) {
    jsoff_t len;
    jsoff_t off = vstr(js, val, &len);
    return (char *)&js->mem[off];
  } else return js_str(js, val);
}

static jsval_t builtin_Map(struct js *js, jsval_t *args, int nargs) {
  jsval_t map_obj = mkobj(js, 0);
  
  jsval_t map_proto = get_ctor_proto(js, "Map", 3);
  if (vtype(map_proto) == T_OBJ) set_proto(js, map_obj, map_proto);
  
  map_entry_t **map_head = (map_entry_t **)ant_calloc(sizeof(map_entry_t *));
  if (!map_head) return js_mkerr(js, "out of memory");
  *map_head = NULL;
  
  map_registry_entry_t *reg = (map_registry_entry_t *)ant_calloc(sizeof(map_registry_entry_t));
  if (reg) {
    reg->head = map_head;
    HASH_ADD_PTR(map_registry, head, reg);
  }
  
  jsval_t map_ptr = tov((double)(size_t)map_head);
  set_slot(js, map_obj, SLOT_MAP, map_ptr);
  
  if (nargs == 0 || vtype(args[0]) != T_ARR) return map_obj;
  
  jsval_t iterable = args[0];
  jsoff_t length = js_arr_len(js, iterable);
  
  for (jsoff_t i = 0; i < length; i++) {
    jsval_t entry = js_arr_get(js, iterable, i);
    if (vtype(entry) != T_ARR) continue;
    
    jsoff_t entry_len = js_arr_len(js, entry);
    if (entry_len < 2) continue;
    
    jsval_t key = js_arr_get(js, entry, 0);
    jsval_t value = js_arr_get(js, entry, 1);
    const char *key_str = jsval_to_key(js, key);
    
    map_entry_t *map_entry;
    HASH_FIND_STR(*map_head, key_str, map_entry);
    if (map_entry) {
      map_entry->value = value;
      continue;
    }
    
    map_entry = (map_entry_t *)ant_calloc(sizeof(map_entry_t));
    if (!map_entry) return js_mkerr(js, "out of memory");
    map_entry->key = strdup(key_str);
    map_entry->value = value;
    HASH_ADD_STR(*map_head, key, map_entry);
  }
  
  return map_obj;
}

static jsval_t builtin_Set(struct js *js, jsval_t *args, int nargs) {
  jsval_t set_obj = mkobj(js, 0);
  
  jsval_t set_proto_val = get_ctor_proto(js, "Set", 3);
  if (vtype(set_proto_val) == T_OBJ) set_proto(js, set_obj, set_proto_val);
  
  set_entry_t **set_head = (set_entry_t **)ant_calloc(sizeof(set_entry_t *));
  if (!set_head) return js_mkerr(js, "out of memory");
  *set_head = NULL;
  
  set_registry_entry_t *reg = (set_registry_entry_t *)ant_calloc(sizeof(set_registry_entry_t));
  if (reg) {
    reg->head = set_head;
    HASH_ADD_PTR(set_registry, head, reg);
  }
  
  jsval_t set_ptr = tov((double)(size_t)set_head);
  set_slot(js, set_obj, SLOT_SET, set_ptr);
  
  if (nargs == 0 || vtype(args[0]) != T_ARR) return set_obj;
  
  jsval_t iterable = args[0];
  jsoff_t length = js_arr_len(js, iterable);
  
  for (jsoff_t i = 0; i < length; i++) {
    jsval_t value = js_arr_get(js, iterable, i);
    const char *key_str = jsval_to_key(js, value);
    
    set_entry_t *entry;
    HASH_FIND_STR(*set_head, key_str, entry);
    if (entry) continue;
    
    entry = (set_entry_t *)ant_calloc(sizeof(set_entry_t));
    if (!entry) return js_mkerr(js, "out of memory");
    entry->value = value;
    entry->key = strdup(key_str);
    HASH_ADD_KEYPTR(hh, *set_head, entry->key, strlen(entry->key), entry);
  }
  
  return set_obj;
}

static map_entry_t** get_map_from_obj(struct js *js, jsval_t obj) {
  jsval_t map_val = js_get_slot(js, obj, SLOT_MAP);
  if (vtype(map_val) == T_UNDEF) return NULL;
  return (map_entry_t**)(size_t)tod(map_val);
}

static set_entry_t** get_set_from_obj(struct js *js, jsval_t obj) {
  jsval_t set_val = js_get_slot(js, obj, SLOT_SET);
  if (vtype(set_val) == T_UNDEF) return NULL;
  return (set_entry_t**)(size_t)tod(set_val);
}

static jsval_t map_set(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "Map.set() requires 2 arguments");
  
  jsval_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(js, this_val);
  if (!map_ptr) return js_mkerr(js, "Invalid Map object");
  
  const char *key_str;
  if (vtype(args[0]) == T_STR) {
    jsoff_t len;
    jsoff_t off = vstr(js, args[0], &len);
    key_str = (char *)&js->mem[off];
  } else key_str = js_str(js, args[0]);
  
  map_entry_t *entry;
  HASH_FIND_STR(*map_ptr, key_str, entry);
  if (entry) {
    entry->value = args[1];
  } else {
    entry = (map_entry_t *)ant_calloc(sizeof(map_entry_t));
    if (!entry) return js_mkerr(js, "out of memory");
    entry->key = strdup(key_str);
    entry->value = args[1];
    HASH_ADD_STR(*map_ptr, key, entry);
  }
  
  return this_val;
}

static jsval_t map_get(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Map.get() requires 1 argument");
  
  jsval_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(js, this_val);
  if (!map_ptr) return js_mkundef();
  
  const char *key_str;
  if (vtype(args[0]) == T_STR) {
    jsoff_t len;
    jsoff_t off = vstr(js, args[0], &len);
    key_str = (char *)&js->mem[off];
  } else key_str = js_str(js, args[0]);
  
  map_entry_t *entry;
  HASH_FIND_STR(*map_ptr, key_str, entry);
  return entry ? entry->value : js_mkundef();
}

static jsval_t map_has(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Map.has() requires 1 argument");
  
  jsval_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(js, this_val);
  if (!map_ptr) return mkval(T_BOOL, 0);
  
  const char *key_str;
  if (vtype(args[0]) == T_STR) {
    jsoff_t len;
    jsoff_t off = vstr(js, args[0], &len);
    key_str = (char *)&js->mem[off];
  } else key_str = js_str(js, args[0]);
  
  map_entry_t *entry;
  HASH_FIND_STR(*map_ptr, key_str, entry);
  return mkval(T_BOOL, entry ? 1 : 0);
}

static jsval_t map_delete(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Map.delete() requires 1 argument");
  
  jsval_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(js, this_val);
  if (!map_ptr) return mkval(T_BOOL, 0);
  
  const char *key_str;
  if (vtype(args[0]) == T_STR) {
      jsoff_t len;
      jsoff_t off = vstr(js, args[0], &len);
      key_str = (char *)&js->mem[off];
  } else key_str = js_str(js, args[0]);
  
  map_entry_t *entry;
  HASH_FIND_STR(*map_ptr, key_str, entry);
  if (entry) {
    HASH_DEL(*map_ptr, entry);
    free(entry->key);
    free(entry);
    return mkval(T_BOOL, 1);
  }
  return mkval(T_BOOL, 0);
}

static jsval_t map_clear(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(js, this_val);
  if (!map_ptr) return js_mkundef();
  
  map_entry_t *entry, *tmp;
  HASH_ITER(hh, *map_ptr, entry, tmp) {
    HASH_DEL(*map_ptr, entry);
    free(entry->key);
    free(entry);
  }
  *map_ptr = NULL;
  
  return js_mkundef();
}

static jsval_t map_size(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(js, this_val);
  if (!map_ptr) return tov(0);
  
  size_t count = HASH_COUNT(*map_ptr);
  return tov((double)count);
}

static jsval_t map_iter_next(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t this_val = js->this_val;
  jsval_t idx_val = js_get(js, this_val, "__idx");
  jsval_t keys = js_get(js, this_val, "__keys");
  jsval_t vals = js_get(js, this_val, "__vals");
  jsval_t len_val = js_get(js, this_val, "__len");
  
  int idx = (int)js_getnum(idx_val);
  int len = (int)js_getnum(len_val);
  
  jsval_t result = js_mkobj(js);
  if (idx >= len) {
    js_set(js, result, "done", js_true);
    js_set(js, result, "value", js_mkundef());
  } else {
    char idxstr[16];
    uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
    jsval_t k = js_get(js, keys, idxstr);
    jsval_t v = js_get(js, vals, idxstr);
    jsval_t entry = js_mkarr(js);
    js_arr_push(js, entry, k);
    js_arr_push(js, entry, v);
    js_set(js, result, "value", entry);
    js_set(js, result, "done", js_false);
    js_set(js, this_val, "__idx", js_mknum(idx + 1));
  }
  return result;
}

static jsval_t map_entries(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(js, this_val);
  
  jsval_t keys = js_mkarr(js);
  jsval_t vals = js_mkarr(js);
  int count = 0;
  
  if (map_ptr && *map_ptr) {
    map_entry_t *entry, *tmp;
    HASH_ITER(hh, *map_ptr, entry, tmp) {
      js_arr_push(js, keys, js_mkstr(js, entry->key, strlen(entry->key)));
      js_arr_push(js, vals, entry->value);
      count++;
    }
  }
  
  jsval_t iter = js_mkobj(js);
  js_set(js, iter, "__keys", keys);
  js_set(js, iter, "__vals", vals);
  js_set(js, iter, "__idx", js_mknum(0));
  js_set(js, iter, "__len", js_mknum(count));
  js_set(js, iter, "next", js_mkfun(map_iter_next));
  return iter;
}

static jsval_t set_iter_next(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t this_val = js->this_val;
  jsval_t idx_val = js_get(js, this_val, "__idx");
  jsval_t vals = js_get(js, this_val, "__vals");
  jsval_t len_val = js_get(js, this_val, "__len");
  
  int idx = (int)js_getnum(idx_val);
  int len = (int)js_getnum(len_val);
  
  jsval_t result = js_mkobj(js);
  if (idx >= len) {
    js_set(js, result, "done", js_true);
    js_set(js, result, "value", js_mkundef());
  } else {
    char idxstr[16];
    uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
    jsval_t v = js_get(js, vals, idxstr);
    js_set(js, result, "value", v);
    js_set(js, result, "done", js_false);
    js_set(js, this_val, "__idx", js_mknum(idx + 1));
  }
  return result;
}

static jsval_t set_values(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t this_val = js->this_val;
  set_entry_t **set_ptr = get_set_from_obj(js, this_val);
  
  jsval_t vals = js_mkarr(js);
  int count = 0;
  
  if (set_ptr && *set_ptr) {
    set_entry_t *entry, *tmp;
    HASH_ITER(hh, *set_ptr, entry, tmp) {
      js_arr_push(js, vals, entry->value);
      count++;
    }
  }
  
  jsval_t iter = js_mkobj(js);
  js_set(js, iter, "__vals", vals);
  js_set(js, iter, "__idx", js_mknum(0));
  js_set(js, iter, "__len", js_mknum(count));
  js_set(js, iter, "next", js_mkfun(set_iter_next));
  return iter;
}

static jsval_t set_add(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Set.add() requires 1 argument");
  
  jsval_t this_val = js->this_val;
  set_entry_t **set_ptr = get_set_from_obj(js, this_val);
  if (!set_ptr) return js_mkerr(js, "Invalid Set object");
  
  const char *key_str = jsval_to_key(js, args[0]);
  
  set_entry_t *entry;
  HASH_FIND_STR(*set_ptr, key_str, entry);
  
  if (!entry) {
    entry = (set_entry_t *)ant_calloc(sizeof(set_entry_t));
    if (!entry) return js_mkerr(js, "out of memory");
    entry->value = args[0];
    entry->key = strdup(key_str);
    HASH_ADD_KEYPTR(hh, *set_ptr, entry->key, strlen(entry->key), entry);
  }
  
  return this_val;
}

static jsval_t set_has(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Set.has() requires 1 argument");
  
  jsval_t this_val = js->this_val;
  set_entry_t **set_ptr = get_set_from_obj(js, this_val);
  if (!set_ptr) return mkval(T_BOOL, 0);
  
  const char *key_str = jsval_to_key(js, args[0]);
  
  set_entry_t *entry;
  HASH_FIND_STR(*set_ptr, key_str, entry);
  
  return mkval(T_BOOL, entry ? 1 : 0);
}

static jsval_t set_delete(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Set.delete() requires 1 argument");
  
  jsval_t this_val = js->this_val;
  set_entry_t **set_ptr = get_set_from_obj(js, this_val);
  if (!set_ptr) return mkval(T_BOOL, 0);
  
  const char *key_str = jsval_to_key(js, args[0]);
  set_entry_t *entry;
  HASH_FIND_STR(*set_ptr, key_str, entry);
  
  if (entry) {
    HASH_DEL(*set_ptr, entry);
    free(entry->key);
    free(entry);
    return mkval(T_BOOL, 1);
  }
  return mkval(T_BOOL, 0);
}

static jsval_t set_clear(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_val = js->this_val;
  set_entry_t **set_ptr = get_set_from_obj(js, this_val);
  if (!set_ptr) return js_mkundef();
  
  set_entry_t *entry, *tmp;
  HASH_ITER(hh, *set_ptr, entry, tmp) {
    HASH_DEL(*set_ptr, entry);
    free(entry->key);
    free(entry);
  }
  *set_ptr = NULL;
  
  return js_mkundef();
}

static jsval_t set_size(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_val = js->this_val;
  set_entry_t **set_ptr = get_set_from_obj(js, this_val);
  if (!set_ptr) return tov(0);
  
  size_t count = HASH_COUNT(*set_ptr);
  return tov((double)count);
}

static jsval_t builtin_WeakMap(struct js *js, jsval_t *args, int nargs) {
  jsval_t wm_obj = mkobj(js, 0);
  
  jsval_t wm_proto = get_ctor_proto(js, "WeakMap", 7);
  if (vtype(wm_proto) == T_OBJ) set_proto(js, wm_obj, wm_proto);
  
  weakmap_entry_t **wm_head = (weakmap_entry_t **)ant_calloc(sizeof(weakmap_entry_t *));
  if (!wm_head) return js_mkerr(js, "out of memory");
  *wm_head = NULL;
  
  jsval_t wm_ptr = mkval(T_NUM, (size_t)wm_head);
  jsval_t wm_key = js_mkstr(js, "__weakmap", 9);
  setprop(js, wm_obj, wm_key, wm_ptr);
  
  if (nargs == 0 || vtype(args[0]) != T_ARR) return wm_obj;
  
  jsval_t iterable = args[0];
  jsoff_t length = js_arr_len(js, iterable);
  
  for (jsoff_t i = 0; i < length; i++) {
    jsval_t entry = js_arr_get(js, iterable, i);
    if (vtype(entry) != T_ARR) continue;
    
    jsoff_t entry_len = js_arr_len(js, entry);
    if (entry_len < 2) continue;
    
    jsval_t key = js_arr_get(js, entry, 0);
    jsval_t value = js_arr_get(js, entry, 1);
    
    if (vtype(key) != T_OBJ) return js_mkerr(js, "WeakMap key must be an object");
    
    weakmap_entry_t *wm_entry;
    HASH_FIND(hh, *wm_head, &key, sizeof(jsval_t), wm_entry);
    if (wm_entry) {
      wm_entry->value = value;
      continue;
    }
    
    wm_entry = (weakmap_entry_t *)ant_calloc(sizeof(weakmap_entry_t));
    if (!wm_entry) return js_mkerr(js, "out of memory");
    wm_entry->key_obj = key;
    wm_entry->value = value;
    HASH_ADD(hh, *wm_head, key_obj, sizeof(jsval_t), wm_entry);
  }
  
  return wm_obj;
}

static jsval_t builtin_WeakSet(struct js *js, jsval_t *args, int nargs) {
  jsval_t ws_obj = mkobj(js, 0);
  
  jsval_t ws_proto = get_ctor_proto(js, "WeakSet", 7);
  if (vtype(ws_proto) == T_OBJ) set_proto(js, ws_obj, ws_proto);
  
  weakset_entry_t **ws_head = (weakset_entry_t **)ant_calloc(sizeof(weakset_entry_t *));
  if (!ws_head) return js_mkerr(js, "out of memory");
  *ws_head = NULL;
  
  jsval_t ws_ptr = mkval(T_NUM, (size_t)ws_head);
  jsval_t ws_key = js_mkstr(js, "__weakset", 9);
  setprop(js, ws_obj, ws_key, ws_ptr);
  
  if (nargs == 0 || vtype(args[0]) != T_ARR) return ws_obj;
  
  jsval_t iterable = args[0];
  jsoff_t length = js_arr_len(js, iterable);
  
  for (jsoff_t i = 0; i < length; i++) {
    jsval_t value = js_arr_get(js, iterable, i);
    
    if (vtype(value) != T_OBJ) return js_mkerr(js, "WeakSet value must be an object");
    
    weakset_entry_t *entry;
    HASH_FIND(hh, *ws_head, &value, sizeof(jsval_t), entry);
    if (entry) continue;
    
    entry = (weakset_entry_t *)ant_calloc(sizeof(weakset_entry_t));
    if (!entry) return js_mkerr(js, "out of memory");
    entry->value_obj = value;
    HASH_ADD(hh, *ws_head, value_obj, sizeof(jsval_t), entry);
  }
  
  return ws_obj;
}

static jsval_t builtin_WeakRef(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_OBJ) {
    return js_mkerr(js, "WeakRef target must be an object");
  }
  
  jsval_t wr_obj = mkobj(js, 0);
  jsval_t wr_proto = get_ctor_proto(js, "WeakRef", 7);
  if (vtype(wr_proto) == T_OBJ) set_proto(js, wr_obj, wr_proto);
  set_slot(js, wr_obj, SLOT_DATA, args[0]);
  
  return wr_obj;
}

static jsval_t weakref_deref(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t this_val = js->this_val;
  if (vtype(this_val) != T_OBJ) return js_mkundef();
  
  jsval_t target = get_slot(js, this_val, SLOT_DATA);
  if (vtype(target) != T_OBJ) return js_mkundef();
  
  return target;
}

static jsval_t builtin_FinalizationRegistry(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1 || (vtype(args[0]) != T_FUNC && vtype(args[0]) != T_CFUNC)) {
    return js_mkerr(js, "FinalizationRegistry callback must be a function");
  }
  
  jsval_t fr_obj = mkobj(js, 0);
  jsval_t fr_proto = get_ctor_proto(js, "FinalizationRegistry", 20);
  if (vtype(fr_proto) == T_OBJ) set_proto(js, fr_obj, fr_proto);
  
  set_slot(js, fr_obj, SLOT_DATA, args[0]);
  set_slot(js, fr_obj, SLOT_MAP, mkarr(js));
  
  return fr_obj;
}

static jsval_t finreg_register(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_val = js->this_val;
  if (vtype(this_val) != T_OBJ) return js_mkundef();
  
  if (nargs < 1 || vtype(args[0]) != T_OBJ) {
    return js_mkerr(js, "FinalizationRegistry.register target must be an object");
  }
  
  jsval_t target = args[0];
  jsval_t held_value = nargs > 1 ? args[1] : js_mkundef();
  jsval_t unregister_token = nargs > 2 ? args[2] : js_mkundef();
  
  if (vdata(target) == vdata(held_value) && vtype(held_value) == T_OBJ) {
    return js_mkerr(js, "target and held value must not be the same");
  }
  
  jsval_t registrations = get_slot(js, this_val, SLOT_MAP);
  if (vtype(registrations) != T_ARR) return js_mkundef();
  
  jsval_t entry = mkarr(js);
  jsoff_t len = js_arr_len(js, registrations);
  
  char idx[16];
  size_t idx_len = uint_to_str(idx, sizeof(idx), 0);
  setprop(js, entry, js_mkstr(js, idx, idx_len), target);
  idx_len = uint_to_str(idx, sizeof(idx), 1);
  setprop(js, entry, js_mkstr(js, idx, idx_len), held_value);
  idx_len = uint_to_str(idx, sizeof(idx), 2);
  setprop(js, entry, js_mkstr(js, idx, idx_len), unregister_token);
  setprop(js, entry, js_mkstr(js, "length", 6), tov(3.0));
  
  idx_len = uint_to_str(idx, sizeof(idx), len);
  setprop(js, registrations, js_mkstr(js, idx, idx_len), entry);
  setprop(js, registrations, js_mkstr(js, "length", 6), tov((double)(len + 1)));
  
  return js_mkundef();
}

static jsval_t finreg_unregister(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_val = js->this_val;
  if (vtype(this_val) != T_OBJ) return js_false;
  
  if (nargs < 1 || vtype(args[0]) != T_OBJ) {
    return js_mkerr(js, "FinalizationRegistry.unregister token must be an object");
  }
  
  jsval_t token = args[0];
  jsval_t registrations = get_slot(js, this_val, SLOT_MAP);
  if (vtype(registrations) != T_ARR) return js_false;
  
  jsoff_t len = js_arr_len(js, registrations);
  bool removed = false;
  
  for (jsoff_t i = 0; i < len; i++) {
    jsval_t entry = js_arr_get(js, registrations, i);
    if (vtype(entry) != T_ARR) continue;
    jsval_t entry_token = js_arr_get(js, entry, 2);
    if (vtype(entry_token) == T_OBJ && vdata(entry_token) == vdata(token)) {
      char idx[16]; size_t idx_len = uint_to_str(idx, sizeof(idx), i);
      setprop(js, registrations, js_mkstr(js, idx, idx_len), js_mkundef());
      removed = true;
    }
  }
  
  return js_bool(removed);
}

static proxy_data_t *get_proxy_data(jsval_t obj) {
  if (vtype(obj) != T_OBJ) return NULL;
  jsoff_t off = (jsoff_t)vdata(obj);
  proxy_data_t *data = NULL;
  HASH_FIND(hh, proxy_registry, &off, sizeof(jsoff_t), data);
  return data;
}

static bool is_proxy(struct js *js, jsval_t obj) {
  (void)js;
  return get_proxy_data(obj) != NULL;
}

static jsval_t throw_proxy_error(struct js *js, const char *message) {
  jsval_t err_obj = mkobj(js, 0);
  setprop(js, err_obj, js_mkstr(js, "message", 7), js_mkstr(js, message, strlen(message)));
  setprop(js, err_obj, js_mkstr(js, "name", 4), js_mkstr(js, "TypeError", 9));
  return js_throw(js, err_obj);
}

static jsval_t proxy_get(struct js *js, jsval_t proxy, const char *key, size_t key_len) {
  proxy_data_t *data = get_proxy_data(proxy);
  if (!data) return js_mkundef();
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'get' on a proxy that has been revoked");
  
  jsval_t target = data->target;
  jsval_t handler = data->handler;
  
  jsoff_t get_trap_off = vtype(handler) == T_OBJ ? lkp_interned(js, handler, INTERN_GET, 3) : 0;
  if (get_trap_off != 0) {
    jsval_t get_trap = resolveprop(js, mkval(T_PROP, get_trap_off));
    if (vtype(get_trap) == T_FUNC || vtype(get_trap) == T_CFUNC) {
      jsval_t key_val = js_mkstr(js, key, key_len);
      jsval_t args[3] = { target, key_val, proxy };
      return js_call(js, get_trap, args, 3);
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

static jsval_t proxy_set(struct js *js, jsval_t proxy, const char *key, size_t key_len, jsval_t value) {
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
      jsval_t result = js_call(js, set_trap, args, 4);
      if (is_err(result)) return result;
      return js_true;
    }
  }
  
  jsval_t key_str = js_mkstr(js, key, key_len);
  setprop(js, target, key_str, value);
  return js_true;
}

static jsval_t proxy_has(struct js *js, jsval_t proxy, const char *key, size_t key_len) {
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
      return js_call(js, has_trap, args, 2);
    }
  }
  
  char key_buf[256];
  size_t len = key_len < sizeof(key_buf) - 1 ? key_len : sizeof(key_buf) - 1;
  memcpy(key_buf, key, len);
  key_buf[len] = '\0';
  
  jsoff_t off = lkp_proto(js, target, key_buf, len);
  return js_bool(off != 0);
}

static jsval_t proxy_delete(struct js *js, jsval_t proxy, const char *key, size_t key_len) {
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
      return js_call(js, delete_trap, args, 2);
    }
  }
  
  jsval_t key_str = js_mkstr(js, key, key_len);
  setprop(js, target, key_str, js_mkundef());
  return js_true;
}

static jsval_t mkproxy(struct js *js, jsval_t target, jsval_t handler) {
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

static jsval_t builtin_Proxy(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t proxy_revoke_fn(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t builtin_Proxy_revocable(struct js *js, jsval_t *args, int nargs) {
  jsval_t proxy = builtin_Proxy(js, args, nargs);
  if (is_err(proxy)) return proxy;
  
  jsval_t revoke_obj = mkobj(js, 0);
  set_slot(js, revoke_obj, SLOT_CFUNC, js_mkfun(proxy_revoke_fn));
  set_slot(js, revoke_obj, SLOT_PROXY_REF, proxy);
  
  jsval_t revoke_func = mkval(T_FUNC, vdata(revoke_obj));
  
  jsval_t result = mkobj(js, 0);
  setprop(js, result, js_mkstr(js, "proxy", 5), proxy);
  setprop(js, result, js_mkstr(js, "revoke", 6), revoke_func);
  
  return result;
}

static weakmap_entry_t** get_weakmap_from_obj(struct js *js, jsval_t obj) {
  jsoff_t wm_off = lkp(js, obj, "__weakmap", 9);
  if (wm_off == 0) return NULL;
  jsval_t wm_val = resolveprop(js, mkval(T_PROP, wm_off));
  if (vtype(wm_val) != T_NUM) return NULL;
  return (weakmap_entry_t**)(size_t)vdata(wm_val);
}

static weakset_entry_t** get_weakset_from_obj(struct js *js, jsval_t obj) {
  jsoff_t ws_off = lkp(js, obj, "__weakset", 9);
  if (ws_off == 0) return NULL;
  jsval_t ws_val = resolveprop(js, mkval(T_PROP, ws_off));
  if (vtype(ws_val) != T_NUM) return NULL;
  return (weakset_entry_t**)(size_t)vdata(ws_val);
}

static jsval_t weakmap_set(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "WeakMap.set() requires 2 arguments");
  
  jsval_t this_val = js->this_val;
  weakmap_entry_t **wm_ptr = get_weakmap_from_obj(js, this_val);
  if (!wm_ptr) return js_mkerr(js, "Invalid WeakMap object");
  
  if (vtype(args[0]) != T_OBJ) {
    return js_mkerr(js, "WeakMap key must be an object");
  }
  
  jsval_t key_obj = args[0];
  
  weakmap_entry_t *entry;
  HASH_FIND(hh, *wm_ptr, &key_obj, sizeof(jsval_t), entry);
  if (entry) {
    entry->value = args[1];
  } else {
    entry = (weakmap_entry_t *)ant_calloc(sizeof(weakmap_entry_t));
    if (!entry) return js_mkerr(js, "out of memory");
    entry->key_obj = key_obj;
    entry->value = args[1];
    HASH_ADD(hh, *wm_ptr, key_obj, sizeof(jsval_t), entry);
  }
  
  return this_val;
}

static jsval_t weakmap_get(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "WeakMap.get() requires 1 argument");
  
  jsval_t this_val = js->this_val;
  weakmap_entry_t **wm_ptr = get_weakmap_from_obj(js, this_val);
  if (!wm_ptr) return js_mkundef();
  
  if (vtype(args[0]) != T_OBJ) return js_mkundef();
  
  jsval_t key_obj = args[0];
  weakmap_entry_t *entry;
  HASH_FIND(hh, *wm_ptr, &key_obj, sizeof(jsval_t), entry);
  return entry ? entry->value : js_mkundef();
}

static jsval_t weakmap_has(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "WeakMap.has() requires 1 argument");
  
  jsval_t this_val = js->this_val;
  weakmap_entry_t **wm_ptr = get_weakmap_from_obj(js, this_val);
  if (!wm_ptr) return mkval(T_BOOL, 0);
  
  if (vtype(args[0]) != T_OBJ) return mkval(T_BOOL, 0);
  
  jsval_t key_obj = args[0];
  weakmap_entry_t *entry;
  HASH_FIND(hh, *wm_ptr, &key_obj, sizeof(jsval_t), entry);
  return mkval(T_BOOL, entry ? 1 : 0);
}

static jsval_t weakmap_delete(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "WeakMap.delete() requires 1 argument");
  
  jsval_t this_val = js->this_val;
  weakmap_entry_t **wm_ptr = get_weakmap_from_obj(js, this_val);
  if (!wm_ptr) return mkval(T_BOOL, 0);
  
  if (vtype(args[0]) != T_OBJ) return mkval(T_BOOL, 0);
  
  jsval_t key_obj = args[0];
  weakmap_entry_t *entry;
  HASH_FIND(hh, *wm_ptr, &key_obj, sizeof(jsval_t), entry);
  if (entry) {
    HASH_DEL(*wm_ptr, entry);
    free(entry);
    return mkval(T_BOOL, 1);
  }
  return mkval(T_BOOL, 0);
}

static jsval_t weakset_add(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "WeakSet.add() requires 1 argument");
  
  jsval_t this_val = js->this_val;
  weakset_entry_t **ws_ptr = get_weakset_from_obj(js, this_val);
  if (!ws_ptr) return js_mkerr(js, "Invalid WeakSet object");
  
  if (vtype(args[0]) != T_OBJ) {
    return js_mkerr(js, "WeakSet value must be an object");
  }
  
  jsval_t value_obj = args[0];
  
  weakset_entry_t *entry;
  HASH_FIND(hh, *ws_ptr, &value_obj, sizeof(jsval_t), entry);
  
  if (!entry) {
    entry = (weakset_entry_t *)ant_calloc(sizeof(weakset_entry_t));
    if (!entry) return js_mkerr(js, "out of memory");
    entry->value_obj = value_obj;
    HASH_ADD(hh, *ws_ptr, value_obj, sizeof(jsval_t), entry);
  }
  
  return this_val;
}

static jsval_t weakset_has(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "WeakSet.has() requires 1 argument");
  
  jsval_t this_val = js->this_val;
  weakset_entry_t **ws_ptr = get_weakset_from_obj(js, this_val);
  if (!ws_ptr) return mkval(T_BOOL, 0);
  
  if (vtype(args[0]) != T_OBJ) return mkval(T_BOOL, 0);
  
  jsval_t value_obj = args[0];
  weakset_entry_t *entry;
  HASH_FIND(hh, *ws_ptr, &value_obj, sizeof(jsval_t), entry);
  
  return mkval(T_BOOL, entry ? 1 : 0);
}

static jsval_t weakset_delete(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "WeakSet.delete() requires 1 argument");
  
  jsval_t this_val = js->this_val;
  weakset_entry_t **ws_ptr = get_weakset_from_obj(js, this_val);
  if (!ws_ptr) return mkval(T_BOOL, 0);
  
  if (vtype(args[0]) != T_OBJ) return mkval(T_BOOL, 0);
  
  jsval_t value_obj = args[0];
  weakset_entry_t *entry;
  HASH_FIND(hh, *ws_ptr, &value_obj, sizeof(jsval_t), entry);
  
  if (entry) {
    HASH_DEL(*ws_ptr, entry);
    free(entry);
    return mkval(T_BOOL, 1);
  }
  return mkval(T_BOOL, 0);
}

ant_t *js_create(void *buf, size_t len) {
  assert(
    (uintptr_t)buf <= ((1ULL << 53) - 1) &&
    "ANT_PTR: pointer exceeds 53-bit NaN-boxing limit"
  );
  
  intern_init();
  ant_t *js = NULL;
  
  if (len < sizeof(*js) + esize(T_OBJ)) return js;
  memset(buf, 0, len);
  
  js = (struct js *) buf;
  js->mem = (uint8_t *) (js + 1);
  js->size = (jsoff_t) (len - sizeof(*js));
  js->global = mkobj(js, 0);
  js->scope = js->global;
  js->size = js->size / 8U * 8U;
  js->this_val = js->scope;
  js->super_val = js_mkundef();
  js->new_target = js_mkundef();
  js->errmsg_size = 4096;
  js->errmsg = (char *)malloc(js->errmsg_size);
  if (js->errmsg) js->errmsg[0] = '\0';
  js->gc_suppress = true;
  
#ifdef _WIN32
  js->stack_limit = 512 * 1024;
#else
  struct rlimit rl;
  if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
    js->stack_limit = rl.rlim_cur / 2;
  } else {
    js->stack_limit = 512 * 1024;
  }
#endif
  
  jsval_t glob = js->scope;
  jsval_t object_proto = js_mkobj(js);
  set_proto(js, object_proto, js_mknull());
  
  setprop(js, object_proto, js_mkstr(js, "toString", 8), js_mkfun(builtin_object_toString));
  js_set_descriptor(js, object_proto, "toString", 8, JS_DESC_W | JS_DESC_C);
  
  setprop(js, object_proto, js_mkstr(js, "valueOf", 7), js_mkfun(builtin_object_valueOf));
  js_set_descriptor(js, object_proto, "valueOf", 7, JS_DESC_W | JS_DESC_C);
  
  setprop(js, object_proto, js_mkstr(js, "toLocaleString", 14), js_mkfun(builtin_object_toLocaleString));
  js_set_descriptor(js, object_proto, "toLocaleString", 14, JS_DESC_W | JS_DESC_C);
  
  setprop(js, object_proto, js_mkstr(js, "hasOwnProperty", 14), js_mkfun(builtin_object_hasOwnProperty));
  js_set_descriptor(js, object_proto, "hasOwnProperty", 14, JS_DESC_W | JS_DESC_C);
  
  setprop(js, object_proto, js_mkstr(js, "isPrototypeOf", 13), js_mkfun(builtin_object_isPrototypeOf));
  js_set_descriptor(js, object_proto, "isPrototypeOf", 13, JS_DESC_W | JS_DESC_C);
  
  setprop(js, object_proto, js_mkstr(js, "propertyIsEnumerable", 20), js_mkfun(builtin_object_propertyIsEnumerable));
  js_set_descriptor(js, object_proto, "propertyIsEnumerable", 20, JS_DESC_W | JS_DESC_C);
  
  jsval_t proto_getter = js_mkfun(builtin_proto_getter);
  jsval_t proto_setter = js_mkfun(builtin_proto_setter);
  setprop(js, object_proto, js_mkstr(js, STR_PROTO, STR_PROTO_LEN), js_mkundef());
  js_set_accessor_desc(js, object_proto, STR_PROTO, STR_PROTO_LEN, proto_getter, proto_setter, JS_DESC_C);
  
  jsval_t function_proto_obj = js_mkobj(js);
  set_proto(js, function_proto_obj, object_proto);
  set_slot(js, function_proto_obj, SLOT_CFUNC, js_mkfun(builtin_function_empty));
  setprop(js, function_proto_obj, ANT_STRING("call"), js_mkfun(builtin_function_call));
  setprop(js, function_proto_obj, ANT_STRING("apply"), js_mkfun(builtin_function_apply));
  setprop(js, function_proto_obj, ANT_STRING("bind"), js_mkfun(builtin_function_bind));
  setprop(js, function_proto_obj, ANT_STRING("toString"), js_mkfun(builtin_function_toString));
  jsval_t function_proto = mkval(T_FUNC, vdata(function_proto_obj));
  set_slot(js, glob, SLOT_FUNC_PROTO, function_proto);
  
  jsval_t array_proto = js_mkobj(js);
  set_proto(js, array_proto, object_proto);
  setprop(js, array_proto, js_mkstr(js, "push", 4), js_mkfun(builtin_array_push));
  setprop(js, array_proto, js_mkstr(js, "pop", 3), js_mkfun(builtin_array_pop));
  setprop(js, array_proto, js_mkstr(js, "slice", 5), js_mkfun(builtin_array_slice));
  setprop(js, array_proto, js_mkstr(js, "join", 4), js_mkfun(builtin_array_join));
  setprop(js, array_proto, js_mkstr(js, "includes", 8), js_mkfun(builtin_array_includes));
  setprop(js, array_proto, js_mkstr(js, "every", 5), js_mkfun(builtin_array_every));
  setprop(js, array_proto, js_mkstr(js, "reverse", 7), js_mkfun(builtin_array_reverse));
  setprop(js, array_proto, js_mkstr(js, "map", 3), js_mkfun(builtin_array_map));
  setprop(js, array_proto, js_mkstr(js, "filter", 6), js_mkfun(builtin_array_filter));
  setprop(js, array_proto, js_mkstr(js, "reduce", 6), js_mkfun(builtin_array_reduce));
  setprop(js, array_proto, js_mkstr(js, "flat", 4), js_mkfun(builtin_array_flat));
  setprop(js, array_proto, js_mkstr(js, "concat", 6), js_mkfun(builtin_array_concat));
  setprop(js, array_proto, js_mkstr(js, "at", 2), js_mkfun(builtin_array_at));
  setprop(js, array_proto, js_mkstr(js, "fill", 4), js_mkfun(builtin_array_fill));
  setprop(js, array_proto, js_mkstr(js, "find", 4), js_mkfun(builtin_array_find));
  setprop(js, array_proto, js_mkstr(js, "findIndex", 9), js_mkfun(builtin_array_findIndex));
  setprop(js, array_proto, js_mkstr(js, "findLast", 8), js_mkfun(builtin_array_findLast));
  setprop(js, array_proto, js_mkstr(js, "findLastIndex", 13), js_mkfun(builtin_array_findLastIndex));
  setprop(js, array_proto, js_mkstr(js, "flatMap", 7), js_mkfun(builtin_array_flatMap));
  setprop(js, array_proto, js_mkstr(js, "forEach", 7), js_mkfun(builtin_array_forEach));
  setprop(js, array_proto, js_mkstr(js, "indexOf", 7), js_mkfun(builtin_array_indexOf));
  setprop(js, array_proto, js_mkstr(js, "lastIndexOf", 11), js_mkfun(builtin_array_lastIndexOf));
  setprop(js, array_proto, js_mkstr(js, "reduceRight", 11), js_mkfun(builtin_array_reduceRight));
  setprop(js, array_proto, js_mkstr(js, "shift", 5), js_mkfun(builtin_array_shift));
  setprop(js, array_proto, js_mkstr(js, "unshift", 7), js_mkfun(builtin_array_unshift));
  setprop(js, array_proto, js_mkstr(js, "some", 4), js_mkfun(builtin_array_some));
  setprop(js, array_proto, js_mkstr(js, "sort", 4), js_mkfun(builtin_array_sort));
  setprop(js, array_proto, js_mkstr(js, "splice", 6), js_mkfun(builtin_array_splice));
  setprop(js, array_proto, js_mkstr(js, "copyWithin", 10), js_mkfun(builtin_array_copyWithin));
  setprop(js, array_proto, js_mkstr(js, "toReversed", 10), js_mkfun(builtin_array_toReversed));
  setprop(js, array_proto, js_mkstr(js, "toSorted", 8), js_mkfun(builtin_array_toSorted));
  setprop(js, array_proto, js_mkstr(js, "toSpliced", 9), js_mkfun(builtin_array_toSpliced));
  setprop(js, array_proto, js_mkstr(js, "with", 4), js_mkfun(builtin_array_with));
  setprop(js, array_proto, js_mkstr(js, "keys", 4), js_mkfun(builtin_array_keys));
  setprop(js, array_proto, js_mkstr(js, "values", 6), js_mkfun(builtin_array_values));
  setprop(js, array_proto, js_mkstr(js, "entries", 7), js_mkfun(builtin_array_entries));
  setprop(js, array_proto, js_mkstr(js, "toString", 8), js_mkfun(builtin_array_toString));
  setprop(js, array_proto, js_mkstr(js, "toLocaleString", 14), js_mkfun(builtin_array_toLocaleString));
  
  jsval_t string_proto = js_mkobj(js);
  set_proto(js, string_proto, object_proto);
  setprop(js, string_proto, js_mkstr(js, "indexOf", 7), js_mkfun(builtin_string_indexOf));
  setprop(js, string_proto, js_mkstr(js, "substring", 9), js_mkfun(builtin_string_substring));
  setprop(js, string_proto, js_mkstr(js, "substr", 6), js_mkfun(builtin_string_substr));
  setprop(js, string_proto, js_mkstr(js, "split", 5), js_mkfun(builtin_string_split));
  setprop(js, string_proto, js_mkstr(js, "slice", 5), js_mkfun(builtin_string_slice));
  setprop(js, string_proto, js_mkstr(js, "includes", 8), js_mkfun(builtin_string_includes));
  setprop(js, string_proto, js_mkstr(js, "startsWith", 10), js_mkfun(builtin_string_startsWith));
  setprop(js, string_proto, js_mkstr(js, "endsWith", 8), js_mkfun(builtin_string_endsWith));
  setprop(js, string_proto, js_mkstr(js, "replace", 7), js_mkfun(builtin_string_replace));
  setprop(js, string_proto, js_mkstr(js, "replaceAll", 10), js_mkfun(builtin_string_replaceAll));
  setprop(js, string_proto, js_mkstr(js, "match", 5), js_mkfun(builtin_string_match));
  setprop(js, string_proto, js_mkstr(js, "template", 8), js_mkfun(builtin_string_template));
  setprop(js, string_proto, js_mkstr(js, "charCodeAt", 10), js_mkfun(builtin_string_charCodeAt));
  setprop(js, string_proto, js_mkstr(js, "codePointAt", 11), js_mkfun(builtin_string_codePointAt));
  setprop(js, string_proto, js_mkstr(js, "toLowerCase", 11), js_mkfun(builtin_string_toLowerCase));
  setprop(js, string_proto, js_mkstr(js, "toUpperCase", 11), js_mkfun(builtin_string_toUpperCase));
  setprop(js, string_proto, js_mkstr(js, "toLocaleLowerCase", 17), js_mkfun(builtin_string_toLowerCase));
  setprop(js, string_proto, js_mkstr(js, "toLocaleUpperCase", 17), js_mkfun(builtin_string_toUpperCase));
  setprop(js, string_proto, js_mkstr(js, "trim", 4), js_mkfun(builtin_string_trim));
  setprop(js, string_proto, js_mkstr(js, "trimStart", 9), js_mkfun(builtin_string_trimStart));
  setprop(js, string_proto, js_mkstr(js, "trimEnd", 7), js_mkfun(builtin_string_trimEnd));
  setprop(js, string_proto, js_mkstr(js, "repeat", 6), js_mkfun(builtin_string_repeat));
  setprop(js, string_proto, js_mkstr(js, "padStart", 8), js_mkfun(builtin_string_padStart));
  setprop(js, string_proto, js_mkstr(js, "padEnd", 6), js_mkfun(builtin_string_padEnd));
  setprop(js, string_proto, js_mkstr(js, "charAt", 6), js_mkfun(builtin_string_charAt));
  setprop(js, string_proto, js_mkstr(js, "at", 2), js_mkfun(builtin_string_at));
  setprop(js, string_proto, js_mkstr(js, "lastIndexOf", 11), js_mkfun(builtin_string_lastIndexOf));
  setprop(js, string_proto, js_mkstr(js, "concat", 6), js_mkfun(builtin_string_concat));
  setprop(js, string_proto, js_mkstr(js, "search", 6), js_mkfun(builtin_string_search));
  setprop(js, string_proto, js_mkstr(js, "localeCompare", 13), js_mkfun(builtin_string_localeCompare));
  setprop(js, string_proto, js_mkstr(js, "valueOf", 7), js_mkfun(builtin_string_valueOf));
  setprop(js, string_proto, js_mkstr(js, "toString", 8), js_mkfun(builtin_string_toString));

  jsval_t number_proto = js_mkobj(js);
  set_proto(js, number_proto, object_proto);
  setprop(js, number_proto, js_mkstr(js, "toString", 8), js_mkfun(builtin_number_toString));
  setprop(js, number_proto, js_mkstr(js, "toFixed", 7), js_mkfun(builtin_number_toFixed));
  setprop(js, number_proto, js_mkstr(js, "toPrecision", 11), js_mkfun(builtin_number_toPrecision));
  setprop(js, number_proto, js_mkstr(js, "toExponential", 13), js_mkfun(builtin_number_toExponential));
  setprop(js, number_proto, js_mkstr(js, "valueOf", 7), js_mkfun(builtin_number_valueOf));
  setprop(js, number_proto, js_mkstr(js, "toLocaleString", 14), js_mkfun(builtin_number_toLocaleString));
  
  jsval_t boolean_proto = js_mkobj(js);
  set_proto(js, boolean_proto, object_proto);
  setprop(js, boolean_proto, js_mkstr(js, "valueOf", 7), js_mkfun(builtin_boolean_valueOf));
  setprop(js, boolean_proto, js_mkstr(js, "toString", 8), js_mkfun(builtin_boolean_toString));
  
  jsval_t bigint_proto = js_mkobj(js);
  set_proto(js, bigint_proto, object_proto);
  setprop(js, bigint_proto, js_mkstr(js, "toString", 8), js_mkfun(builtin_bigint_toString));
  
  jsval_t error_proto = js_mkobj(js);
  set_proto(js, error_proto, object_proto);
  setprop(js, error_proto, ANT_STRING("name"), ANT_STRING("Error"));
  setprop(js, error_proto, ANT_STRING("message"), js_mkstr(js, "", 0));
  
  jsval_t err_ctor_obj = mkobj(js, 0);
  set_proto(js, err_ctor_obj, function_proto);
  set_slot(js, err_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_Error));
  js_setprop_nonconfigurable(js, err_ctor_obj, "prototype", 9, error_proto);
  setprop(js, err_ctor_obj, ANT_STRING("name"), ANT_STRING("Error"));
  setprop(js, glob, ANT_STRING("Error"), mkval(T_FUNC, vdata(err_ctor_obj)));
  setprop(js, error_proto, js_mkstr(js, "constructor", 11), mkval(T_FUNC, vdata(err_ctor_obj)));
  js_set_descriptor(js, error_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  #define REGISTER_ERROR_SUBTYPE(name_str) do { \
    jsval_t proto = js_mkobj(js); \
    set_proto(js, proto, error_proto); \
    setprop(js, proto, ANT_STRING("name"), ANT_STRING(name_str)); \
    jsval_t ctor = mkobj(js, 0); \
    set_proto(js, ctor, function_proto); \
    set_slot(js, ctor, SLOT_CFUNC, js_mkfun(builtin_Error)); \
    js_setprop_nonconfigurable(js, ctor, "prototype", 9, proto); \
    setprop(js, ctor, ANT_STRING("name"), ANT_STRING(name_str)); \
    setprop(js, proto, ANT_STRING("constructor"), mkval(T_FUNC, vdata(ctor))); \
    js_set_descriptor(js, proto, "constructor", 11, JS_DESC_W | JS_DESC_C); \
    setprop(js, glob, ANT_STRING(name_str), mkval(T_FUNC, vdata(ctor))); \
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
  setprop(js, proto, ANT_STRING("name"), ANT_STRING("AggregateError"));
  jsval_t ctor = mkobj(js, 0);
  set_proto(js, ctor, function_proto);
  set_slot(js, ctor, SLOT_CFUNC, js_mkfun(builtin_AggregateError));
  js_setprop_nonconfigurable(js, ctor, "prototype", 9, proto);
  setprop(js, ctor, ANT_STRING("name"), ANT_STRING("AggregateError"));
  setprop(js, proto, ANT_STRING("constructor"), mkval(T_FUNC, vdata(ctor)));
  js_set_descriptor(js, proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  setprop(js, glob, ANT_STRING("AggregateError"), mkval(T_FUNC, vdata(ctor)));
  
  jsval_t date_proto = js_mkobj(js);
  set_proto(js, date_proto, object_proto);
  setprop(js, date_proto, js_mkstr(js, "getTime", 7), js_mkfun(builtin_Date_getTime));
  setprop(js, date_proto, js_mkstr(js, "getFullYear", 11), js_mkfun(builtin_Date_getFullYear));
  setprop(js, date_proto, js_mkstr(js, "getMonth", 8), js_mkfun(builtin_Date_getMonth));
  setprop(js, date_proto, js_mkstr(js, "getDate", 7), js_mkfun(builtin_Date_getDate));
  setprop(js, date_proto, js_mkstr(js, "getHours", 8), js_mkfun(builtin_Date_getHours));
  setprop(js, date_proto, js_mkstr(js, "getMinutes", 10), js_mkfun(builtin_Date_getMinutes));
  setprop(js, date_proto, js_mkstr(js, "getSeconds", 10), js_mkfun(builtin_Date_getSeconds));
  setprop(js, date_proto, js_mkstr(js, "getMilliseconds", 15), js_mkfun(builtin_Date_getMilliseconds));
  setprop(js, date_proto, js_mkstr(js, "getDay", 6), js_mkfun(builtin_Date_getDay));
  setprop(js, date_proto, js_mkstr(js, "getTimezoneOffset", 17), js_mkfun(builtin_Date_getTimezoneOffset));
  setprop(js, date_proto, js_mkstr(js, "getUTCFullYear", 14), js_mkfun(builtin_Date_getUTCFullYear));
  setprop(js, date_proto, js_mkstr(js, "getUTCMonth", 11), js_mkfun(builtin_Date_getUTCMonth));
  setprop(js, date_proto, js_mkstr(js, "getUTCDate", 10), js_mkfun(builtin_Date_getUTCDate));
  setprop(js, date_proto, js_mkstr(js, "getUTCHours", 11), js_mkfun(builtin_Date_getUTCHours));
  setprop(js, date_proto, js_mkstr(js, "getUTCMinutes", 13), js_mkfun(builtin_Date_getUTCMinutes));
  setprop(js, date_proto, js_mkstr(js, "getUTCSeconds", 13), js_mkfun(builtin_Date_getUTCSeconds));
  setprop(js, date_proto, js_mkstr(js, "getUTCMilliseconds", 18), js_mkfun(builtin_Date_getUTCMilliseconds));
  setprop(js, date_proto, js_mkstr(js, "getUTCDay", 9), js_mkfun(builtin_Date_getUTCDay));
  setprop(js, date_proto, js_mkstr(js, "setTime", 7), js_mkfun(builtin_Date_setTime));
  setprop(js, date_proto, js_mkstr(js, "setMilliseconds", 15), js_mkfun(builtin_Date_setMilliseconds));
  setprop(js, date_proto, js_mkstr(js, "setSeconds", 10), js_mkfun(builtin_Date_setSeconds));
  setprop(js, date_proto, js_mkstr(js, "setMinutes", 10), js_mkfun(builtin_Date_setMinutes));
  setprop(js, date_proto, js_mkstr(js, "setHours", 8), js_mkfun(builtin_Date_setHours));
  setprop(js, date_proto, js_mkstr(js, "setDate", 7), js_mkfun(builtin_Date_setDate));
  setprop(js, date_proto, js_mkstr(js, "setMonth", 8), js_mkfun(builtin_Date_setMonth));
  setprop(js, date_proto, js_mkstr(js, "setFullYear", 11), js_mkfun(builtin_Date_setFullYear));
  setprop(js, date_proto, js_mkstr(js, "setUTCMilliseconds", 18), js_mkfun(builtin_Date_setUTCMilliseconds));
  setprop(js, date_proto, js_mkstr(js, "setUTCSeconds", 13), js_mkfun(builtin_Date_setUTCSeconds));
  setprop(js, date_proto, js_mkstr(js, "setUTCMinutes", 13), js_mkfun(builtin_Date_setUTCMinutes));
  setprop(js, date_proto, js_mkstr(js, "setUTCHours", 11), js_mkfun(builtin_Date_setUTCHours));
  setprop(js, date_proto, js_mkstr(js, "setUTCDate", 10), js_mkfun(builtin_Date_setUTCDate));
  setprop(js, date_proto, js_mkstr(js, "setUTCMonth", 11), js_mkfun(builtin_Date_setUTCMonth));
  setprop(js, date_proto, js_mkstr(js, "setUTCFullYear", 14), js_mkfun(builtin_Date_setUTCFullYear));
  setprop(js, date_proto, js_mkstr(js, "valueOf", 7), js_mkfun(builtin_Date_valueOf));
  setprop(js, date_proto, js_mkstr(js, "toISOString", 11), js_mkfun(builtin_Date_toISOString));
  setprop(js, date_proto, js_mkstr(js, "toUTCString", 11), js_mkfun(builtin_Date_toUTCString));
  setprop(js, date_proto, js_mkstr(js, "toGMTString", 11), js_mkfun(builtin_Date_toUTCString));
  setprop(js, date_proto, js_mkstr(js, "toString", 8), js_mkfun(builtin_Date_toString));
  setprop(js, date_proto, js_mkstr(js, "toDateString", 12), js_mkfun(builtin_Date_toDateString));
  setprop(js, date_proto, js_mkstr(js, "toTimeString", 12), js_mkfun(builtin_Date_toTimeString));
  setprop(js, date_proto, js_mkstr(js, "toLocaleDateString", 18), js_mkfun(builtin_Date_toLocaleDateString));
  setprop(js, date_proto, js_mkstr(js, "toLocaleTimeString", 18), js_mkfun(builtin_Date_toLocaleTimeString));
  setprop(js, date_proto, js_mkstr(js, "getYear", 7), js_mkfun(builtin_Date_getYear));
  setprop(js, date_proto, js_mkstr(js, "setYear", 7), js_mkfun(builtin_Date_setYear));
  setprop(js, date_proto, js_mkstr(js, "toJSON", 6), js_mkfun(builtin_Date_toJSON));
  
  jsval_t regexp_proto = js_mkobj(js);
  set_proto(js, regexp_proto, object_proto);
  setprop(js, regexp_proto, js_mkstr(js, "test", 4), js_mkfun(builtin_regexp_test));
  setprop(js, regexp_proto, js_mkstr(js, "exec", 4), js_mkfun(builtin_regexp_exec));
  setprop(js, regexp_proto, js_mkstr(js, "toString", 8), js_mkfun(builtin_regexp_toString));

  jsval_t map_proto = js_mkobj(js);
  set_proto(js, map_proto, object_proto);
  setprop(js, map_proto, js_mkstr(js, "set", 3), js_mkfun(map_set));
  setprop(js, map_proto, js_mkstr(js, "get", 3), js_mkfun(map_get));
  setprop(js, map_proto, js_mkstr(js, "has", 3), js_mkfun(map_has));
  setprop(js, map_proto, js_mkstr(js, "delete", 6), js_mkfun(map_delete));
  setprop(js, map_proto, js_mkstr(js, "clear", 5), js_mkfun(map_clear));
  setprop(js, map_proto, js_mkstr(js, "size", 4), js_mkfun(map_size));
  setprop(js, map_proto, js_mkstr(js, "entries", 7), js_mkfun(map_entries));
  
  jsval_t set_proto_obj = js_mkobj(js);
  set_proto(js, set_proto_obj, object_proto);
  setprop(js, set_proto_obj, js_mkstr(js, "add", 3), js_mkfun(set_add));
  setprop(js, set_proto_obj, js_mkstr(js, "has", 3), js_mkfun(set_has));
  setprop(js, set_proto_obj, js_mkstr(js, "delete", 6), js_mkfun(set_delete));
  setprop(js, set_proto_obj, js_mkstr(js, "clear", 5), js_mkfun(set_clear));
  setprop(js, set_proto_obj, js_mkstr(js, "size", 4), js_mkfun(set_size));
  setprop(js, set_proto_obj, js_mkstr(js, "values", 6), js_mkfun(set_values));
  
  jsval_t weakmap_proto = js_mkobj(js);
  set_proto(js, weakmap_proto, object_proto);
  setprop(js, weakmap_proto, js_mkstr(js, "set", 3), js_mkfun(weakmap_set));
  setprop(js, weakmap_proto, js_mkstr(js, "get", 3), js_mkfun(weakmap_get));
  setprop(js, weakmap_proto, js_mkstr(js, "has", 3), js_mkfun(weakmap_has));
  setprop(js, weakmap_proto, js_mkstr(js, "delete", 6), js_mkfun(weakmap_delete));
  
  jsval_t weakset_proto = js_mkobj(js);
  set_proto(js, weakset_proto, object_proto);
  setprop(js, weakset_proto, js_mkstr(js, "add", 3), js_mkfun(weakset_add));
  setprop(js, weakset_proto, js_mkstr(js, "has", 3), js_mkfun(weakset_has));
  setprop(js, weakset_proto, js_mkstr(js, "delete", 6), js_mkfun(weakset_delete));
  
  jsval_t weakref_proto = js_mkobj(js);
  set_proto(js, weakref_proto, object_proto);
  setprop(js, weakref_proto, js_mkstr(js, "deref", 5), js_mkfun(weakref_deref));
  
  jsval_t finreg_proto = js_mkobj(js);
  set_proto(js, finreg_proto, object_proto);
  setprop(js, finreg_proto, js_mkstr(js, "register", 8), js_mkfun(finreg_register));
  setprop(js, finreg_proto, js_mkstr(js, "unregister", 10), js_mkfun(finreg_unregister));
  
  jsval_t promise_proto = js_mkobj(js);
  set_proto(js, promise_proto, object_proto);
  setprop(js, promise_proto, js_mkstr(js, "then", 4), js_mkfun(builtin_promise_then));
  setprop(js, promise_proto, js_mkstr(js, "catch", 5), js_mkfun(builtin_promise_catch));
  setprop(js, promise_proto, js_mkstr(js, "finally", 7), js_mkfun(builtin_promise_finally));
  
  jsval_t obj_func_obj = mkobj(js, 0);
  set_proto(js, obj_func_obj, function_proto);
  set_slot(js, obj_func_obj, SLOT_BUILTIN, tov(BUILTIN_OBJECT));
  setprop(js, obj_func_obj, js_mkstr(js, "keys", 4), js_mkfun(builtin_object_keys));
  setprop(js, obj_func_obj, js_mkstr(js, "values", 6), js_mkfun(builtin_object_values));
  setprop(js, obj_func_obj, js_mkstr(js, "entries", 7), js_mkfun(builtin_object_entries));
  setprop(js, obj_func_obj, js_mkstr(js, "getPrototypeOf", 14), js_mkfun(builtin_object_getPrototypeOf));
  setprop(js, obj_func_obj, js_mkstr(js, "setPrototypeOf", 14), js_mkfun(builtin_object_setPrototypeOf));
  setprop(js, obj_func_obj, js_mkstr(js, "create", 6), js_mkfun(builtin_object_create));
  setprop(js, obj_func_obj, js_mkstr(js, "hasOwn", 6), js_mkfun(builtin_object_hasOwn));
  setprop(js, obj_func_obj, js_mkstr(js, "defineProperty", 14), js_mkfun(builtin_object_defineProperty));
  setprop(js, obj_func_obj, js_mkstr(js, "defineProperties", 16), js_mkfun(builtin_object_defineProperties));
  setprop(js, obj_func_obj, js_mkstr(js, "assign", 6), js_mkfun(builtin_object_assign));
  setprop(js, obj_func_obj, js_mkstr(js, "freeze", 6), js_mkfun(builtin_object_freeze));
  setprop(js, obj_func_obj, js_mkstr(js, "isFrozen", 8), js_mkfun(builtin_object_isFrozen));
  setprop(js, obj_func_obj, js_mkstr(js, "seal", 4), js_mkfun(builtin_object_seal));
  setprop(js, obj_func_obj, js_mkstr(js, "isSealed", 8), js_mkfun(builtin_object_isSealed));
  setprop(js, obj_func_obj, js_mkstr(js, "fromEntries", 11), js_mkfun(builtin_object_fromEntries));
  setprop(js, obj_func_obj, js_mkstr(js, "getOwnPropertyDescriptor", 24), js_mkfun(builtin_object_getOwnPropertyDescriptor));
  setprop(js, obj_func_obj, js_mkstr(js, "getOwnPropertyNames", 19), js_mkfun(builtin_object_getOwnPropertyNames));
  setprop(js, obj_func_obj, js_mkstr(js, "isExtensible", 12), js_mkfun(builtin_object_isExtensible));
  setprop(js, obj_func_obj, js_mkstr(js, "preventExtensions", 17), js_mkfun(builtin_object_preventExtensions));
  setprop(js, obj_func_obj, ANT_STRING("name"), ANT_STRING("Object"));
  js_setprop_nonconfigurable(js, obj_func_obj, "prototype", 9, object_proto);
  setprop(js, glob, js_mkstr(js, "Object", 6), mkval(T_FUNC, vdata(obj_func_obj)));
  
  jsval_t func_ctor_obj = mkobj(js, 0);
  set_proto(js, func_ctor_obj, function_proto);
  set_slot(js, func_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_Function));
  js_setprop_nonconfigurable(js, func_ctor_obj, "prototype", 9, function_proto);
  setprop(js, func_ctor_obj, js_mkstr(js, "length", 6), tov(1.0));
  js_set_descriptor(js, func_ctor_obj, "length", 6, JS_DESC_C);
  setprop(js, func_ctor_obj, ANT_STRING("name"), ANT_STRING("Function"));
  setprop(js, glob, js_mkstr(js, "Function", 8), mkval(T_FUNC, vdata(func_ctor_obj)));
  
  jsval_t async_func_proto_obj = js_mkobj(js);
  set_proto(js, async_func_proto_obj, function_proto);
  set_slot(js, async_func_proto_obj, SLOT_ASYNC, js_true);
  jsval_t async_func_proto = mkval(T_FUNC, vdata(async_func_proto_obj));
  set_slot(js, glob, SLOT_ASYNC_PROTO, async_func_proto);
  
  jsval_t async_func_ctor_obj = mkobj(js, 0);
  set_proto(js, async_func_ctor_obj, function_proto);
  set_slot(js, async_func_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_AsyncFunction));
  js_setprop_nonconfigurable(js, async_func_ctor_obj, "prototype", 9, async_func_proto);
  setprop(js, async_func_ctor_obj, js_mkstr(js, "length", 6), tov(1.0));
  js_set_descriptor(js, async_func_ctor_obj, "length", 6, JS_DESC_C);
  setprop(js, async_func_ctor_obj, ANT_STRING("name"), ANT_STRING("AsyncFunction"));
  jsval_t async_func_ctor = mkval(T_FUNC, vdata(async_func_ctor_obj));
  
  setprop(js, async_func_proto_obj, js_mkstr(js, "constructor", 11), async_func_ctor);
  js_set_descriptor(js, async_func_proto_obj, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  jsval_t str_ctor_obj = mkobj(js, 0);
  set_proto(js, str_ctor_obj, function_proto);
  set_slot(js, str_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_String));
  js_setprop_nonconfigurable(js, str_ctor_obj, "prototype", 9, string_proto);
  setprop(js, str_ctor_obj, js_mkstr(js, "fromCharCode", 12), js_mkfun(builtin_string_fromCharCode));
  setprop(js, str_ctor_obj, js_mkstr(js, "fromCodePoint", 13), js_mkfun(builtin_string_fromCodePoint));
  setprop(js, str_ctor_obj, ANT_STRING("name"), ANT_STRING("String"));
  setprop(js, glob, js_mkstr(js, "String", 6), mkval(T_FUNC, vdata(str_ctor_obj)));
  
  jsval_t number_ctor_obj = mkobj(js, 0);
  set_proto(js, number_ctor_obj, function_proto);
  
  set_slot(js, number_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_Number));
  setprop(js, number_ctor_obj, js_mkstr(js, "isNaN", 5), js_mkfun(builtin_Number_isNaN));
  setprop(js, number_ctor_obj, js_mkstr(js, "isFinite", 8), js_mkfun(builtin_Number_isFinite));
  setprop(js, number_ctor_obj, js_mkstr(js, "isInteger", 9), js_mkfun(builtin_Number_isInteger));
  setprop(js, number_ctor_obj, js_mkstr(js, "isSafeInteger", 13), js_mkfun(builtin_Number_isSafeInteger));
  
  setprop(js, number_ctor_obj, js_mkstr(js, "MAX_VALUE", 9), tov(1.7976931348623157e+308));
  setprop(js, number_ctor_obj, js_mkstr(js, "MIN_VALUE", 9), tov(5e-324));
  setprop(js, number_ctor_obj, js_mkstr(js, "MAX_SAFE_INTEGER", 16), tov(9007199254740991.0));
  setprop(js, number_ctor_obj, js_mkstr(js, "MIN_SAFE_INTEGER", 16), tov(-9007199254740991.0));
  setprop(js, number_ctor_obj, js_mkstr(js, "POSITIVE_INFINITY", 17), tov(JS_INF));
  setprop(js, number_ctor_obj, js_mkstr(js, "NEGATIVE_INFINITY", 17), tov(JS_NEG_INF));
  setprop(js, number_ctor_obj, js_mkstr(js, "NaN", 3), tov(JS_NAN));
  setprop(js, number_ctor_obj, js_mkstr(js, "EPSILON", 7), tov(2.220446049250313e-16));
  
  js_setprop_nonconfigurable(js, number_ctor_obj, "prototype", 9, number_proto);
  setprop(js, number_ctor_obj, ANT_STRING("name"), ANT_STRING("Number"));
  setprop(js, glob, js_mkstr(js, "Number", 6), mkval(T_FUNC, vdata(number_ctor_obj)));
  
  jsval_t bool_ctor_obj = mkobj(js, 0);
  set_proto(js, bool_ctor_obj, function_proto);
  set_slot(js, bool_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_Boolean));
  js_setprop_nonconfigurable(js, bool_ctor_obj, "prototype", 9, boolean_proto);
  setprop(js, bool_ctor_obj, ANT_STRING("name"), ANT_STRING("Boolean"));
  setprop(js, glob, js_mkstr(js, "Boolean", 7), mkval(T_FUNC, vdata(bool_ctor_obj)));
  
  jsval_t arr_ctor_obj = mkobj(js, 0);
  set_proto(js, arr_ctor_obj, function_proto);
  set_slot(js, arr_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_Array));
  js_setprop_nonconfigurable(js, arr_ctor_obj, "prototype", 9, array_proto);
  setprop(js, arr_ctor_obj, js_mkstr(js, "isArray", 7), js_mkfun(builtin_Array_isArray));
  setprop(js, arr_ctor_obj, js_mkstr(js, "from", 4), js_mkfun(builtin_Array_from));
  setprop(js, arr_ctor_obj, js_mkstr(js, "of", 2), js_mkfun(builtin_Array_of));
  setprop(js, arr_ctor_obj, js_mkstr(js, "length", 6), tov(1.0));
  js_set_descriptor(js, arr_ctor_obj, "length", 6, JS_DESC_C);
  setprop(js, arr_ctor_obj, ANT_STRING("name"), ANT_STRING("Array"));
  setprop(js, glob, js_mkstr(js, "Array", 5), mkval(T_FUNC, vdata(arr_ctor_obj)));

  jsval_t map_ctor_obj = mkobj(js, 0);
  set_proto(js, map_ctor_obj, function_proto);
  set_slot(js, map_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_Map));
  js_setprop_nonconfigurable(js, map_ctor_obj, "prototype", 9, map_proto);
  setprop(js, map_ctor_obj, ANT_STRING("name"), ANT_STRING("Map"));
  setprop(js, glob, js_mkstr(js, "Map", 3), mkval(T_FUNC, vdata(map_ctor_obj)));
  
  jsval_t set_ctor_obj = mkobj(js, 0);
  set_proto(js, set_ctor_obj, function_proto);
  set_slot(js, set_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_Set));
  js_setprop_nonconfigurable(js, set_ctor_obj, "prototype", 9, set_proto_obj);
  setprop(js, set_ctor_obj, ANT_STRING("name"), ANT_STRING("Set"));
  setprop(js, glob, js_mkstr(js, "Set", 3), mkval(T_FUNC, vdata(set_ctor_obj)));
  
  jsval_t weakmap_ctor_obj = mkobj(js, 0);
  set_proto(js, weakmap_ctor_obj, function_proto);
  set_slot(js, weakmap_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_WeakMap));
  js_setprop_nonconfigurable(js, weakmap_ctor_obj, "prototype", 9, weakmap_proto);
  setprop(js, weakmap_ctor_obj, ANT_STRING("name"), ANT_STRING("WeakMap"));
  setprop(js, glob, js_mkstr(js, "WeakMap", 7), mkval(T_FUNC, vdata(weakmap_ctor_obj)));
  
  jsval_t weakset_ctor_obj = mkobj(js, 0);
  set_proto(js, weakset_ctor_obj, function_proto);
  set_slot(js, weakset_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_WeakSet));
  js_setprop_nonconfigurable(js, weakset_ctor_obj, "prototype", 9, weakset_proto);
  setprop(js, weakset_ctor_obj, ANT_STRING("name"), ANT_STRING("WeakSet"));
  setprop(js, glob, js_mkstr(js, "WeakSet", 7), mkval(T_FUNC, vdata(weakset_ctor_obj)));
  
  jsval_t weakref_ctor_obj = mkobj(js, 0);
  set_proto(js, weakref_ctor_obj, function_proto);
  set_slot(js, weakref_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_WeakRef));
  js_setprop_nonconfigurable(js, weakref_ctor_obj, "prototype", 9, weakref_proto);
  setprop(js, weakref_ctor_obj, ANT_STRING("name"), ANT_STRING("WeakRef"));
  setprop(js, glob, js_mkstr(js, "WeakRef", 7), mkval(T_FUNC, vdata(weakref_ctor_obj)));
  
  jsval_t finreg_ctor_obj = mkobj(js, 0);
  set_proto(js, finreg_ctor_obj, function_proto);
  set_slot(js, finreg_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_FinalizationRegistry));
  js_setprop_nonconfigurable(js, finreg_ctor_obj, "prototype", 9, finreg_proto);
  setprop(js, finreg_ctor_obj, ANT_STRING("name"), ANT_STRING("FinalizationRegistry"));
  setprop(js, glob, js_mkstr(js, "FinalizationRegistry", 20), mkval(T_FUNC, vdata(finreg_ctor_obj)));
  
  jsval_t proxy_ctor_obj = mkobj(js, 0);
  set_proto(js, proxy_ctor_obj, function_proto);
  set_slot(js, proxy_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_Proxy));
  setprop(js, proxy_ctor_obj, js_mkstr(js, "revocable", 9), js_mkfun(builtin_Proxy_revocable));
  setprop(js, proxy_ctor_obj, ANT_STRING("name"), ANT_STRING("Proxy"));
  setprop(js, glob, js_mkstr(js, "Proxy", 5), mkval(T_FUNC, vdata(proxy_ctor_obj)));
  

  
  jsval_t regex_ctor_obj = mkobj(js, 0);
  set_proto(js, regex_ctor_obj, function_proto);
  set_slot(js, regex_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_RegExp));
  js_setprop_nonconfigurable(js, regex_ctor_obj, "prototype", 9, regexp_proto);
  setprop(js, regex_ctor_obj, ANT_STRING("name"), ANT_STRING("RegExp"));
  setprop(js, glob, js_mkstr(js, "RegExp", 6), mkval(T_FUNC, vdata(regex_ctor_obj)));
  
  jsval_t date_ctor_obj = mkobj(js, 0);
  set_proto(js, date_ctor_obj, function_proto);
  set_slot(js, date_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_Date));
  setprop(js, date_ctor_obj, js_mkstr(js, "now", 3), js_mkfun(builtin_Date_now));
  setprop(js, date_ctor_obj, js_mkstr(js, "UTC", 3), js_mkfun(builtin_Date_UTC));
  js_setprop_nonconfigurable(js, date_ctor_obj, "prototype", 9, date_proto);
  setprop(js, date_ctor_obj, ANT_STRING("name"), ANT_STRING("Date"));
  setprop(js, glob, js_mkstr(js, "Date", 4), mkval(T_FUNC, vdata(date_ctor_obj)));
  
  jsval_t p_ctor_obj = mkobj(js, 0);
  set_proto(js, p_ctor_obj, function_proto);
  set_slot(js, p_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_Promise));
  setprop(js, p_ctor_obj, js_mkstr(js, "resolve", 7), js_mkfun(builtin_Promise_resolve));
  setprop(js, p_ctor_obj, js_mkstr(js, "reject", 6), js_mkfun(builtin_Promise_reject));
  setprop(js, p_ctor_obj, js_mkstr(js, "try", 3), js_mkfun(builtin_Promise_try));
  setprop(js, p_ctor_obj, js_mkstr(js, "all", 3), js_mkfun(builtin_Promise_all));
  setprop(js, p_ctor_obj, js_mkstr(js, "race", 4), js_mkfun(builtin_Promise_race));
  setprop(js, p_ctor_obj, js_mkstr(js, "any", 3), js_mkfun(builtin_Promise_any));
  js_setprop_nonconfigurable(js, p_ctor_obj, "prototype", 9, promise_proto);
  setprop(js, p_ctor_obj, ANT_STRING("name"), ANT_STRING("Promise"));
  setprop(js, glob, js_mkstr(js, "Promise", 7), mkval(T_FUNC, vdata(p_ctor_obj)));
  
  jsval_t bigint_ctor_obj = mkobj(js, 0);
  set_proto(js, bigint_ctor_obj, function_proto);
  set_slot(js, bigint_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_BigInt));
  setprop(js, bigint_ctor_obj, js_mkstr(js, "asIntN", 6), js_mkfun(builtin_BigInt_asIntN));
  setprop(js, bigint_ctor_obj, js_mkstr(js, "asUintN", 7), js_mkfun(builtin_BigInt_asUintN));
  js_setprop_nonconfigurable(js, bigint_ctor_obj, "prototype", 9, bigint_proto);
  setprop(js, bigint_ctor_obj, ANT_STRING("name"), ANT_STRING("BigInt"));
  setprop(js, glob, js_mkstr(js, "BigInt", 6), mkval(T_FUNC, vdata(bigint_ctor_obj)));
  
  setprop(js, glob, js_mkstr(js, "eval", 4), js_mkfun(builtin_eval));
  setprop(js, glob, js_mkstr(js, "parseInt", 8), js_mkfun(builtin_parseInt));
  setprop(js, glob, js_mkstr(js, "parseFloat", 10), js_mkfun(builtin_parseFloat));
  setprop(js, glob, js_mkstr(js, "isNaN", 5), js_mkfun(builtin_global_isNaN));
  setprop(js, glob, js_mkstr(js, "isFinite", 8), js_mkfun(builtin_global_isFinite));
  setprop(js, glob, js_mkstr(js, "btoa", 4), js_mkfun(builtin_btoa));
  setprop(js, glob, js_mkstr(js, "atob", 4), js_mkfun(builtin_atob));
  setprop(js, glob, js_mkstr(js, "NaN", 3), tov(JS_NAN));
  setprop(js, glob, js_mkstr(js, "Infinity", 8), tov(JS_INF));
  setprop(js, glob, js_mkstr(js, "undefined", 9), js_mkundef());
  
  jsval_t math_obj = mkobj(js, 0);
  set_proto(js, math_obj, object_proto);
  setprop(js, math_obj, js_mkstr(js, "E", 1), tov(M_E));
  setprop(js, math_obj, js_mkstr(js, "LN10", 4), tov(M_LN10));
  setprop(js, math_obj, js_mkstr(js, "LN2", 3), tov(M_LN2));
  setprop(js, math_obj, js_mkstr(js, "LOG10E", 6), tov(M_LOG10E));
  setprop(js, math_obj, js_mkstr(js, "LOG2E", 5), tov(M_LOG2E));
  setprop(js, math_obj, js_mkstr(js, "PI", 2), tov(M_PI));
  setprop(js, math_obj, js_mkstr(js, "SQRT1_2", 7), tov(M_SQRT1_2));
  setprop(js, math_obj, js_mkstr(js, "SQRT2", 5), tov(M_SQRT2));
  setprop(js, math_obj, js_mkstr(js, "abs", 3), js_mkfun(builtin_Math_abs));
  setprop(js, math_obj, js_mkstr(js, "acos", 4), js_mkfun(builtin_Math_acos));
  setprop(js, math_obj, js_mkstr(js, "acosh", 5), js_mkfun(builtin_Math_acosh));
  setprop(js, math_obj, js_mkstr(js, "asin", 4), js_mkfun(builtin_Math_asin));
  setprop(js, math_obj, js_mkstr(js, "asinh", 5), js_mkfun(builtin_Math_asinh));
  setprop(js, math_obj, js_mkstr(js, "atan", 4), js_mkfun(builtin_Math_atan));
  setprop(js, math_obj, js_mkstr(js, "atanh", 5), js_mkfun(builtin_Math_atanh));
  setprop(js, math_obj, js_mkstr(js, "atan2", 5), js_mkfun(builtin_Math_atan2));
  setprop(js, math_obj, js_mkstr(js, "cbrt", 4), js_mkfun(builtin_Math_cbrt));
  setprop(js, math_obj, js_mkstr(js, "ceil", 4), js_mkfun(builtin_Math_ceil));
  setprop(js, math_obj, js_mkstr(js, "clz32", 5), js_mkfun(builtin_Math_clz32));
  setprop(js, math_obj, js_mkstr(js, "cos", 3), js_mkfun(builtin_Math_cos));
  setprop(js, math_obj, js_mkstr(js, "cosh", 4), js_mkfun(builtin_Math_cosh));
  setprop(js, math_obj, js_mkstr(js, "exp", 3), js_mkfun(builtin_Math_exp));
  setprop(js, math_obj, js_mkstr(js, "expm1", 5), js_mkfun(builtin_Math_expm1));
  setprop(js, math_obj, js_mkstr(js, "floor", 5), js_mkfun(builtin_Math_floor));
  setprop(js, math_obj, js_mkstr(js, "fround", 6), js_mkfun(builtin_Math_fround));
  setprop(js, math_obj, js_mkstr(js, "hypot", 5), js_mkfun(builtin_Math_hypot));
  setprop(js, math_obj, js_mkstr(js, "imul", 4), js_mkfun(builtin_Math_imul));
  setprop(js, math_obj, js_mkstr(js, "log", 3), js_mkfun(builtin_Math_log));
  setprop(js, math_obj, js_mkstr(js, "log1p", 5), js_mkfun(builtin_Math_log1p));
  setprop(js, math_obj, js_mkstr(js, "log10", 5), js_mkfun(builtin_Math_log10));
  setprop(js, math_obj, js_mkstr(js, "log2", 4), js_mkfun(builtin_Math_log2));
  setprop(js, math_obj, js_mkstr(js, "max", 3), js_mkfun(builtin_Math_max));
  setprop(js, math_obj, js_mkstr(js, "min", 3), js_mkfun(builtin_Math_min));
  setprop(js, math_obj, js_mkstr(js, "pow", 3), js_mkfun(builtin_Math_pow));
  setprop(js, math_obj, js_mkstr(js, "random", 6), js_mkfun(builtin_Math_random));
  setprop(js, math_obj, js_mkstr(js, "round", 5), js_mkfun(builtin_Math_round));
  setprop(js, math_obj, js_mkstr(js, "sign", 4), js_mkfun(builtin_Math_sign));
  setprop(js, math_obj, js_mkstr(js, "sin", 3), js_mkfun(builtin_Math_sin));
  setprop(js, math_obj, js_mkstr(js, "sinh", 4), js_mkfun(builtin_Math_sinh));
  setprop(js, math_obj, js_mkstr(js, "sqrt", 4), js_mkfun(builtin_Math_sqrt));
  setprop(js, math_obj, js_mkstr(js, "tan", 3), js_mkfun(builtin_Math_tan));
  setprop(js, math_obj, js_mkstr(js, "tanh", 4), js_mkfun(builtin_Math_tanh));
  setprop(js, math_obj, js_mkstr(js, "trunc", 5), js_mkfun(builtin_Math_trunc));
  setprop(js, glob, js_mkstr(js, "Math", 4), math_obj);
  
  jsval_t import_obj = mkobj(js, 0);
  set_proto(js, import_obj, function_proto);
  
  set_slot(js, import_obj, SLOT_CFUNC, js_mkfun(builtin_import));
  setprop(js, glob, js_mkstr(js, "import", 6), mkval(T_FUNC, vdata(import_obj)));
  js->module_ns = js_mkundef();
  
  setprop(js, object_proto, js_mkstr(js, "constructor", 11), mkval(T_FUNC, vdata(obj_func_obj)));
  js_set_descriptor(js, object_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  setprop(js, function_proto, js_mkstr(js, "constructor", 11), mkval(T_FUNC, vdata(func_ctor_obj)));
  js_set_descriptor(js, function_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  setprop(js, array_proto, js_mkstr(js, "constructor", 11), mkval(T_FUNC, vdata(arr_ctor_obj)));
  js_set_descriptor(js, array_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  setprop(js, string_proto, js_mkstr(js, "constructor", 11), mkval(T_FUNC, vdata(str_ctor_obj)));
  js_set_descriptor(js, string_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  setprop(js, number_proto, js_mkstr(js, "constructor", 11), mkval(T_FUNC, vdata(number_ctor_obj)));
  js_set_descriptor(js, number_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  setprop(js, boolean_proto, js_mkstr(js, "constructor", 11), mkval(T_FUNC, vdata(bool_ctor_obj)));
  js_set_descriptor(js, boolean_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  setprop(js, date_proto, js_mkstr(js, "constructor", 11), mkval(T_FUNC, vdata(date_ctor_obj)));
  js_set_descriptor(js, date_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  setprop(js, regexp_proto, js_mkstr(js, "constructor", 11), mkval(T_FUNC, vdata(regex_ctor_obj)));
  js_set_descriptor(js, regexp_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  set_proto(js, glob, object_proto);
  
  js->owns_mem = false;
  js->max_size = 0;
  
  return js;
}

struct js *js_create_dynamic(size_t initial_size, size_t max_size) {
  if (initial_size < sizeof(struct js) + esize(T_OBJ)) initial_size = ANT_ARENA_MIN;
  if (max_size == 0 || max_size < initial_size) max_size = ANT_ARENA_MAX;
  
  void *init_buf = ant_calloc(initial_size);
  if (init_buf == NULL) return NULL;
  
  struct js *js = js_create(init_buf, initial_size);
  if (js == NULL) { free(init_buf); return NULL; }
  
  uint8_t *arena = (uint8_t *)ant_arena_reserve(max_size);
  if (arena == NULL) { free(init_buf); return NULL; }
  
  if (ant_arena_commit(arena, 0, js->size) != 0) {
    ant_arena_free(arena, max_size);
    free(init_buf); return NULL;
  }
  
  memcpy(arena, js->mem, js->brk);
  js->mem = arena;
  
  js->owns_mem = true;
  js->max_size = (jsoff_t) max_size;
  
  struct js *new_js = (struct js *)malloc(sizeof(struct js));
  if (new_js == NULL) {
    ant_arena_free(arena, max_size);
    free(init_buf);
    return NULL;
  }
  
  memcpy(new_js, js, sizeof(struct js));
  free(init_buf);

  return new_js;
}

void js_destroy(struct js *js) {
  if (js == NULL) return;
  
  esm_cleanup_module_cache();
  code_arena_reset();
  cleanup_buffer_module();
  
  if (js->errmsg) {
    free(js->errmsg);
    js->errmsg = NULL;
  }
  
  if (js->for_let_stack) {
    free(js->for_let_stack);
    js->for_let_stack = NULL;
  }
  
  if (js->owns_mem) {
    ant_arena_free(js->mem, js->max_size);
    free(js);
  }
}

inline double js_getnum(jsval_t value) { return tod(value); }
inline void js_setstacklimit(struct js *js, size_t max) { js->stack_limit = max; }
inline void js_setstackbase(struct js *js, void *base) { js->cstk = base; }
inline void js_set_filename(struct js *js, const char *filename) { js->filename = filename; }

inline jsval_t js_mkundef(void) { return mkval(T_UNDEF, 0); }
inline jsval_t js_mknull(void) { return mkval(T_NULL, 0); }
inline jsval_t js_mknum(double value) { return tov(value); }
inline jsval_t js_mkobj(struct js *js) { return mkobj(js, 0); }
inline jsval_t js_glob(struct js *js) { return js->global; }
inline jsval_t js_getscope(struct js *js) { return js->scope; }
inline jsval_t js_mkfun(jsval_t (*fn)(struct js *, jsval_t *, int)) { return mkval(T_CFUNC, (size_t) (void *) fn); }

inline jsval_t js_getthis(struct js *js) { return js->this_val; }
inline void js_setthis(struct js *js, jsval_t val) { js->this_val = val; }
inline jsval_t js_getcurrentfunc(struct js *js) { return js->current_func; }

jsval_t js_heavy_mkfun(struct js *js, jsval_t (*fn)(struct js *, jsval_t *, int), jsval_t data) {
  jsval_t cfunc = js_mkfun(fn);
  jsval_t fn_obj = mkobj(js, 0);
  
  set_slot(js, fn_obj, SLOT_CFUNC, cfunc);
  set_slot(js, fn_obj, SLOT_DATA, data);
  
  return mkval(T_FUNC, (unsigned long)vdata(fn_obj));
}

void js_set(struct js *js, jsval_t obj, const char *key, jsval_t val) {
  size_t key_len = strlen(key);
  
  if (vtype(obj) == T_OBJ) {
    jsoff_t existing = lkp(js, obj, key, key_len);
    if (existing > 0) {
      if (is_const_prop(js, existing)) {
        if (js->flags & F_STRICT) js_mkerr(js, "assignment to constant");
        return;
      }
      saveval(js, existing + sizeof(jsoff_t) * 2, val);
    } else {
      jsval_t key_str = js_mkstr(js, key, key_len);
      mkprop(js, obj, key_str, val, 0);
    }
  } else if (vtype(obj) == T_FUNC) {
    jsval_t func_obj = mkval(T_OBJ, vdata(obj));
    jsoff_t existing = lkp(js, func_obj, key, key_len);
    if (existing > 0) {
      if (is_const_prop(js, existing)) {
        if (js->flags & F_STRICT) js_mkerr(js, "assignment to constant");
        return;
      }
      saveval(js, existing + sizeof(jsoff_t) * 2, val);
    } else {
      jsval_t key_str = js_mkstr(js, key, key_len);
      mkprop(js, func_obj, key_str, val, 0);
    }
  }
}

bool js_del(struct js *js, jsval_t obj, const char *key) {
  size_t len = strlen(key);
  jsoff_t obj_off;
  
  if (vtype(obj) == T_OBJ) {
    obj_off = (jsoff_t)vdata(obj);
  } else if (vtype(obj) == T_ARR || vtype(obj) == T_FUNC) {
    obj_off = (jsoff_t)vdata(obj);
    obj = mkval(T_OBJ, obj_off);
  } else {
    return false;
  }
  
  jsoff_t prop_off = lkp(js, obj, key, len);
  if (prop_off == 0) return true;
  if (is_nonconfig_prop(js, prop_off)) return false;
  
  descriptor_entry_t *desc = lookup_descriptor(obj_off, key, len);
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

jsval_t js_get(struct js *js, jsval_t obj, const char *key) {
  size_t key_len = strlen(key);
  
  if (vtype(obj) == T_FUNC) {
    jsval_t func_obj = mkval(T_OBJ, vdata(obj));
    jsoff_t off = lkp(js, func_obj, key, key_len);
    return off == 0 ? js_mkundef() : resolveprop(js, mkval(T_PROP, off));
  }
  
  if (vtype(obj) == T_ARR) {
    jsval_t arr_obj = mkval(T_OBJ, vdata(obj));
    jsoff_t off = lkp(js, arr_obj, key, key_len);
    return off == 0 ? js_mkundef() : resolveprop(js, mkval(T_PROP, off));
  }

  uint8_t t = vtype(obj);
  bool is_promise = (t == T_PROMISE);
  if (is_promise) obj = mkval(T_OBJ, vdata(obj));
  else if (t != T_OBJ) return js_mkundef();
  jsoff_t off = lkp(js, obj, key, key_len);
  
  if (off == 0) {
    jsval_t result = try_dynamic_getter(js, obj, key, key_len);
    if (vtype(result) != T_UNDEF) return result;
  }
  
  if (off == 0 && is_promise) {
    jsval_t promise_proto = get_ctor_proto(js, "Promise", 7);
    if (vtype(promise_proto) != T_UNDEF && vtype(promise_proto) != T_NULL) {
      off = lkp(js, promise_proto, key, key_len);
      if (off != 0) return resolveprop(js, mkval(T_PROP, off));
    }
  }
  
  return off == 0 ? js_mkundef() : resolveprop(js, mkval(T_PROP, off));
}

jsval_t js_getprop_proto(struct js *js, jsval_t obj, const char *key) {
  size_t key_len = strlen(key);
  jsoff_t off = lkp_proto(js, obj, key, key_len);
  return off == 0 ? js_mkundef() : resolveprop(js, mkval(T_PROP, off));
}

typedef struct {
  bool (*callback)(struct js *js, jsval_t value, void *udata);
  void *udata;
} js_iter_ctx_t;

static iter_action_t js_iter_cb(struct js *js, jsval_t value, void *ctx, jsval_t *out) {
  (void)out;
  js_iter_ctx_t *ictx = (js_iter_ctx_t *)ctx;
  return ictx->callback(js, value, ictx->udata) ? ITER_CONTINUE : ITER_BREAK;
}

bool js_iter(struct js *js, jsval_t iterable, bool (*callback)(struct js *js, jsval_t value, void *udata), void *udata) {
  js_iter_ctx_t ctx = { .callback = callback, .udata = udata };
  jsval_t result = iter_foreach(js, iterable, js_iter_cb, &ctx);
  return !is_err(result);
}

char *js_getstr(struct js *js, jsval_t value, size_t *len) {
  if (vtype(value) != T_STR) return NULL;
  jsoff_t n, off = vstr(js, value, &n);
  if (len != NULL) *len = n;
  return (char *) &js->mem[off];
}

void js_merge_obj(struct js *js, jsval_t dst, jsval_t src) {
  if (vtype(dst) != T_OBJ || vtype(src) != T_OBJ) return;
  jsoff_t next = loadoff(js, (jsoff_t) vdata(src)) & ~(3U | FLAGMASK);
  while (next < js->brk && next != 0) {
    jsoff_t header = loadoff(js, next);
    if (is_slot_prop(header)) { next = next_prop(header); continue; }
    
    jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
    jsval_t val = loadval(js, next + (jsoff_t) (sizeof(next) + sizeof(koff)));
    
    setprop(js, dst, mkval(T_STR, koff), val);
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

static void gc_roots_common(gc_off_op_t op_off, gc_val_op_t op_val, gc_cb_ctx_t *c) {
  UTARRAY_EACH(global_scope_stack, jsoff_t, off) op_off(c, off);
  UTARRAY_EACH(saved_scope_stack, jsval_t, val) op_val(c, val);

  for (int i = 0; i < global_this_stack.depth; i++)
    op_val(c, &global_this_stack.stack[i]);

  UTARRAY_EACH(propref_stack, propref_data_t, pref) {
    op_off(c, &pref->obj_off);
    op_off(c, &pref->key_off);
  }

  UTARRAY_EACH(prim_propref_stack, prim_propref_data_t, ppref) {
    op_val(c, &ppref->prim_val);
    op_off(c, &ppref->key_off);
  }

  if (rt && rt->js == c->js)
    op_val(c, &rt->ant_obj);

  for (coroutine_t *coro = pending_coroutines.head; coro; coro = coro->next) {
    op_val(c, &coro->scope);
    op_val(c, &coro->this_val);
    op_val(c, &coro->super_val);
    op_val(c, &coro->new_target);
    op_val(c, &coro->awaited_promise);
    op_val(c, &coro->result);
    op_val(c, &coro->async_func);
    op_val(c, &coro->yield_value);

    for (int i = 0; i < coro->for_let_stack_len; i++) {
      op_val(c, &coro->for_let_stack[i].body_scope);
      op_off(c, &coro->for_let_stack[i].prop_off);
    }

    if (coro->scope_stack) {
      UTARRAY_EACH(coro->scope_stack, jsoff_t, off) op_off(c, off);
    }

    if (coro->mco) {
      async_exec_context_t *actx = (async_exec_context_t *)mco_get_user_data(coro->mco);
      if (actx) {
        op_val(c, &actx->closure_scope);
        op_val(c, &actx->result);
        op_val(c, &actx->promise);
      }
    }
  }

  esm_module_t *mod, *mod_tmp;
  HASH_ITER(hh, global_module_cache.modules, mod, mod_tmp) {
    op_val(c, &mod->namespace_obj);
    op_val(c, &mod->default_export);
  }

  timer_gc_update_roots(op_val, c);
  ffi_gc_update_roots(op_val, c);
  fetch_gc_update_roots(op_val, c);
  fs_gc_update_roots(op_val, c);
  child_process_gc_update_roots(op_val, c);
  readline_gc_update_roots(op_val, c);
  process_gc_update_roots(op_val, c);

  map_registry_entry_t *map_reg, *map_reg_tmp;
  HASH_ITER(hh, map_registry, map_reg, map_reg_tmp) {
    if (map_reg->head && *map_reg->head) {
      map_entry_t *me, *me_tmp;
      HASH_ITER(hh, *map_reg->head, me, me_tmp) op_val(c, &me->value);
    }
  }

  set_registry_entry_t *set_reg, *set_reg_tmp;
  HASH_ITER(hh, set_registry, set_reg, set_reg_tmp) {
    if (set_reg->head && *set_reg->head) {
      set_entry_t *se, *se_tmp;
      HASH_ITER(hh, *set_reg->head, se, se_tmp) op_val(c, &se->value);
    }
  }

  for (int i = 0; i < c->js->for_let_stack_len; i++) {
    op_val(c, &c->js->for_let_stack[i].body_scope);
    op_off(c, &c->js->for_let_stack[i].prop_off);
  }
  
  for (jshdl_t i = 0; i < c->js->gc_roots_len; i++) op_val(c, &c->js->gc_roots[i]);
  if (c->js->ascii_cache_init) for (int i = 0; i < 128; i++) op_val(c, &c->js->ascii_char_cache[i]);
}

void js_gc_reserve_roots(GC_UPDATE_ARGS) {
  #define RSV_OFF(x) ((x) ? (void)fwd_off(ctx, x) : (void)0)
  #define RSV_VAL(x) (void)fwd_val(ctx, x)
  
  gc_cb_ctx_t cb_ctx = { fwd_off, fwd_val, ctx, js };
  gc_roots_common(gc_reserve_off_cb, gc_reserve_val_cb, &cb_ctx);
  
  promise_data_entry_t *pd, *pd_tmp;
  HASH_ITER(hh, promise_registry, pd, pd_tmp) {
    (void)fwd_off(ctx, pd->obj_offset);
    RSV_VAL(pd->value);
    UTARRAY_EACH(pd->handlers, promise_handler_t, h) {
      RSV_VAL(h->onFulfilled); RSV_VAL(h->onRejected); RSV_VAL(h->nextPromise);
    }
  }

  // accessor registry is a weak reference
  // objects only survive if reachable from other roots
  (void)accessor_registry;

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
  
  #undef RSV_OFF
  #undef RSV_VAL
}

void js_gc_update_roots(GC_UPDATE_ARGS) {
  #define FWD_OFF(x) ((x) ? ((x) = fwd_off(ctx, x)) : 0)
  #define FWD_VAL(x) ((x) = fwd_val(ctx, x))
  
  gc_cb_ctx_t cb_ctx = { fwd_off, fwd_val, ctx, js };
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
      HASH_FIND(hh_unhandled, unhandled_rejections, &pd->promise_id, sizeof(uint32_t), in_unhandled);
      if (in_unhandled) HASH_DELETE(hh_unhandled, unhandled_rejections, pd);
      
      jsoff_t new_off = fwd_off(ctx, pd->obj_offset);
      if (new_off == pd->obj_offset && pd->obj_offset != 0) {
        utarray_free(pd->handlers); free(pd); continue;
      }
      
      pd->obj_offset = new_off;
      FWD_VAL(pd->value);
      UTARRAY_EACH(pd->handlers, promise_handler_t, h) {
        FWD_VAL(h->onFulfilled); FWD_VAL(h->onRejected); FWD_VAL(h->nextPromise);
      }
      
      HASH_ADD(hh, new_promise_registry, promise_id, sizeof(uint32_t), pd);
      if (in_unhandled) HASH_ADD(hh_unhandled, new_unhandled, promise_id, sizeof(uint32_t), pd);
    }

  proxy_data_t *proxy, *proxy_tmp;
  for (proxy_data_t *new_proxy_registry = NULL, *_once = NULL; !_once; _once = (void*)1, proxy_registry = new_proxy_registry)
    HASH_ITER(hh, proxy_registry, proxy, proxy_tmp) {
      HASH_DEL(proxy_registry, proxy);
      jsoff_t old_off = proxy->obj_offset;
      jsoff_t new_off = fwd_off(ctx, old_off);
      if (new_off == old_off && old_off != 0) { free(proxy); continue; }
      proxy->obj_offset = new_off;
      FWD_VAL(proxy->target); FWD_VAL(proxy->handler);
      HASH_ADD(hh, new_proxy_registry, obj_offset, sizeof(jsoff_t), proxy);
    }

  dynamic_accessors_t *acc, *acc_tmp;
  for (dynamic_accessors_t *new_acc_registry = NULL, *_once = NULL; !_once; _once = (void*)1, accessor_registry = new_acc_registry)
    HASH_ITER(hh, accessor_registry, acc, acc_tmp) {
      HASH_DEL(accessor_registry, acc);
      jsoff_t old_off = acc->obj_offset;
      jsoff_t new_off = fwd_off(ctx, old_off);
      if (new_off == old_off && old_off != 0) { free(acc); continue; }
      acc->obj_offset = new_off;
      HASH_ADD(hh, new_acc_registry, obj_offset, sizeof(jsoff_t), acc);
    }

  descriptor_entry_t *desc, *desc_tmp;
  for (descriptor_entry_t *new_desc_registry = NULL, *_once = NULL; !_once; _once = (void*)1, desc_registry = new_desc_registry)
    HASH_ITER(hh, desc_registry, desc, desc_tmp) {
      HASH_DEL(desc_registry, desc);
      jsoff_t old_off = (jsoff_t)(desc->key >> 32);
      jsoff_t new_off = fwd_off(ctx, old_off);
      if (new_off == old_off && old_off != 0) { free(desc); continue; }
      if (desc->has_getter) FWD_VAL(desc->getter);
      if (desc->has_setter) FWD_VAL(desc->setter);
      desc->key = ((uint64_t)new_off << 32) | (uint32_t)(desc->key & 0xFFFFFFFF);
      desc->obj_off = new_off;
      HASH_ADD(hh, new_desc_registry, key, sizeof(uint64_t), desc);
    }

  memset(intern_prop_cache, 0, sizeof(intern_prop_cache));
  
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

static jsval_t js_eval_inherit_strict(struct js *js, const char *buf, size_t len, bool inherit_strict) {
  jsval_t res = js_mkundef();
  if (len == (size_t) ~0U) len = strlen(buf);
  js->eval_depth++;
  js->consumed = 1;
  js->tok = TOK_ERR;
  js->code = buf;
  js->clen = (jsoff_t) len;
  js->pos = 0;
  
  uint8_t saved_tok = js->tok;
  jsoff_t saved_pos = js->pos;
  uint8_t saved_consumed = js->consumed;
  js->consumed = 1;
  
  bool is_strict = inherit_strict;
  if (inherit_strict) js->flags |= F_STRICT;
  
  if (next(js) == TOK_STRING) {
    const char *str = &js->code[js->toff + 1];
    size_t str_len = js->tlen - 2;
    if (str_len == 10 && memcmp(str, "use strict", 10) == 0) {
      js->flags |= F_STRICT;
      is_strict = true;
    }
  }
  
  js->tok = saved_tok;
  js->pos = saved_pos;
  js->consumed = saved_consumed;
  
  if (is_strict) {
    mkscope(js);
    set_slot(js, js->scope, SLOT_STRICT_EVAL_SCOPE, tov(1));
  }
  
  hoist_function_declarations(js);
  if (!(js->flags & F_CALL)) hoist_var_declarations(js, js->scope);

  while (next(js) != TOK_EOF && !is_err(res)) {
    res = js_stmt(js);
    if (js->needs_gc && js->eval_depth == 1 && !js->gc_suppress) {
      js->needs_gc = false;
      js_gc_compact(js);
    }
    if (js->flags & F_RETURN) break;
  }
  
  if (is_strict) delscope(js);
  js->eval_depth--;
  return res;
}

inline jsval_t js_eval(struct js *js, const char *buf, size_t len) {
  return js_eval_inherit_strict(js, buf, len, false);
}

static jsval_t js_call_internal(struct js *js, jsval_t func, jsval_t bound_this, jsval_t *args, int nargs, bool use_bound_this) {
  bool saved_gc_suppress = js->gc_suppress;
  js->gc_suppress = true;

  if (vtype(func) == T_FFI) {
    jsval_t res = ffi_call_by_index(js, (unsigned int)vdata(func), args, nargs);
    js->gc_suppress = saved_gc_suppress;
    return res;
  } else if (vtype(func) == T_CFUNC) {
    jsval_t saved_this = js->this_val;
    if (use_bound_this) js->this_val = bound_this;
    jsval_t (*fn)(struct js *, jsval_t *, int) = (jsval_t(*)(struct js *, jsval_t *, int)) vdata(func);
    jsval_t res = fn(js, args, nargs);
    js->this_val = saved_this;
    js->gc_suppress = saved_gc_suppress;
    return res;
  } else if (vtype(func) == T_FUNC) {
    jsval_t func_obj = mkval(T_OBJ, vdata(func));
    jsval_t cfunc_slot = get_slot(js, func_obj, SLOT_CFUNC);
    
    if (vtype(cfunc_slot) == T_CFUNC) {
      jsval_t slot_bound_this = get_slot(js, func_obj, SLOT_BOUND_THIS);
      bool has_slot_bound_this = vtype(slot_bound_this) != T_UNDEF;
      
      int final_nargs;
      jsval_t *combined_args = resolve_bound_args(js, func_obj, args, nargs, &final_nargs);
      jsval_t *final_args = combined_args ? combined_args : args;
      
      jsval_t saved_func = js->current_func;
      jsval_t saved_this = js->this_val;
      js->current_func = func;
      
      if (has_slot_bound_this) {
        js->this_val = slot_bound_this;
      } else if (use_bound_this) js->this_val = bound_this;
      
      jsval_t (*fn)(struct js *, jsval_t *, int) = (jsval_t(*)(struct js *, jsval_t *, int)) vdata(cfunc_slot);
      jsval_t res = fn(js, final_args, final_nargs);
      js->current_func = saved_func;
      js->this_val = saved_this;
      
      if (combined_args) free(combined_args);
      js->gc_suppress = saved_gc_suppress;
      return res;
    }
    
    jsoff_t fnlen;
    const char *fn = get_func_code(js, func_obj, &fnlen);
    if (!fn) return js_mkerr(js, "function has no code");
    
    jsval_t slot_bound_this = get_slot(js, func_obj, SLOT_BOUND_THIS);
    bool has_slot_bound_this = vtype(slot_bound_this) != T_UNDEF;
    
    int final_nargs;
    jsval_t *combined_args = resolve_bound_args(js, func_obj, args, nargs, &final_nargs);
    jsval_t *final_args = combined_args ? combined_args : args;
    
    jsval_t async_slot = get_slot(js, func_obj, SLOT_ASYNC);
    bool is_async = vtype(async_slot) == T_BOOL && vdata(async_slot) == 1;
    
    if (is_async) {
      jsval_t closure_scope = get_slot(js, func_obj, SLOT_SCOPE);
      jsval_t res = start_async_in_coroutine(js, fn, fnlen, closure_scope, final_args, final_nargs);
      if (combined_args) free(combined_args);
      js->gc_suppress = saved_gc_suppress;
      return res;
    }
    
    jsval_t saved_scope = js->scope;
    jsval_t closure_scope = get_slot(js, func_obj, SLOT_SCOPE);
    if (vtype(closure_scope) == T_OBJ) js->scope = closure_scope;
    
    uint8_t saved_flags = js->flags;
    js->flags = 0;
    mkscope(js);
    js->flags = saved_flags;
    
    parsed_func_t *pf = get_or_parse_func(fn, fnlen);
    if (!pf) {
      delscope(js);
      js->scope = saved_scope;
      if (combined_args) free(combined_args);
      js->gc_suppress = saved_gc_suppress;
      return js_mkerr(js, "failed to parse function");
    }
    
    int arg_idx = 0;
    for (unsigned int i = 0; i < (unsigned int)pf->param_count; i++) {
      parsed_param_t *pp = (parsed_param_t *)utarray_eltptr(pf->params, i);
      
      if (pp->is_destruct) {
        jsval_t arg_val = (arg_idx < final_nargs) ? final_args[arg_idx++] : js_mkundef();
        if (vtype(arg_val) == T_UNDEF && pp->default_len > 0) {
          arg_val = js_eval_str(js, &fn[pp->default_start], pp->default_len);
        }
        bind_destruct_pattern(js, &fn[pp->pattern_off], pp->pattern_len, arg_val, js->scope);
      } else {
        jsval_t v = arg_idx < final_nargs ? final_args[arg_idx++] : js_mkundef();
        if (vtype(v) == T_UNDEF && pp->default_len > 0) {
          v = js_eval_str(js, &fn[pp->default_start], pp->default_len);
        }
        setprop(js, js->scope, js_mkstr(js, &fn[pp->name_off], pp->name_len), v);
      }
    }
    
    if (pf->has_rest && pf->rest_param_len > 0) {
      jsval_t rest_array = mkarr(js);
      if (!is_err(rest_array)) {
        jsoff_t idx = 0;
        while (arg_idx < final_nargs) {
          char idxstr[16];
          size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
          jsval_t key = js_mkstr(js, idxstr, idxlen);
          setprop(js, rest_array, key, final_args[arg_idx]);
          idx++;
          arg_idx++;
        }
        jsval_t len_key = js_mkstr(js, "length", 6);
        setprop(js, rest_array, len_key, tov((double) idx));
        rest_array = mkval(T_ARR, vdata(rest_array));
        setprop(js, js->scope, js_mkstr(js, &fn[pf->rest_param_start], pf->rest_param_len), rest_array);
      }
    }
    
    if (code_uses_arguments(&fn[pf->body_start], pf->body_len)) {
      setup_arguments(js, js->scope, final_args, final_nargs, pf->is_strict);
    }
    
    jsval_t saved_this = js->this_val;
    if (has_slot_bound_this) {
      js->this_val = slot_bound_this;
    } else if (use_bound_this) {
      js->this_val = bound_this;
    } else js->this_val = js_glob(js);
    
    js_parse_state_t saved_state;
    JS_SAVE_STATE(js, saved_state);
    uint8_t caller_flags = js->flags;
    
    js->flags = F_CALL | (pf->is_strict ? F_STRICT : 0);
    jsval_t res = js_eval(js, &fn[pf->body_start], pf->body_len);
    if (!is_err(res) && !(js->flags & F_RETURN)) res = js_mkundef();
    
    JS_RESTORE_STATE(js, saved_state);
    js->flags = caller_flags;
    
    js->this_val = saved_this;
    delscope(js);
    js->scope = saved_scope;
    if (combined_args) free(combined_args);
    
    js->gc_suppress = saved_gc_suppress;
    return res;
  }
  
  js->gc_suppress = saved_gc_suppress;
  return js_mkerr(js, "not a function");
}

jsval_t js_call(struct js *js, jsval_t func, jsval_t *args, int nargs) {
  return js_call_internal(js, func, js_mkundef(), args, nargs, false);
}

jsval_t js_call_with_this(struct js *js, jsval_t func, jsval_t bound_this, jsval_t *args, int nargs) {
  return js_call_internal(js, func, bound_this, args, nargs, true);
}

ant_iter_t js_prop_iter_begin(struct js *js, jsval_t obj) {
  ant_iter_t iter = {.ctx = js, .off = 0};
  
  uint8_t t = vtype(obj);
  if (t == T_OBJ || t == T_ARR || t == T_FUNC) {
    iter.off = (jsoff_t)vdata(obj);
  }
  
  return iter;
}

bool js_prop_iter_next(ant_iter_t *iter, const char **key, size_t *key_len, jsval_t *value) {
  if (!iter || !iter->ctx) return false;
  
  ant_t *js = (ant_t *)iter->ctx;
  jsoff_t next = loadoff(js, iter->off) & ~(3U | FLAGMASK);
  
  while (next < js->brk && next != 0) {
    jsoff_t header = loadoff(js, next);
    if (!is_slot_prop(header)) break;
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

jsval_t js_mkpromise(struct js *js) { return mkpromise(js); }
void js_resolve_promise(struct js *js, jsval_t promise, jsval_t value) { resolve_promise(js, promise, value); }
void js_reject_promise(struct js *js, jsval_t promise, jsval_t value) { reject_promise(js, promise, value); }

void js_check_unhandled_rejections(struct js *js) {
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
      fprintf(stderr, "%s\n", js->errmsg ? js->errmsg : js_str(js, pd->value));
      js_destroy(js); exit(1);
    }
    
    char buf[1024];
    buf[tostr(js, pd->value, buf, sizeof(buf) - 1)] = '\0';
    fprintf(stderr, "Uncaught (in promise) %s\n", buf);
    
    pd->has_rejection_handler = true;
    HASH_DELETE(hh_unhandled, unhandled_rejections, pd);
  }
}

bool js_is_slot_prop(jsoff_t header) { return is_slot_prop(header); }
jsoff_t js_next_prop(jsoff_t header) { return next_prop(header); }
jsoff_t js_loadoff(struct js *js, jsoff_t off) { return loadoff(js, off); }

void js_set_getter(struct js *js, jsval_t obj, js_getter_fn getter) {
  if (vtype(obj) != T_OBJ) return;
  jsoff_t obj_off = (jsoff_t)vdata(obj);
  dynamic_accessors_t *entry = NULL;
  HASH_FIND(hh, accessor_registry, &obj_off, sizeof(jsoff_t), entry);
  if (!entry) {
    entry = (dynamic_accessors_t *)malloc(sizeof(dynamic_accessors_t));
    if (!entry) return;
    entry->obj_offset = obj_off;
    entry->getter = NULL;
    entry->setter = NULL;
    HASH_ADD(hh, accessor_registry, obj_offset, sizeof(jsoff_t), entry);
  }
  entry->getter = getter;
}

void js_set_setter(struct js *js, jsval_t obj, js_setter_fn setter) {
  if (vtype(obj) != T_OBJ) return;
  jsoff_t obj_off = (jsoff_t)vdata(obj);
  dynamic_accessors_t *entry = NULL;
  HASH_FIND(hh, accessor_registry, &obj_off, sizeof(jsoff_t), entry);
  if (!entry) {
    entry = (dynamic_accessors_t *)malloc(sizeof(dynamic_accessors_t));
    if (!entry) return;
    entry->obj_offset = obj_off;
    entry->getter = NULL;
    entry->setter = NULL;
    HASH_ADD(hh, accessor_registry, obj_offset, sizeof(jsoff_t), entry);
  }
  entry->setter = setter;
}

static inline uint64_t make_desc_key(jsoff_t obj_off, const char *key, size_t klen) {
  uint32_t key_hash = (uint32_t)hash_key(key, klen);
  return ((uint64_t)obj_off << 32) | key_hash;
}

static descriptor_entry_t *lookup_descriptor(jsoff_t obj_off, const char *key, size_t klen) {
  uint64_t desc_key = make_desc_key(obj_off, key, klen);
  descriptor_entry_t *entry = NULL;
  HASH_FIND(hh, desc_registry, &desc_key, sizeof(uint64_t), entry);
  return entry;
}

static descriptor_entry_t *get_or_create_desc(struct js *js, jsval_t obj, const char *key, size_t klen) {
  if (vtype(obj) != T_OBJ && vtype(obj) != T_FUNC && vtype(obj) != T_ARR) return NULL;
  jsoff_t obj_off = (jsoff_t)vdata(obj);
  uint64_t desc_key = make_desc_key(obj_off, key, klen);
  
  descriptor_entry_t *entry = NULL;
  HASH_FIND(hh, desc_registry, &desc_key, sizeof(uint64_t), entry);
  if (!entry) {
    entry = (descriptor_entry_t *)ant_calloc(sizeof(descriptor_entry_t) + klen + 1);
    if (!entry) return NULL;
    
    entry->key = desc_key;
    entry->obj_off = obj_off;
    entry->prop_name = (char *)(entry + 1);
    memcpy(entry->prop_name, key, klen);
    entry->prop_name[klen] = '\0';
    entry->prop_len = klen;
    entry->writable = true;
    entry->enumerable = true;
    entry->configurable = true;
    entry->has_getter = false;
    entry->has_setter = false;
    entry->getter = js_mkundef();
    entry->setter = js_mkundef();
    
    HASH_ADD(hh, desc_registry, key, sizeof(uint64_t), entry);
  }
  return entry;
}

void js_set_descriptor(struct js *js, jsval_t obj, const char *key, size_t klen, int flags) {
  descriptor_entry_t *entry = get_or_create_desc(js, obj, key, klen);
  if (!entry) return;
  entry->writable = (flags & JS_DESC_W) != 0;
  entry->enumerable = (flags & JS_DESC_E) != 0;
  entry->configurable = (flags & JS_DESC_C) != 0;
}

void js_set_getter_desc(struct js *js, jsval_t obj, const char *key, size_t klen, jsval_t getter, int flags) {
  descriptor_entry_t *entry = get_or_create_desc(js, obj, key, klen);
  if (!entry) return;
  entry->enumerable = (flags & JS_DESC_E) != 0;
  entry->configurable = (flags & JS_DESC_C) != 0;
  entry->has_getter = true;
  entry->getter = getter;
}

void js_set_setter_desc(struct js *js, jsval_t obj, const char *key, size_t klen, jsval_t setter, int flags) {
  descriptor_entry_t *entry = get_or_create_desc(js, obj, key, klen);
  if (!entry) return;
  entry->enumerable = (flags & JS_DESC_E) != 0;
  entry->configurable = (flags & JS_DESC_C) != 0;
  entry->has_setter = true;
  entry->setter = setter;
}

void js_set_accessor_desc(struct js *js, jsval_t obj, const char *key, size_t klen, jsval_t getter, jsval_t setter, int flags) {
  descriptor_entry_t *entry = get_or_create_desc(js, obj, key, klen);
  if (!entry) return;
  entry->enumerable = (flags & JS_DESC_E) != 0;
  entry->configurable = (flags & JS_DESC_C) != 0;
  entry->has_getter = true;
  entry->has_setter = true;
  entry->getter = getter;
  entry->setter = setter;
}
