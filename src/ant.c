#if defined(__GNUC__) && !defined(__clang__)
  #pragma GCC optimize("O3,inline")
#endif

#include <assert.h>
#include <math.h>
#include <regex.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>
#include <sys/stat.h>
#include <utarray.h>
#include <uthash.h>

#include "ant.h"
#include "config.h"
#include "arena.h"

#include "modules/fs.h"
#include "modules/timer.h"
#include "modules/fetch.h"

#define MINICORO_IMPL
#include "minicoro.h"

typedef uint32_t jsoff_t;

typedef struct {
  jsval_t *stack;
  int depth;
  int capacity;
} this_stack_t;

typedef struct call_frame {
  const char *filename;
  const char *function_name;
  int line;
  int col;
} call_frame_t;

typedef struct {
  call_frame_t *frames;
  int depth;
  int capacity;
} call_stack_t;

typedef enum {
  CORO_ASYNC_AWAIT,
  CORO_GENERATOR,
  CORO_ASYNC_GENERATOR
} coroutine_type_t;

typedef struct coroutine {
  struct js *js;
  coroutine_type_t type;
  jsval_t scope;
  jsval_t this_val;
  jsval_t awaited_promise;
  jsval_t result;
  jsval_t async_func;
  jsval_t *args;
  int nargs;
  bool is_settled;
  bool is_error;
  bool is_done;
  jsoff_t resume_point;
  jsval_t yield_value;
  struct coroutine *next;
  mco_coro* mco;
  bool mco_started;
  bool is_ready;
} coroutine_t;

typedef struct {
  coroutine_t *head;
  coroutine_t *tail;
} coroutine_queue_t;

typedef struct {
  struct js *js;
  const char *code;
  size_t code_len;
  jsval_t closure_scope;
  jsval_t result;
  jsval_t promise;
  bool has_error;
  coroutine_t *coro;
} async_exec_context_t;

typedef struct {
  jsoff_t offset;
  jsoff_t size;
  uint8_t type;
  char detail[128];
} FreeListEntry;

static UT_array *global_free_list = NULL;
static UT_array *global_scope_stack = NULL;
static jsoff_t protected_brk = 0;

static const UT_icd jsoff_icd = {
  .sz = sizeof(jsoff_t),
  .init = NULL,
  .copy = NULL,
  .dtor = NULL,
};

static const UT_icd free_list_icd = {
  .sz = sizeof(FreeListEntry),
  .init = NULL,
  .copy = NULL,
  .dtor = NULL,
};

static this_stack_t global_this_stack = {NULL, 0, 0};
static call_stack_t global_call_stack = {NULL, 0, 0};
static coroutine_queue_t pending_coroutines = {NULL, NULL};

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

typedef struct {
  char key[64];
  jsoff_t offset;
  uint32_t hash;
  UT_hash_handle hh;
} prop_cache_entry_t;

typedef struct {
  jsoff_t obj_offset;
  prop_cache_entry_t *cache;
  uint32_t hit_count;
  UT_hash_handle hh;
} obj_prop_cache_t;

static ant_library_t *library_registry = NULL;
static esm_module_cache_t global_module_cache = {NULL, 0};
static obj_prop_cache_t *global_property_cache = NULL;

void js_protect_init_memory(struct js *js) {
  protected_brk = js_getbrk(js);
  if (protected_brk < 0x2000) protected_brk = 0x2000;
}

void ant_register_library(ant_library_init_fn init_fn, const char *name, ...) {
  va_list args;
  const char *alias = name;
  
  va_start(args, name);
  while (alias != NULL) {
    ant_library_t *lib = (ant_library_t *)ANT_GC_MALLOC(sizeof(ant_library_t));
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

struct js {
  jsoff_t css;            // max observed C stack size
  jsoff_t lwm;            // JS ram low watermark: min free ram observed
  const char *code;       // currently parsed code snippet
  char errmsg[256];       // error message placeholder (increased from 33)
  const char *filename;   // current filename for error reporting
  uint8_t tok;            // last parsed token value
  uint8_t consumed;       // indicator that last parsed token was consumed
  uint8_t flags;          // execution flags, see F_* constants below
  #define F_NOEXEC 1U     // parse code, but not execute
  #define F_LOOP 2U       // we are inside the loop
  #define F_CALL 4U       // we are inside a function call
  #define F_BREAK 8U      // exit the loop
  #define F_RETURN 16U    // return has been executed
  #define F_THROW 32U     // throw has been executed
  #define F_SWITCH 64U    // we are inside a switch statement
  #define F_STRICT 128U   // strict mode is enabled
  jsoff_t clen;           // code snippet length
  jsoff_t pos;            // current parsing position
  jsoff_t toff;           // offset of the last parsed token
  jsoff_t tlen;           // length of the last parsed token
  jsoff_t nogc;           // entity offset to exclude from GC
  jsval_t tval;           // holds last parsed numeric or string literal value
  jsval_t scope;          // current scope
  jsval_t this_val;       // 'this' value for currently executing function
  uint8_t *mem;           // available JS memory
  jsoff_t size;           // memory size
  jsoff_t brk;            // current mem usage boundary
  jsoff_t gct;            // GC threshold. if brk > gct, trigger GC
  jsoff_t maxcss;         // maximum allowed C stack size usage
  void *cstk;             // C stack pointer at the beginning of js_eval()
  jsval_t current_func;   // currently executing function (for native closures)
  bool var_warning_shown; // flag to show var deprecation warning only once
  bool owns_mem;          // true if js owns the memory buffer (dynamic allocation)
  jsoff_t max_size;       // maximum allowed memory size (for dynamic growth)
};

enum {
  TOK_ERR, TOK_EOF, TOK_IDENTIFIER, TOK_NUMBER, TOK_STRING, TOK_SEMICOLON, TOK_BIGINT,
  TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE, TOK_LBRACKET, TOK_RBRACKET,
  TOK_ASYNC = 50, TOK_AWAIT, TOK_BREAK, TOK_CASE, TOK_CATCH, TOK_CLASS, TOK_CONST, TOK_CONTINUE,
  TOK_DEFAULT, TOK_DELETE, TOK_DO, TOK_ELSE, TOK_EXPORT, TOK_FINALLY, TOK_FOR, TOK_FROM, TOK_FUNC,
  TOK_IF, TOK_IMPORT, TOK_IN, TOK_INSTANCEOF, TOK_LET, TOK_NEW, TOK_OF, TOK_RETURN, TOK_SWITCH,
  TOK_THIS, TOK_THROW, TOK_TRY, TOK_VAR, TOK_VOID, TOK_WHILE, TOK_WITH,
  TOK_YIELD, TOK_UNDEF, TOK_NULL, TOK_TRUE, TOK_FALSE, TOK_AS, TOK_STATIC,
  TOK_DOT = 100, TOK_CALL, TOK_BRACKET, TOK_POSTINC, TOK_POSTDEC, TOK_NOT, TOK_TILDA,
  TOK_TYPEOF, TOK_UPLUS, TOK_UMINUS, TOK_EXP, TOK_MUL, TOK_DIV, TOK_REM,
  TOK_OPTIONAL_CHAIN, TOK_REST,
  TOK_PLUS, TOK_MINUS, TOK_SHL, TOK_SHR, TOK_ZSHR, TOK_LT, TOK_LE, TOK_GT,
  TOK_GE, TOK_EQ, TOK_NE, TOK_AND, TOK_XOR, TOK_OR, TOK_LAND, TOK_LOR, TOK_NULLISH,
  TOK_COLON, TOK_Q,  TOK_ASSIGN, TOK_PLUS_ASSIGN, TOK_MINUS_ASSIGN,
  TOK_MUL_ASSIGN, TOK_DIV_ASSIGN, TOK_REM_ASSIGN, TOK_SHL_ASSIGN,
  TOK_SHR_ASSIGN, TOK_ZSHR_ASSIGN, TOK_AND_ASSIGN, TOK_XOR_ASSIGN,
  TOK_OR_ASSIGN, TOK_COMMA, TOK_TEMPLATE, TOK_ARROW,
};

enum {
  T_OBJ, T_PROP, T_STR, T_UNDEF, T_NULL, T_NUM,
  T_BOOL, T_FUNC, T_CODEREF, T_CFUNC, T_ERR, T_ARR,
  T_PROMISE, T_GENERATOR, T_BIGINT, T_PROPREF
};

static const char *typestr_raw(uint8_t t) {
  const char *names[] = { 
    "object", "prop", "string", "undefined", "null", "number",
    "boolean", "function", "coderef", "cfunc", "err", "array", 
    "promise", "generator", "bigint", "propref"
  };
  
  return (t < sizeof(names) / sizeof(names[0])) ? names[t] : "??";
}

static jsval_t tov(double d) { union { double d; jsval_t v; } u = {d}; return u.v; }
static double tod(jsval_t v) { union { jsval_t v; double d; } u = {v}; return u.d; }
static bool is_nan(jsval_t v) { return (v >> 52U) == 0x7feU; }
static size_t vdata(jsval_t v) { return (size_t) (v & ~((jsval_t) 0x7fffUL << 48U)); }
static jsoff_t coderefoff(jsval_t v) { return v & 0xffffffU; }
static jsoff_t codereflen(jsval_t v) { return (v >> 24U) & 0xffffffU; }
static jsoff_t propref_obj(jsval_t v) { return v & 0xffffffU; }
static jsoff_t propref_key(jsval_t v) { return (v >> 24U) & 0xffffffU; }

static const char *typestr(uint8_t t) {
  return t == T_CFUNC ? "function" : typestr_raw(t);
}

uint8_t vtype(jsval_t v) { 
  return is_nan(v) ? ((v >> 48U) & 15U) : (uint8_t) T_NUM; 
}

static jsval_t mkval(uint8_t type, uint64_t data) { 
  return ((jsval_t) 0x7fe0U << 48U) | ((jsval_t) (type) << 48) | (data & 0xffffffffffffUL); 
}

static jsval_t mkcoderef(jsval_t off, jsoff_t len) { 
  return mkval(T_CODEREF, (off & 0xffffffU) | ((jsval_t)(len & 0xffffffU) << 24U));
}

static jsval_t mkpropref(jsoff_t obj_off, jsoff_t key_off) { 
  return mkval(T_PROPREF, (obj_off & 0xffffffU) | ((jsval_t)(key_off & 0xffffffU) << 24U)); 
}

static uint8_t unhex(uint8_t c) { return (c >= '0' && c <= '9') ? (uint8_t) (c - '0') : (c >= 'a' && c <= 'f') ? (uint8_t) (c - 'W') : (c >= 'A' && c <= 'F') ? (uint8_t) (c - '7') : 0; }
static bool is_space(int c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '\f' || c == '\v'; }
static bool is_digit(int c) { return c >= '0' && c <= '9'; }
static bool is_xdigit(int c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static bool is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool is_ident_begin(int c) { return c == '_' || c == '$' || is_alpha(c); }
static bool is_ident_continue(int c) { return c == '_' || c == '$' || is_alpha(c) || is_digit(c); }
static bool is_err(jsval_t v) { return vtype(v) == T_ERR; }
static bool is_unary(uint8_t tok) { return (tok >= TOK_POSTINC && tok <= TOK_UMINUS) || tok == TOK_NOT || tok == TOK_TILDA || tok == TOK_TYPEOF || tok == TOK_VOID; }
static bool is_assign(uint8_t tok) { return (tok >= TOK_ASSIGN && tok <= TOK_OR_ASSIGN); }
static void saveoff(struct js *js, jsoff_t off, jsoff_t val) { memcpy(&js->mem[off], &val, sizeof(val)); }
static void saveval(struct js *js, jsoff_t off, jsval_t val) { memcpy(&js->mem[off], &val, sizeof(val)); }
static jsoff_t loadoff(struct js *js, jsoff_t off) { jsoff_t v = 0; assert(js->brk <= js->size); memcpy(&v, &js->mem[off], sizeof(v)); return v; }
static jsoff_t offtolen(jsoff_t off) { return (off >> 2) - 1; }
static jsoff_t vstrlen(struct js *js, jsval_t v) { return offtolen(loadoff(js, (jsoff_t) vdata(v))); }
static jsval_t loadval(struct js *js, jsoff_t off) { jsval_t v = 0; memcpy(&v, &js->mem[off], sizeof(v)); return v; }
static jsval_t upper(struct js *js, jsval_t scope) { return mkval(T_OBJ, loadoff(js, (jsoff_t) (vdata(scope) + sizeof(jsoff_t)))); }
static jsoff_t align32(jsoff_t v) { return ((v + 3) >> 2) << 2; }

#define CHECKV(_v) do { if (is_err(_v)) { res = (_v); goto done; } } while (0)
#define EXPECT(_tok, _e) do { if (next(js) != _tok) { _e; return js_mkerr(js, "parse error"); }; js->consumed = 1; } while (0)

static bool streq(const char *buf, size_t len, const char *p, size_t n);
static size_t tostr(struct js *js, jsval_t value, char *buf, size_t len);
static size_t strpromise(struct js *js, jsval_t value, char *buf, size_t len);

static inline jsoff_t esize(jsoff_t w);
static jsval_t js_expr(struct js *js);
static jsval_t js_stmt(struct js *js);
static jsval_t js_assignment(struct js *js);
static jsval_t js_arrow_func(struct js *js, jsoff_t params_start, jsoff_t params_end, bool is_async);
static jsval_t js_while(struct js *js);
static jsval_t js_do_while(struct js *js);
static jsval_t js_block_or_stmt(struct js *js);
static jsval_t js_var_decl(struct js *js);
static bool parse_func_params(struct js *js, uint8_t *flags);
static jsval_t js_regex_literal(struct js *js);
static jsval_t js_try(struct js *js);
static jsval_t js_switch(struct js *js);
static jsval_t do_op(struct js *, uint8_t op, jsval_t l, jsval_t r);
static jsval_t do_instanceof(struct js *js, jsval_t l, jsval_t r);
static jsval_t do_in(struct js *js, jsval_t l, jsval_t r);
static jsval_t resolveprop(struct js *js, jsval_t v);
static jsoff_t free_list_allocate(size_t size);
static jsoff_t lkp(struct js *js, jsval_t obj, const char *buf, size_t len);
static jsval_t builtin_array_push(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_array_pop(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_array_slice(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_array_join(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_array_includes(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_array_every(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Error(struct js *js, jsval_t *args, int nargs);
static jsval_t js_import_stmt(struct js *js);
static jsval_t js_export_stmt(struct js *js);
static jsval_t builtin_import(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_import_meta_resolve(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_indexOf(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_substring(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_split(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_slice(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_includes(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_startsWith(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_endsWith(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_replace(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_template(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_charCodeAt(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_toLowerCase(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_toUpperCase(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_trim(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_repeat(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_padStart(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_padEnd(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_charAt(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_number_toString(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_number_toFixed(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_number_toPrecision(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_number_toExponential(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_parseInt(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_parseFloat(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_btoa(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_atob(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Object(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_RegExp(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Promise(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Promise_resolve(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Promise_reject(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Promise_try(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Promise_all(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Promise_race(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_promise_then(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_promise_catch(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_promise_finally(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Date(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Date_now(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Map(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Set(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_WeakMap(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_WeakSet(struct js *js, jsval_t *args, int nargs);
static jsval_t map_set(struct js *js, jsval_t *args, int nargs);
static jsval_t map_get(struct js *js, jsval_t *args, int nargs);
static jsval_t map_has(struct js *js, jsval_t *args, int nargs);
static jsval_t map_delete(struct js *js, jsval_t *args, int nargs);
static jsval_t map_clear(struct js *js, jsval_t *args, int nargs);
static jsval_t map_size(struct js *js, jsval_t *args, int nargs);
static jsval_t map_entries(struct js *js, jsval_t *args, int nargs);
static jsval_t map_keys(struct js *js, jsval_t *args, int nargs);
static jsval_t map_values(struct js *js, jsval_t *args, int nargs);
static jsval_t map_forEach(struct js *js, jsval_t *args, int nargs);
static jsval_t set_add(struct js *js, jsval_t *args, int nargs);
static jsval_t set_has(struct js *js, jsval_t *args, int nargs);
static jsval_t set_delete(struct js *js, jsval_t *args, int nargs);
static jsval_t set_clear(struct js *js, jsval_t *args, int nargs);
static jsval_t set_size(struct js *js, jsval_t *args, int nargs);
static jsval_t set_values(struct js *js, jsval_t *args, int nargs);
static jsval_t set_forEach(struct js *js, jsval_t *args, int nargs);
static jsval_t weakmap_set(struct js *js, jsval_t *args, int nargs);
static jsval_t weakmap_get(struct js *js, jsval_t *args, int nargs);
static jsval_t weakmap_has(struct js *js, jsval_t *args, int nargs);
static jsval_t weakmap_delete(struct js *js, jsval_t *args, int nargs);
static jsval_t weakset_add(struct js *js, jsval_t *args, int nargs);
static jsval_t weakset_has(struct js *js, jsval_t *args, int nargs);
static jsval_t weakset_delete(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_function_call(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_function_apply(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_function_bind(struct js *js, jsval_t *args, int nargs);

static jsval_t builtin_Math_abs(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_acos(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_acosh(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_asin(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_asinh(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_atan(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_atanh(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_atan2(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_cbrt(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_ceil(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_clz32(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_cos(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_cosh(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_exp(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_expm1(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_floor(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_fround(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_hypot(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_imul(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_log(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_log1p(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_log10(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_log2(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_max(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_min(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_pow(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_random(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_round(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_sign(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_sin(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_sinh(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_sqrt(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_tan(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_tanh(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Math_trunc(struct js *js, jsval_t *args, int nargs);

static jsval_t call_js(struct js *js, const char *fn, jsoff_t fnlen, jsval_t closure_scope);
static jsval_t call_js_with_args(struct js *js, jsval_t func, jsval_t *args, int nargs);
static jsval_t call_js_code_with_args(struct js *js, const char *fn, jsoff_t fnlen, jsval_t closure_scope, jsval_t *args, int nargs);
static jsval_t start_async_in_coroutine(struct js *js, const char *code, size_t code_len, jsval_t closure_scope, jsval_t *args, int nargs);

jsval_t js_get_proto(struct js *js, jsval_t obj);
void js_set_proto(struct js *js, jsval_t obj, jsval_t proto);
jsval_t js_setprop(struct js *js, jsval_t obj, jsval_t k, jsval_t v);
static jsoff_t lkp_proto(struct js *js, jsval_t obj, const char *key, size_t len);
jsval_t js_get_ctor_proto(struct js *js, const char *name, size_t len);
static jsval_t get_prototype_for_type(struct js *js, uint8_t type);

static jsval_t get_proto(struct js *js, jsval_t obj);
static void set_proto(struct js *js, jsval_t obj, jsval_t proto);
static jsval_t get_ctor_proto(struct js *js, const char *name, size_t len);
static jsval_t setprop(struct js *js, jsval_t obj, jsval_t k, jsval_t v);

static void free_coroutine(coroutine_t *coro);
static bool has_ready_coroutines(void);

static void mco_async_entry(mco_coro* mco) {
  async_exec_context_t *ctx = (async_exec_context_t *)mco_get_user_data(mco);
  
  struct js *js = ctx->js;
  coroutine_t *coro = ctx->coro;
  jsval_t result;
  
  if (coro && coro->nargs > 0 && coro->args) {
    result = call_js_code_with_args(js, ctx->code, (jsoff_t)ctx->code_len, ctx->closure_scope, coro->args, coro->nargs);
  } else {
    result = call_js(js, ctx->code, (jsoff_t)ctx->code_len, ctx->closure_scope);
  }
  
  ctx->result = result;
  ctx->has_error = is_err(result);
  
  if (ctx->has_error) {
    js_reject_promise(js, ctx->promise, result);
  } else {
    js_resolve_promise(js, ctx->promise, result);
  }
  
}

static void enqueue_coroutine(coroutine_t *coro) {
  if (!coro) return;
  coro->next = NULL;
  
  if (pending_coroutines.tail && pending_coroutines.tail != coro) {
    pending_coroutines.tail->next = coro;
    pending_coroutines.tail = coro;
  } else if (!pending_coroutines.tail) {
    pending_coroutines.head = coro;
    pending_coroutines.tail = coro;
  }
}

static coroutine_t *dequeue_coroutine(void) {
  coroutine_t *coro = pending_coroutines.head;
  if (coro) {
    pending_coroutines.head = coro->next;
    if (!pending_coroutines.head) {
      pending_coroutines.tail = NULL;
    }
    coro->next = NULL;
  }
  return coro;
}

static bool has_pending_coroutines(void) {
  return pending_coroutines.head != NULL;
}

static bool has_ready_coroutines(void) {
  coroutine_t *temp = pending_coroutines.head;
  while (temp) {
    if (temp->is_ready) return true;
    temp = temp->next;
  }
  return false;
}

void js_poll_events(struct js *js) {
  fetch_poll_events();
  fs_poll_events();
  
  int has_timers = has_pending_timers();
  if (has_timers) {
    int64_t next_timeout_ms = get_next_timer_timeout();
    if (next_timeout_ms <= 0) process_timers(js);
  }
  
  process_microtasks(js);
  
  coroutine_t *temp = pending_coroutines.head;
  coroutine_t *prev = NULL;
  
  while (temp) {
    coroutine_t *next = temp->next;
    
    if (temp->is_ready && temp->mco && mco_status(temp->mco) == MCO_SUSPENDED) {
      if (prev) {
        prev->next = next;
      } else {
        pending_coroutines.head = next;
      }
      if (pending_coroutines.tail == temp) {
        pending_coroutines.tail = prev;
      }
      
      mco_result res = mco_resume(temp->mco);
      
      if (res == MCO_SUCCESS && mco_status(temp->mco) == MCO_DEAD) {
        free_coroutine(temp);
      } else if (res == MCO_SUCCESS) {
        temp->is_ready = false;
        temp->next = NULL;
        if (pending_coroutines.tail) {
          pending_coroutines.tail->next = temp;
        } else {
          pending_coroutines.head = temp;
        }
        pending_coroutines.tail = temp;
      } else {
        free_coroutine(temp);
      }
      
      temp = next;
    } else {
      prev = temp;
      temp = next;
    }
  }
}

void js_run_event_loop(struct js *js) {
  while (has_pending_microtasks() || has_pending_timers() || has_pending_coroutines() || has_pending_fetches() || has_pending_fs_ops()) {
    js_poll_events(js);
    
    if (!has_pending_microtasks() && has_pending_timers() && !has_ready_coroutines()) {
      int64_t next_timeout_ms = get_next_timer_timeout();
      if (next_timeout_ms > 0) usleep(next_timeout_ms > 1000000 ? 1000000 : next_timeout_ms * 1000);
    }
    
    if (!has_pending_microtasks() && !has_pending_timers() && !has_pending_coroutines() && !has_pending_fetches() && !has_pending_fs_ops()) break;
  }
  
  js_poll_events(js);
}

static jsval_t start_async_in_coroutine(struct js *js, const char *code, size_t code_len, jsval_t closure_scope, jsval_t *args, int nargs) {
  jsval_t promise = js_mkpromise(js);  
  async_exec_context_t *ctx = (async_exec_context_t *)ANT_GC_MALLOC(sizeof(async_exec_context_t));
  if (!ctx) return js_mkerr(js, "out of memory for async context");
  
  ctx->js = js;
  ctx->code = code;
  ctx->code_len = code_len;
  ctx->closure_scope = closure_scope;
  ctx->result = js_mkundef();
  ctx->promise = promise;
  ctx->has_error = false;
  ctx->coro = NULL;
  
  mco_desc desc = mco_desc_init(mco_async_entry, 0);
  desc.user_data = ctx;
  
  mco_coro* mco = NULL;
  mco_result res = mco_create(&mco, &desc);
  if (res != MCO_SUCCESS) {
    ANT_GC_FREE(ctx);
    return js_mkerr(js, "failed to create minicoro coroutine");
  }
  
  coroutine_t *coro = (coroutine_t *)ANT_GC_MALLOC(sizeof(coroutine_t));
  if (!coro) {
    mco_destroy(mco);
    ANT_GC_FREE(ctx);
    return js_mkerr(js, "out of memory for coroutine");
  }
  
  coro->js = js;
  coro->type = CORO_ASYNC_AWAIT;
  coro->scope = closure_scope;
  coro->this_val = js->this_val;
  coro->awaited_promise = js_mkundef();
  coro->result = js_mkundef();
  coro->async_func = js->current_func;
  if (nargs > 0) {
    coro->args = (jsval_t *)ANT_GC_MALLOC(sizeof(jsval_t) * nargs);
    if (coro->args) memcpy(coro->args, args, sizeof(jsval_t) * nargs);
  } else {
    coro->args = NULL;
  }
  coro->nargs = nargs;
  coro->is_settled = false;
  coro->is_error = false;
  coro->is_done = false;
  coro->resume_point = 0;
  coro->yield_value = js_mkundef();
  coro->next = NULL;
  coro->mco = mco;
  coro->mco_started = false;
  coro->is_ready = true;
  
  ctx->coro = coro;  
  enqueue_coroutine(coro);
  
  res = mco_resume(mco);
  if (res != MCO_SUCCESS && mco_status(mco) != MCO_DEAD) {
    dequeue_coroutine();
    free_coroutine(coro);
    ANT_GC_FREE(ctx);
    return js_mkerr(js, "failed to start coroutine");
  }
  
  coro->mco_started = true;
  if (mco_status(mco) == MCO_DEAD) {
    dequeue_coroutine();
    free_coroutine(coro);
    ANT_GC_FREE(ctx);
  }
  
  return promise;
}

static void free_coroutine(coroutine_t *coro) {
  if (coro) {
    if (coro->mco) {
      mco_destroy(coro->mco);
      coro->mco = NULL;
    }
    if (coro->args) ANT_GC_FREE(coro->args);
    ANT_GC_FREE(coro);
  }
}

static jsval_t resume_coroutine_wrapper(struct js *js, jsval_t *args, int nargs);
static jsval_t reject_coroutine_wrapper(struct js *js, jsval_t *args, int nargs);

static void setlwm(struct js *js) {
  jsoff_t n = 0, css = 0;
  if (js->brk < js->size) n = js->size - js->brk;
  if (js->lwm > n) js->lwm = n;
  if ((char *) js->cstk > (char *) &n)
    css = (jsoff_t) ((char *) js->cstk - (char *) &n);
  if (css > js->css) js->css = css;
}

static size_t cpy(char *dst, size_t dstlen, const char *src, size_t srclen) {
  size_t i = 0;
  for (i = 0; i < dstlen && i < srclen && src[i] != 0; i++) dst[i] = src[i];
  if (dstlen > 0) dst[i < dstlen ? i : dstlen - 1] = '\0';
  return i;
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
  mark_multiref(obj);
  
  if (stringify_depth >= MAX_STRINGIFY_DEPTH) return;
  stringify_stack[stringify_depth++] = obj;
  
  jsoff_t next = loadoff(js, (jsoff_t) vdata(obj)) & ~(3U | CONSTMASK);
  while (next < js->brk && next != 0) {
    jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
    jsval_t val = loadval(js, next + (jsoff_t) (sizeof(next) + sizeof(koff)));
    scan_refs(js, val);
    next = loadoff(js, next) & ~(3U | CONSTMASK);
  }
  
  stringify_depth--;
}

static void scan_arr_refs(struct js *js, jsval_t obj) {
  if (is_on_stack(obj)) {
    mark_multiref(obj);
    return;
  }
  mark_multiref(obj);
  
  if (stringify_depth >= MAX_STRINGIFY_DEPTH) return;
  stringify_stack[stringify_depth++] = obj;
  
  jsoff_t next = loadoff(js, (jsoff_t) vdata(obj)) & ~(3U | CONSTMASK);
  while (next < js->brk && next != 0) {
    jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
    jsval_t val = loadval(js, next + (jsoff_t) (sizeof(next) + sizeof(koff)));
    scan_refs(js, val);
    next = loadoff(js, next) & ~(3U | CONSTMASK);
  }
  
  stringify_depth--;
}

static void scan_func_refs(struct js *js, jsval_t value) {
  jsval_t func_obj = mkval(T_OBJ, vdata(value));
  
  if (is_on_stack(func_obj)) {
    mark_multiref(func_obj);
    return;
  }
  mark_multiref(func_obj);
  
  if (stringify_depth >= MAX_STRINGIFY_DEPTH) return;
  stringify_stack[stringify_depth++] = func_obj;
  
  jsoff_t next = loadoff(js, (jsoff_t) vdata(func_obj)) & ~(3U | CONSTMASK);
  while (next < js->brk && next != 0) {
    jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
    jsval_t val = loadval(js, next + (jsoff_t) (sizeof(next) + sizeof(koff)));
    scan_refs(js, val);
    next = loadoff(js, next) & ~(3U | CONSTMASK);
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
    return find_multiref(obj);
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
  size_t n = 0;
  for (int i = 0; i < level * 2 && n < len; i++) {
    buf[n++] = ' ';
  }
  return n;
}

static size_t strbigint(struct js *js, jsval_t value, char *buf, size_t len);

static size_t strarr(struct js *js, jsval_t obj, char *buf, size_t len) {
  int ref = get_circular_ref(obj);
  if (ref) return (size_t) snprintf(buf, len, "[Circular *%d]", ref);
  
  push_stringify(obj);
  size_t n = cpy(buf, len, "[ ", 2);
  jsoff_t next = loadoff(js, (jsoff_t) vdata(obj)) & ~(3U | CONSTMASK);
  jsoff_t length = 0;
  jsoff_t scan = next;
  
  while (scan < js->brk && scan != 0) {
    jsoff_t koff = loadoff(js, scan + (jsoff_t) sizeof(scan));
    jsoff_t klen = offtolen(loadoff(js, koff));
    const char *key = (char *) &js->mem[koff + sizeof(koff)];
    
    if (streq(key, klen, "length", 6)) {
      jsval_t val = loadval(js, scan + (jsoff_t) (sizeof(scan) + sizeof(koff)));
      if (vtype(val) == T_NUM) length = (jsoff_t) tod(val);
      break;
    }
    scan = loadoff(js, scan) & ~(3U | CONSTMASK);
  }
  
  for (jsoff_t i = 0; i < length; i++) {
    if (i > 0) n += cpy(buf + n, len - n, ", ", 2);
    char idx[16];
    snprintf(idx, sizeof(idx), "%u", (unsigned) i);
    
    jsoff_t idxlen = (jsoff_t) strlen(idx);
    jsoff_t prop = next;
    jsval_t val = js_mkundef();
    
    bool found = false;
    
    while (prop < js->brk && prop != 0) {
      jsoff_t koff = loadoff(js, prop + (jsoff_t) sizeof(prop));
      jsoff_t klen = offtolen(loadoff(js, koff));
      const char *key = (char *) &js->mem[koff + sizeof(koff)];
      if (streq(key, klen, idx, idxlen)) {
        val = loadval(js, prop + (jsoff_t) (sizeof(prop) + sizeof(koff)));
        found = true;
        break;
      }
      prop = loadoff(js, prop) & ~(3U | CONSTMASK);
    }
    
    if (found) {
      n += tostr(js, val, buf + n, len - n);
    } else {
      n += cpy(buf + n, len - n, "undefined", 9);
    }
  }
  
  n += cpy(buf + n, len - n, " ]", 2);
  pop_stringify();
  return n;
}

static size_t array_to_string(struct js *js, jsval_t obj, char *buf, size_t len) {
  if (is_circular(obj)) return cpy(buf, len, "", 0);
  
  push_stringify(obj);
  size_t n = 0;
  jsoff_t next = loadoff(js, (jsoff_t) vdata(obj)) & ~(3U | CONSTMASK);
  jsoff_t length = 0;
  jsoff_t scan = next;
  
  while (scan < js->brk && scan != 0) {
    jsoff_t koff = loadoff(js, scan + (jsoff_t) sizeof(scan));
    jsoff_t klen = offtolen(loadoff(js, koff));
    const char *key = (char *) &js->mem[koff + sizeof(koff)];
    
    if (streq(key, klen, "length", 6)) {
      jsval_t val = loadval(js, scan + (jsoff_t) (sizeof(scan) + sizeof(koff)));
      if (vtype(val) == T_NUM) length = (jsoff_t) tod(val);
      break;
    }
    scan = loadoff(js, scan) & ~(3U | CONSTMASK);
  }
  
  for (jsoff_t i = 0; i < length; i++) {
    if (i > 0) n += cpy(buf + n, len - n, ",", 1);
    char idx[16];
    snprintf(idx, sizeof(idx), "%u", (unsigned) i);
    
    jsoff_t idxlen = (jsoff_t) strlen(idx);
    jsoff_t prop = next;
    jsval_t val = js_mkundef();
    
    bool found = false;
    
    while (prop < js->brk && prop != 0) {
      jsoff_t koff = loadoff(js, prop + (jsoff_t) sizeof(prop));
      jsoff_t klen = offtolen(loadoff(js, koff));
      const char *key = (char *) &js->mem[koff + sizeof(koff)];
      if (streq(key, klen, idx, idxlen)) {
        val = loadval(js, prop + (jsoff_t) (sizeof(prop) + sizeof(koff)));
        found = true;
        break;
      }
      prop = loadoff(js, prop) & ~(3U | CONSTMASK);
    }
    
    if (found) {
      n += tostr(js, val, buf + n, len - n);
    } else {
      n += cpy(buf + n, len - n, "", 0);
    }
  }
  
  pop_stringify();
  return n;
}

static size_t strdate(struct js *js, jsval_t obj, char *buf, size_t len) {
  jsoff_t time_off = lkp(js, obj, "__time", 6);
  if (time_off == 0) return cpy(buf, len, "Invalid Date", 12);
  
  jsval_t time_val = resolveprop(js, mkval(T_PROP, time_off));
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

static jsoff_t vstr(struct js *js, jsval_t value, jsoff_t *len);
static size_t strstring(struct js *js, jsval_t value, char *buf, size_t len);

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
  if (is_valid_identifier(str, slen)) {
    return cpy(buf, len, str, slen);
  }
  return strstring(js, value, buf, len);
}

static size_t strobj(struct js *js, jsval_t obj, char *buf, size_t len) {
  jsoff_t time_off = lkp(js, obj, "__time", 6);
  if (time_off != 0) return strdate(js, obj, buf, len);
  
  int ref = get_circular_ref(obj);
  if (ref) return (size_t) snprintf(buf, len, "[Circular *%d]", ref);
  
  push_stringify(obj);
  
  size_t n = 0;
  int self_ref = get_self_ref(obj);
  if (self_ref) {
    n += (size_t) snprintf(buf + n, len - n, "<ref *%d> ", self_ref);
  }
  
  jsoff_t tag_off = lkp(js, obj, "@@toStringTag", 13);
  if (tag_off != 0) {
    jsval_t tag_val = resolveprop(js, mkval(T_PROP, tag_off));
    if (vtype(tag_val) == T_STR) {
      jsoff_t tlen, toff = vstr(js, tag_val, &tlen);
      n += cpy(buf + n, len - n, "Object [", 8);
      n += cpy(buf + n, len - n, (const char *) &js->mem[toff], tlen);
      n += cpy(buf + n, len - n, "] {\n", 4);
    } else {
      n += cpy(buf + n, len - n, "{\n", 2);
    }
  } else {
    n += cpy(buf + n, len - n, "{\n", 2);
  }
  
  stringify_indent++;
  jsoff_t next = loadoff(js, (jsoff_t) vdata(obj)) & ~(3U | CONSTMASK);
  bool first = true;
  
  while (next < js->brk && next != 0) {
    jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
    jsoff_t klen = offtolen(loadoff(js, koff));
    const char *key = (char *) &js->mem[koff + sizeof(koff)];
    if (!streq(key, klen, "__proto__", 9) && !streq(key, klen, "@@toStringTag", 13) && !streq(key, klen, "__getter", 8)) {
      jsval_t val = loadval(js, next + (jsoff_t) (sizeof(next) + sizeof(koff)));
      if (!first) n += cpy(buf + n, len - n, ",\n", 2);
      first = false;
      n += add_indent(buf + n, len - n, stringify_indent);
      n += strkey(js, mkval(T_STR, koff), buf + n, len - n);
      n += cpy(buf + n, len - n, ": ", 2);
      n += tostr(js, val, buf + n, len - n);
    }
    next = loadoff(js, next) & ~(3U | CONSTMASK);
  }
  
  stringify_indent--;
  if (!first) n += cpy(buf + n, len - n, "\n", 1);
  n += add_indent(buf + n, len - n, stringify_indent);
  n += cpy(buf + n, len - n, "}", 1);
  pop_stringify();
  return n;
}

static size_t strnum(jsval_t value, char *buf, size_t len) {
  double dv = tod(value), iv;
  double frac = modf(dv, &iv);
  
  if (isnan(dv)) return cpy(buf, len, "NaN", 3);
  if (isinf(dv)) return cpy(buf, len, dv > 0 ? "Infinity" : "-Infinity", dv > 0 ? 8 : 9);
  
  if (dv >= -9007199254740991.0 && dv <= 9007199254740991.0) {
    if (frac == 0.0) {
      return (size_t) snprintf(buf, len, "%.0f", dv);
    } else {
      return (size_t) snprintf(buf, len, "%.17g", dv);
    }
  }
  return (size_t) snprintf(buf, len, "%g", dv);
}

static jsoff_t vstr(struct js *js, jsval_t value, jsoff_t *len) {
  jsoff_t off = (jsoff_t) vdata(value);
  if (len) *len = offtolen(loadoff(js, off));
  return (jsoff_t) (off + sizeof(off));
}

static size_t strstring(struct js *js, jsval_t value, char *buf, size_t len) {
  jsoff_t slen, off = vstr(js, value, &slen);
  const char *str = (const char *) &js->mem[off];
  size_t n = 0;
  n += cpy(buf + n, len - n, "'", 1);
  for (jsoff_t i = 0; i < slen && n < len - 1; i++) {
    char c = str[i];
    if (c == '\n') { n += cpy(buf + n, len - n, "\\n", 2); }
    else if (c == '\r') { n += cpy(buf + n, len - n, "\\r", 2); }
    else if (c == '\t') { n += cpy(buf + n, len - n, "\\t", 2); }
    else if (c == '\\') { n += cpy(buf + n, len - n, "\\\\", 2); }
    else if (c == '\'') { n += cpy(buf + n, len - n, "\\'", 2); }
    else { if (n < len) buf[n++] = c; }
  }
  n += cpy(buf + n, len - n, "'", 1);
  
  return n;
}

static bool is_internal_prop(const char *key, jsoff_t klen) {
  if (klen == 6 && memcmp(key, "__code", 6) == 0) return true;
  if (klen == 13 && memcmp(key, "__native_func", 13) == 0) return true;
  if (klen == 9 && memcmp(key, "prototype", 9) == 0) return true;
  if (klen == 9 && memcmp(key, "__proto__", 9) == 0) return true;
  return false;
}

static size_t strfunc_ctor(struct js *js, jsval_t func_obj, char *buf, size_t len) {
  int ref = get_circular_ref(func_obj);
  if (ref) return (size_t) snprintf(buf, len, "[Circular *%d]", ref);
  push_stringify(func_obj);
  
  size_t n = cpy(buf, len, "{\n", 2);
  stringify_indent++;
  bool first = true;
  
  jsoff_t next = loadoff(js, (jsoff_t) vdata(func_obj)) & ~(3U | CONSTMASK);
  while (next < js->brk && next != 0) {
    jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
    jsval_t val = loadval(js, next + (jsoff_t) (sizeof(next) + sizeof(koff)));
    
    jsoff_t klen = offtolen(loadoff(js, koff));
    const char *kstr = (const char *) &js->mem[koff + sizeof(jsoff_t)];
    
    if (!is_internal_prop(kstr, klen) && !streq(kstr, klen, "name", 4) && !streq(kstr, klen, "@@toStringTag", 13) && !streq(kstr, klen, "__getter", 8)) {
      if (!first) n += cpy(buf + n, len - n, ",\n", 2);
      first = false;
      n += add_indent(buf + n, len - n, stringify_indent);
      n += strkey(js, mkval(T_STR, koff), buf + n, len - n);
      n += cpy(buf + n, len - n, ": ", 2);
      n += tostr(js, val, buf + n, len - n);
    }
    next = loadoff(js, next) & ~(3U | CONSTMASK);
  }
  
  jsoff_t proto_off = lkp(js, func_obj, "prototype", 9);
  if (proto_off != 0) {
    jsval_t proto_val = resolveprop(js, mkval(T_PROP, proto_off));
    if (vtype(proto_val) == T_OBJ) {
      jsoff_t proto_next = loadoff(js, (jsoff_t) vdata(proto_val)) & ~(3U | CONSTMASK);
      while (proto_next < js->brk && proto_next != 0) {
        jsoff_t pkoff = loadoff(js, proto_next + (jsoff_t) sizeof(proto_next));
        jsval_t pval = loadval(js, proto_next + (jsoff_t) (sizeof(proto_next) + sizeof(pkoff)));
        
        jsoff_t pklen = offtolen(loadoff(js, pkoff));
        const char *pkstr = (const char *) &js->mem[pkoff + sizeof(jsoff_t)];
        
        if (!is_internal_prop(pkstr, pklen) && !streq(pkstr, pklen, "name", 4) && !streq(pkstr, pklen, "@@toStringTag", 13) && !streq(pkstr, pklen, "__getter", 8)) {
          if (!first) n += cpy(buf + n, len - n, ",\n", 2);
          first = false;
          n += add_indent(buf + n, len - n, stringify_indent);
          n += strkey(js, mkval(T_STR, pkoff), buf + n, len - n);
          n += cpy(buf + n, len - n, ": ", 2);
          n += tostr(js, pval, buf + n, len - n);
        }
        proto_next = loadoff(js, proto_next) & ~(3U | CONSTMASK);
      }
    }
  }
  
  stringify_indent--;
  if (!first) n += cpy(buf + n, len - n, "\n", 1);
  n += add_indent(buf + n, len - n, stringify_indent);
  n += cpy(buf + n, len - n, "}", 1);
  pop_stringify();
  return n;
}

static size_t strfunc(struct js *js, jsval_t value, char *buf, size_t len) {
  jsval_t func_obj = mkval(T_OBJ, vdata(value));
  jsoff_t code_off = lkp(js, func_obj, "__code", 6);
  
  jsoff_t name_off = lkp(js, func_obj, "name", 4);
  const char *name = NULL;
  jsoff_t name_len = 0;
  if (name_off != 0) {
    jsval_t name_val = resolveprop(js, mkval(T_PROP, name_off));
    if (vtype(name_val) == T_STR) {
      name_len = 0;
      jsoff_t noff = vstr(js, name_val, &name_len);
      name = (const char *) &js->mem[noff];
    }
  }
  
  if (code_off == 0) {
    jsoff_t native_off = lkp(js, func_obj, "__native_func", 13);
    if (native_off != 0) {
      jsval_t native_val = resolveprop(js, mkval(T_PROP, native_off));
      if (vtype(native_val) == T_CFUNC) return strfunc_ctor(js, func_obj, buf, len);
    }
    if (name && name_len > 0) {
      size_t n = cpy(buf, len, "[Function: ", 11);
      n += cpy(buf + n, len - n, name, name_len);
      n += cpy(buf + n, len - n, "]", 1);
      return n;
    }
    return cpy(buf, len, "[Function (anonymous)]", 22);
  }
  jsval_t code_val = resolveprop(js, mkval(T_PROP, code_off));
  
  if (vtype(code_val) != T_STR) {
    if (name && name_len > 0) {
      size_t n = cpy(buf, len, "[Function: ", 11);
      n += cpy(buf + n, len - n, name, name_len);
      n += cpy(buf + n, len - n, "]", 1);
      return n;
    }
    return cpy(buf, len, "[Function (anonymous)]", 22);
  }
  
  jsoff_t sn, off = vstr(js, code_val, &sn);
  if (sn >= 9 && memcmp(&js->mem[off], "__builtin", 9) == 0) {
    return strfunc_ctor(js, func_obj, buf, len);
  }
  
  if (name && name_len > 0) {
    size_t n = cpy(buf, len, "[Function: ", 11);
    n += cpy(buf + n, len - n, name, name_len);
    n += cpy(buf + n, len - n, "]", 1);
    return n;
  }
  return cpy(buf, len, "[Function (anonymous)]", 22);
}

static void get_line_col(const char *code, jsoff_t pos, int *line, int *col) {
  *line = 1;
  *col = 1;
  for (jsoff_t i = 0; i < pos && code[i] != '\0'; i++) {
    if (code[i] == '\n') {
      (*line)++;
      *col = 1;
    } else {
      (*col)++;
    }
  }
}

static void get_error_line(const char *code, jsoff_t clen, jsoff_t pos, char *buf, size_t bufsize, int *line_start_col) {
  jsoff_t line_start = pos;
  while (line_start > 0 && code[line_start - 1] != '\n') {
    line_start--;
  }
  
  jsoff_t line_end = pos;
  while (line_end < clen && code[line_end] != '\n' && code[line_end] != '\0') {
    line_end++;
  }
  
  jsoff_t line_len = line_end - line_start;
  if (line_len >= bufsize) line_len = bufsize - 1;
  
  memcpy(buf, &code[line_start], line_len);
  buf[line_len] = '\0';
  *line_start_col = (int)(pos - line_start) + 1;
}

jsval_t js_mkerr(struct js *js, const char *xx, ...) {
  va_list ap;
  int line = 0, col = 0;
  char error_line[256] = {0};
  int error_col = 0;
  get_line_col(js->code, js->toff > 0 ? js->toff : js->pos, &line, &col);
  get_error_line(js->code, js->clen, js->toff > 0 ? js->toff : js->pos, error_line, sizeof(error_line), &error_col);
  size_t n = 0;
  if (js->filename) {
    n = (size_t) snprintf(js->errmsg, sizeof(js->errmsg), "Panic: %s:%d:%d\n", js->filename, line, col);
  } else {
    n = (size_t) snprintf(js->errmsg, sizeof(js->errmsg), "Panic: <eval>:%d:%d\n", line, col);
  }
  va_start(ap, xx);
  vsnprintf(js->errmsg + n, sizeof(js->errmsg) - n, xx, ap);
  va_end(ap);
  n = strlen(js->errmsg);
  if (n < sizeof(js->errmsg) - 1) {
    js->errmsg[n++] = '\n';
  }
  size_t remaining = sizeof(js->errmsg) - n;
  size_t added = (size_t) snprintf(js->errmsg + n, remaining, "%s\n", error_line);
  n += added;
  if (n < sizeof(js->errmsg) - 1) {
    remaining = sizeof(js->errmsg) - n;
    for (int i = 1; i < error_col && remaining > 1; i++) {
      js->errmsg[n++] = ' ';
      remaining--;
    }
    if (remaining > 3) {
      js->errmsg[n++] = '^';
      js->errmsg[n++] = '^';
      js->errmsg[n++] = '^';
    }
    js->errmsg[n] = '\0';
  }
  js->errmsg[sizeof(js->errmsg) - 1] = '\0';
  js->pos = js->clen, js->tok = TOK_EOF, js->consumed = 0;
  return mkval(T_ERR, 0);
}

static jsval_t js_throw(struct js *js, jsval_t value) {
  int line = 0, col = 0;
  get_line_col(js->code, js->toff > 0 ? js->toff : js->pos, &line, &col);
  
  size_t n = 0;
  if (vtype(value) == T_STR) {
    jsoff_t slen, off = vstr(js, value, &slen);
    n = (size_t) snprintf(js->errmsg, sizeof(js->errmsg), "Uncaught: %.*s\n", (int)slen, (char *)&js->mem[off]);
  } else if (vtype(value) == T_OBJ) {
    jsoff_t name_off = lkp(js, value, "name", 4);
    jsoff_t msg_off = lkp(js, value, "message", 7);
    
    const char *name_str = NULL;
    const char *msg_str = NULL;
    jsoff_t name_len = 0, msg_len = 0;
    
    if (name_off > 0) {
      jsval_t name_val = resolveprop(js, mkval(T_PROP, name_off));
      if (vtype(name_val) == T_STR) {
        jsoff_t off = vstr(js, name_val, &name_len);
        name_str = (const char *)&js->mem[off];
      }
    }
    
    if (msg_off > 0) {
      jsval_t msg_val = resolveprop(js, mkval(T_PROP, msg_off));
      if (vtype(msg_val) == T_STR) {
        jsoff_t off = vstr(js, msg_val, &msg_len);
        msg_str = (const char *)&js->mem[off];
      }
    }
    
    if (name_str && msg_str) {
      n = (size_t) snprintf(js->errmsg, sizeof(js->errmsg), "Uncaught %.*s: %.*s\n", (int)name_len, name_str, (int)msg_len, msg_str);
    } else if (name_str) {
      n = (size_t) snprintf(js->errmsg, sizeof(js->errmsg), "Uncaught %.*s\n", (int)name_len, name_str);
    } else if (msg_str) {
      n = (size_t) snprintf(js->errmsg, sizeof(js->errmsg), "Uncaught: %.*s\n", (int)msg_len, msg_str);
    } else {
      const char *str = js_str(js, value);
      n = (size_t) snprintf(js->errmsg, sizeof(js->errmsg), "Uncaught: %s\n", str);
    }
  } else {
    const char *str = js_str(js, value);
    n = (size_t) snprintf(js->errmsg, sizeof(js->errmsg), "Uncaught: %s\n", str);
  }
  
  size_t remaining = sizeof(js->errmsg) - n;
  if (remaining > 20) {
    if (js->filename) {
      n += (size_t) snprintf(js->errmsg + n, remaining, "  at %s:%d:%d\n", js->filename, line, col);
    } else {
      n += (size_t) snprintf(js->errmsg + n, remaining, "  at <eval>:%d:%d\n", line, col);
    }
  }
  
  remaining = sizeof(js->errmsg) - n;
  for (int i = global_call_stack.depth - 1; i >= 0 && remaining > 20; i--) {
    call_frame_t *frame = &global_call_stack.frames[i];
    const char *fname = frame->function_name ? frame->function_name : "<anonymous>";
    const char *file = frame->filename ? frame->filename : "<eval>";
    
    size_t added = (size_t) snprintf(js->errmsg + n, remaining, "  at %s (%s:%d:%d)\n", fname, file, frame->line, frame->col);
    n += added;
    remaining = sizeof(js->errmsg) - n;
  }
  
  js->errmsg[sizeof(js->errmsg) - 1] = '\0';
  js->flags |= F_THROW;
  js->pos = js->clen;
  js->tok = TOK_EOF;
  js->consumed = 0;
  return mkval(T_ERR, 0);
}

static size_t tostr(struct js *js, jsval_t value, char *buf, size_t len) {
  switch (vtype(value)) {
    case T_UNDEF: return cpy(buf, len, "undefined", 9);
    case T_NULL:  return cpy(buf, len, "null", 4);
    case T_BOOL:  return cpy(buf, len, vdata(value) & 1 ? "true" : "false", vdata(value) & 1 ? 4 : 5);
    case T_ARR:   return strarr(js, value, buf, len);
    case T_OBJ:   return strobj(js, value, buf, len);
    case T_STR:   return strstring(js, value, buf, len);
    case T_NUM:   return strnum(value, buf, len);
    case T_BIGINT: return strbigint(js, value, buf, len);
    case T_PROMISE: return strpromise(js, value, buf, len);
    case T_FUNC:  return strfunc(js, value, buf, len);
    case T_CFUNC: return cpy(buf, len, "[Function (native)]", 19);
    case T_PROP:  return (size_t) snprintf(buf, len, "PROP@%lu", (unsigned long) vdata(value));
    default:      return (size_t) snprintf(buf, len, "VTYPE%d", vtype(value));
  }
}

const char *js_str(struct js *js, jsval_t value) {
  char *buf = (char *) &js->mem[js->brk + sizeof(jsoff_t)];
  size_t len, available = js->size - js->brk - sizeof(jsoff_t);
  
  if (is_err(value)) return js->errmsg;
  if (js->brk + sizeof(jsoff_t) >= js->size) return "";
  
  multiref_count = 0;
  multiref_next_id = 0;
  stringify_depth = 0;
  scan_refs(js, value);
  
  stringify_depth = 0;
  stringify_indent = 0;
  len = tostr(js, value, buf, available);
  js_mkstr(js, NULL, len);
  return buf;
}

static bool bigint_is_zero(struct js *js, jsval_t v);

bool js_truthy(struct js *js, jsval_t v) {
  uint8_t t = vtype(v);
  return (t == T_BOOL && vdata(v) != 0) || (t == T_NUM && tod(v) != 0.0) ||
         (t == T_OBJ || t == T_FUNC || t == T_ARR) || (t == T_STR && vstrlen(js, v) > 0) ||
         (t == T_BIGINT && !bigint_is_zero(js, v));
}

static bool js_try_grow_memory(struct js *js, size_t needed) {
  if (!js->owns_mem) return false;
  if (js->max_size == 0) return false;
  
  size_t current_total = sizeof(struct js) + js->size;
  size_t new_size = current_total * 2;
  
  while (new_size < current_total + needed && new_size <= (size_t)js->max_size) new_size *= 2;
  
  if (new_size > (size_t)js->max_size) new_size = (size_t)js->max_size;
  if (new_size <= current_total) return false;
  
  void *old_buf = (void *)((uint8_t *)js - 0);
  void *new_buf = ANT_GC_REALLOC(old_buf, new_size);
  
  if (new_buf == NULL) return false;
  struct js *new_js = (struct js *)new_buf;
  
  new_js->mem = (uint8_t *)(new_js + 1);
  jsoff_t old_size = new_js->size;
  new_js->size = (jsoff_t)(new_size - sizeof(struct js));
  new_js->size = new_js->size / 8U * 8U;
  
  if (old_size > 0) {
    new_js->gct = (new_js->size * new_js->gct) / old_size;
  } else {
    new_js->gct = new_js->size / 2;
  }
  
  return true;
}


static jsoff_t js_alloc(struct js *js, size_t size) {
  size = align32((jsoff_t) size);
  
  jsoff_t ofs = free_list_allocate(size);
  if (ofs != (jsoff_t) ~0) return ofs;
  
  ofs = js->brk;
  if (js->brk + size > js->size) {
    if (js_try_grow_memory(js, size)) {
      ofs = js->brk;
      if (js->brk + size > js->size) return ~(jsoff_t) 0;
    } else {
      ANT_GC_COLLECT();
      js_gc(js);
      ofs = js->brk;
      if (js->brk + size > js->size) {
        if (js_try_grow_memory(js, size)) {
          ofs = js->brk;
          if (js->brk + size > js->size) return ~(jsoff_t) 0;
        } else {
          return ~(jsoff_t) 0;
        }
      }
    }
  }
  
  js->brk += (jsoff_t) size;
  return ofs;
}

static jsval_t mkentity(struct js *js, jsoff_t b, const void *buf, size_t len) {
  jsoff_t ofs = js_alloc(js, len + sizeof(b));
  if (ofs == (jsoff_t) ~0) return js_mkerr(js, "oom");
  memcpy(&js->mem[ofs], &b, sizeof(b));
  if (buf != NULL) memmove(&js->mem[ofs + sizeof(b)], buf, len);
  if ((b & 3) == T_STR) js->mem[ofs + sizeof(b) + len - 1] = 0;
  return mkval(b & 3, ofs);
}

jsval_t js_mkstr(struct js *js, const void *ptr, size_t len) {
  jsoff_t n = (jsoff_t) (len + 1);
  return mkentity(js, (jsoff_t) ((n << 2) | T_STR), ptr, n);
}

static jsval_t mkbigint(struct js *js, const char *digits, size_t len, bool negative) {
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
  jsval_t r = mkbigint(js, result, rlen, rneg);
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
  jsval_t r = mkbigint(js, result, rlen, rneg);
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
  jsval_t r = mkbigint(js, result, rlen, rneg);
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
  jsval_t r = mkbigint(js, result, rlen, rneg);
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
  jsval_t r = mkbigint(js, rem, remlen, rneg);
  free(rem);
  return r;
}

static jsval_t bigint_neg(struct js *js, jsval_t a) {
  size_t len;
  const char *digits = bigint_digits(js, a, &len);
  bool neg = bigint_IsNegative(js, a);
  if (len == 1 && digits[0] == '0') return mkbigint(js, digits, len, false);
  return mkbigint(js, digits, len, !neg);
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
  if (neg) n += cpy(buf + n, len - n, "-", 1);
  n += cpy(buf + n, len - n, digits, dlen);
  return n;
}

static jsval_t builtin_BigInt(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return mkbigint(js, "0", 1, false);
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
    return mkbigint(js, buf, strlen(buf), neg);
  }
  if (vtype(arg) == T_STR) {
    jsoff_t slen, off = vstr(js, arg, &slen);
    const char *str = (const char *)&js->mem[off];
    bool neg = false;
    size_t i = 0;
    if (slen > 0 && str[0] == '-') { neg = true; i++; }
    else if (slen > 0 && str[0] == '+') { i++; }
    while (i < slen && str[i] == '0') i++;
    if (i >= slen) return mkbigint(js, "0", 1, false);
    for (size_t j = i; j < slen; j++) {
      if (!is_digit(str[j])) return js_mkerr(js, "Cannot convert string to BigInt");
    }
    return mkbigint(js, str + i, slen - i, neg);
  }
  if (vtype(arg) == T_BOOL) {
    return mkbigint(js, vdata(arg) ? "1" : "0", 1, false);
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

static jsval_t mkobj(struct js *js, jsoff_t parent) {
  return mkentity(js, 0 | T_OBJ, &parent, sizeof(parent));
}

static jsval_t mkarr(struct js *js) {
  jsval_t arr = mkobj(js, 0);
  jsval_t array_proto = get_ctor_proto(js, "Array", 5);
  if (vtype(array_proto) == T_OBJ) {
    set_proto(js, arr, array_proto);
  }
  return mkval(T_ARR, vdata(arr));
}

static jsoff_t arr_length(struct js *js, jsval_t arr) {
  if (vtype(arr) != T_ARR) return 0;
  jsoff_t scan = loadoff(js, (jsoff_t) vdata(arr)) & ~(3U | CONSTMASK);
  while (scan < js->brk && scan != 0) {
    jsoff_t koff = loadoff(js, scan + (jsoff_t) sizeof(scan));
    jsoff_t klen = offtolen(loadoff(js, koff));
    const char *key = (char *) &js->mem[koff + sizeof(koff)];
    if (streq(key, klen, "length", 6)) {
      jsval_t val = loadval(js, scan + (jsoff_t) (sizeof(scan) + sizeof(koff)));
      if (vtype(val) == T_NUM) return (jsoff_t) tod(val);
      break;
    }
    scan = loadoff(js, scan) & ~(3U | CONSTMASK);
  }
  return 0;
}

static jsval_t arr_get(struct js *js, jsval_t arr, jsoff_t idx) {
  if (vtype(arr) != T_ARR) return js_mkundef();
  char idxstr[16];
  snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) idx);
  jsoff_t idxlen = (jsoff_t) strlen(idxstr);
  jsoff_t prop = loadoff(js, (jsoff_t) vdata(arr)) & ~(3U | CONSTMASK);
  while (prop < js->brk && prop != 0) {
    jsoff_t koff = loadoff(js, prop + (jsoff_t) sizeof(prop));
    jsoff_t klen = offtolen(loadoff(js, koff));
    const char *key = (char *) &js->mem[koff + sizeof(koff)];
    if (streq(key, klen, idxstr, idxlen)) {
      return loadval(js, prop + (jsoff_t) (sizeof(prop) + sizeof(koff)));
    }
    prop = loadoff(js, prop) & ~(3U | CONSTMASK);
  }
  return js_mkundef();
}

static bool is_const_prop(struct js *js, jsoff_t propoff) {
  for (jsoff_t off = 0; off < js->brk;) {
    jsoff_t v = loadoff(js, off);
    jsoff_t n = esize(v & ~(GCMASK | CONSTMASK));
    jsoff_t cleaned = v & ~(GCMASK | CONSTMASK);
    if ((cleaned & 3) == T_OBJ) {
      jsoff_t firstprop = cleaned & ~3U;
      if (firstprop == propoff && (v & CONSTMASK)) {
        return true;
      }
    } else if ((cleaned & 3) == T_PROP) {
      jsoff_t nextprop = cleaned & ~3U;
      if (nextprop == propoff && (v & CONSTMASK)) {
        return true;
      }
    }
    off += n;
  }
  return false;
}

static inline uint32_t hash_key(const char *key, size_t len) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len && i < 64; i++) {
    hash ^= (uint8_t)key[i];
    hash *= 16777619u;
  }
  return hash;
}

static obj_prop_cache_t* get_obj_cache(jsoff_t obj_offset) {
  obj_prop_cache_t *cache = NULL;
  HASH_FIND(hh, global_property_cache, &obj_offset, sizeof(jsoff_t), cache);
  if (!cache) {
    cache = (obj_prop_cache_t *)malloc(sizeof(obj_prop_cache_t));
    if (!cache) return NULL;
    cache->obj_offset = obj_offset;
    cache->cache = NULL;
    cache->hit_count = 0;
    HASH_ADD(hh, global_property_cache, obj_offset, sizeof(jsoff_t), cache);
  }
  return cache;
}

static void cache_property(jsoff_t obj_offset, const char *key, size_t key_len, jsoff_t prop_offset) {
  if (key_len > 63) return;
  
  obj_prop_cache_t *obj_cache = get_obj_cache(obj_offset);
  if (!obj_cache) return;
  
  prop_cache_entry_t *entry = NULL;
  HASH_FIND_STR(obj_cache->cache, key, entry);
  
  if (!entry) {
    entry = (prop_cache_entry_t *)malloc(sizeof(prop_cache_entry_t));
    if (!entry) return;
    memcpy(entry->key, key, key_len);
    entry->key[key_len] = '\0';
    entry->hash = hash_key(key, key_len);
    HASH_ADD_STR(obj_cache->cache, key, entry);
  }
  entry->offset = prop_offset;
}

static jsoff_t cache_lookup(jsoff_t obj_offset, const char *key, size_t key_len) {
  if (key_len > 63) return 0;
  
  obj_prop_cache_t *obj_cache = NULL;
  HASH_FIND(hh, global_property_cache, &obj_offset, sizeof(jsoff_t), obj_cache);
  if (!obj_cache) return 0;
  
  prop_cache_entry_t *entry = NULL;
  HASH_FIND_STR(obj_cache->cache, key, entry);
  if (entry) {
    obj_cache->hit_count++;
    return entry->offset;
  }
  return 0;
}

static void invalidate_obj_cache(jsoff_t obj_offset) {
  obj_prop_cache_t *obj_cache = NULL;
  HASH_FIND(hh, global_property_cache, &obj_offset, sizeof(jsoff_t), obj_cache);
  if (!obj_cache) return;
  
  prop_cache_entry_t *entry, *tmp;
  HASH_ITER(hh, obj_cache->cache, entry, tmp) {
    HASH_DEL(obj_cache->cache, entry);
    free(entry);
  }
  HASH_DEL(global_property_cache, obj_cache);
  free(obj_cache);
}

static jsval_t mkprop(struct js *js, jsval_t obj, jsval_t k, jsval_t v, bool is_const) {
  jsoff_t koff = (jsoff_t) vdata(k);
  jsoff_t b, head = (jsoff_t) vdata(obj);
  char buf[sizeof(koff) + sizeof(v)];
  memcpy(&b, &js->mem[head], sizeof(b));
  memcpy(buf, &koff, sizeof(koff));
  memcpy(buf + sizeof(koff), &v, sizeof(v));
  jsoff_t brk = js->brk | T_OBJ;
  
  if (is_const) brk |= CONSTMASK;
  memcpy(&js->mem[head], &brk, sizeof(brk));
  invalidate_obj_cache(head);
  
  return mkentity(js, (b & ~(3U | CONSTMASK)) | T_PROP, buf, sizeof(buf));
}

jsval_t js_setprop(struct js *js, jsval_t obj, jsval_t k, jsval_t v) {
  jsoff_t koff = (jsoff_t) vdata(k);
  jsoff_t klen = offtolen(loadoff(js, koff));
  const char *key = (char *) &js->mem[koff + sizeof(jsoff_t)];
  if (streq(key, klen, "__proto__", 9)) {
    return js_mkerr(js, "cannot assign to __proto__");
  }
  jsoff_t existing = lkp(js, obj, key, klen);
  if (existing > 0) {
    char desc_key[128];
    snprintf(desc_key, sizeof(desc_key), "__desc_%.*s", (int)klen, key);
    jsoff_t desc_off = lkp(js, obj, desc_key, strlen(desc_key));
    
    if (desc_off != 0) {
      jsval_t desc_obj = resolveprop(js, mkval(T_PROP, desc_off));
      if (vtype(desc_obj) == T_OBJ) {
        jsoff_t writable_off = lkp(js, desc_obj, "writable", 8);
        if (writable_off != 0) {
          jsval_t writable_val = resolveprop(js, mkval(T_PROP, writable_off));
          if (!js_truthy(js, writable_val)) {
            if (js->flags & F_STRICT) return js_mkerr(js, "assignment to read-only property");
            return mkval(T_PROP, existing);
          }
        }
        
        jsoff_t configurable_off = lkp(js, desc_obj, "configurable", 12);
        if (configurable_off != 0) {
          jsval_t configurable_val = resolveprop(js, mkval(T_PROP, configurable_off));
          if (!js_truthy(js, configurable_val)) {
            if (js->flags & F_STRICT) return js_mkerr(js, "assignment to non-configurable property");
            return mkval(T_PROP, existing);
          }
        }
      }
    } else if (is_const_prop(js, existing)) {
      return js_mkerr(js, "assignment to constant");
    }
    
    saveval(js, existing + sizeof(jsoff_t) * 2, v);
    return mkval(T_PROP, existing);
  }
  return mkprop(js, obj, k, v, false);
}

static jsval_t setprop(struct js *js, jsval_t obj, jsval_t k, jsval_t v) {
  return js_setprop(js, obj, k, v);
}

static inline jsoff_t esize(jsoff_t w) {
  jsoff_t cleaned = w & ~(GCMASK | CONSTMASK);
  switch (cleaned & 3U) {
    case T_OBJ:   return (jsoff_t) (sizeof(jsoff_t) + sizeof(jsoff_t));
    case T_PROP:  return (jsoff_t) (sizeof(jsoff_t) + sizeof(jsoff_t) + sizeof(jsval_t));
    case T_STR:   return (jsoff_t) (sizeof(jsoff_t) + align32(cleaned >> 2U));
    default:      return (jsoff_t) ~0U;
  }
}

static bool is_mem_entity(uint8_t t) {
  return t == T_OBJ || t == T_PROP || t == T_STR || t == T_FUNC || t == T_ARR || t == T_PROMISE;
}

static void js_mark_all_entities_for_deletion(struct js *js) {
  for (jsoff_t v, off = 0; off < js->brk; off += esize(v & ~(GCMASK | CONSTMASK))) {
    v = loadoff(js, off);
    saveoff(js, off, v | GCMASK);
  }
}

static jsoff_t js_unmark_entity(struct js *js, jsoff_t off) {
  if (off >= js->brk) return 0;
  jsoff_t v = loadoff(js, off);
  if (v & GCMASK) {
    saveoff(js, off, v & ~GCMASK);
    jsoff_t cleaned = v & ~(GCMASK | CONSTMASK);
    if ((cleaned & 3) == T_OBJ) js_unmark_entity(js, cleaned & ~3);
    if ((cleaned & 3) == T_PROP) {
      js_unmark_entity(js, cleaned & ~3);
      js_unmark_entity(js, loadoff(js, (jsoff_t) (off + sizeof(off))));
      jsval_t val = loadval(js, (jsoff_t) (off + sizeof(off) + sizeof(off)));
      if (is_mem_entity(vtype(val))) js_unmark_entity(js, (jsoff_t) vdata(val));
    }
  }
  return v & ~(GCMASK | CONSTMASK | 3U);
}

static void js_unmark_jsval(struct js *js, jsval_t v) {
  if (is_mem_entity(vtype(v))) js_unmark_entity(js, (jsoff_t) vdata(v));
}

static void js_unmark_used_entities(struct js *js) {
  js_unmark_entity(js, (jsoff_t) vdata(js->scope));
  if (global_scope_stack) {
    jsoff_t *p = NULL;
    while ((p = (jsoff_t *)utarray_next(global_scope_stack, p)) != NULL) {
      js_unmark_entity(js, *p);
    }
  }
  
  js_unmark_entity(js, 0);
  if (js->nogc) js_unmark_entity(js, js->nogc);
  
  mco_coro *running = mco_running();
  if (running) {
    async_exec_context_t *ctx = (async_exec_context_t *)mco_get_user_data(running);
    if (ctx) {
      js_unmark_jsval(js, ctx->closure_scope);
      js_unmark_jsval(js, ctx->result);
      js_unmark_jsval(js, ctx->promise);
      if (ctx->coro) {
        js_unmark_jsval(js, ctx->coro->scope);
        js_unmark_jsval(js, ctx->coro->this_val);
        js_unmark_jsval(js, ctx->coro->awaited_promise);
        js_unmark_jsval(js, ctx->coro->result);
        js_unmark_jsval(js, ctx->coro->async_func);
        js_unmark_jsval(js, ctx->coro->yield_value);
        for (int i = 0; i < ctx->coro->nargs; i++) js_unmark_jsval(js, ctx->coro->args[i]);
      }
    }
  }
  
  for (coroutine_t *coro = pending_coroutines.head; coro != NULL; coro = coro->next) {
    js_unmark_jsval(js, coro->scope);
    js_unmark_jsval(js, coro->this_val);
    js_unmark_jsval(js, coro->awaited_promise);
    js_unmark_jsval(js, coro->result);
    js_unmark_jsval(js, coro->async_func);
    js_unmark_jsval(js, coro->yield_value);
    for (int i = 0; i < coro->nargs; i++) js_unmark_jsval(js, coro->args[i]);
  }
  
  for (int i = 0; i < global_this_stack.depth; i++) {
    js_unmark_jsval(js, global_this_stack.stack[i]);
  }
}

static void init_free_list(void) {
  if (global_free_list == NULL) {
    utarray_new(global_free_list, &free_list_icd);
  } else {
    utarray_clear(global_free_list);
  }
}

static void free_list_clear(void) {
  if (global_free_list != NULL) utarray_clear(global_free_list);
}

static void free_list_compact(void) {
  unsigned int len = utarray_len(global_free_list);
  if (len <= 1) return;
  
  FreeListEntry *entries = (FreeListEntry *)utarray_front(global_free_list);
  
  for (unsigned int i = 0; i < len - 1; i++) {
    for (unsigned int j = 0; j < len - i - 1; j++) {
      if (entries[j].offset > entries[j + 1].offset) {
        FreeListEntry temp = entries[j];
        entries[j] = entries[j + 1];
        entries[j + 1] = temp;
      }
    }
  }
  
  unsigned int write_pos = 0;
  for (unsigned int i = 1; i < len; i++) {
    if (entries[write_pos].offset + entries[write_pos].size == entries[i].offset) {
      entries[write_pos].size += entries[i].size;
    } else {
      write_pos++;
      entries[write_pos] = entries[i];
    }
  }
  
  unsigned int final_count = write_pos + 1;
  while (utarray_len(global_free_list) > final_count) {
    utarray_pop_back(global_free_list);
  }
}

static jsoff_t free_list_zero_out(struct js *js) {
  unsigned int len = utarray_len(global_free_list);
  if (len == 0) return 0;
  
  jsoff_t safe_threshold = protected_brk > 0 ? protected_brk + 0x400 : 0x1000;
  jsoff_t total_freed = 0;
  FreeListEntry *entries = (FreeListEntry *)utarray_front(global_free_list);
  for (unsigned int i = 0; i < len; i++) {
    if (entries[i].offset > 0 && entries[i].size > 0) {
      if (entries[i].offset < safe_threshold) continue;
      if (entries[i].offset + entries[i].size > js->size) continue;    
      // ugh disable zeroing for now until a better solution is found  
      // memset(&js->mem[entries[i].offset], 0, entries[i].size);
      total_freed += entries[i].size;
    }
  }
  
  return total_freed;
}

static jsoff_t free_list_allocate(size_t size) {
  unsigned int len = utarray_len(global_free_list);
  if (len == 0) return ~(jsoff_t) 0;
  size = align32((jsoff_t) size);
  
  FreeListEntry *entries = (FreeListEntry *)utarray_front(global_free_list);
  jsoff_t safe_reuse_threshold = protected_brk > 0 ? protected_brk + 0x400 : 0x1000;
  
  for (unsigned int i = 0; i < len; i++) {
    if (entries[i].offset >= safe_reuse_threshold && entries[i].size >= size) {
      jsoff_t allocated_offset = entries[i].offset;
      
      entries[i].offset += size;
      entries[i].size -= size;
      
      if (entries[i].size == 0) utarray_erase(global_free_list, i, 1);      
      return allocated_offset;
    }
  }
  
  return ~(jsoff_t) 0;
}

static bool is_builtin_or_system(jsoff_t offset, struct js *js) {
  jsval_t obj_val = mkval(T_OBJ, offset);
  
  jsoff_t native_off = lkp(js, obj_val, "__native_func", 13);
  if (native_off != 0) return true;
  
  jsoff_t code_off = lkp(js, obj_val, "__code", 6);
  if (code_off != 0) {
    jsval_t code_val = resolveprop(js, mkval(T_PROP, code_off));
    if (vtype(code_val) == T_STR) {
      jsoff_t slen, str_off = vstr(js, code_val, &slen);
      if (slen > 10 && memcmp(&js->mem[str_off], "__builtin_", 10) == 0) return true;
    }
  }
  
  return false;
}

static void free_list_add(jsoff_t offset, jsoff_t size, struct js *js) {
  if (offset >= js->size || size == 0 || offset + size > js->size * 2) return;
  jsoff_t safe_threshold = protected_brk > 0 ? protected_brk + 0x400 : 0x1000;
  if (offset < safe_threshold) return;
  
  jsoff_t entity_val = loadoff(js, offset);
  uint8_t entity_type = entity_val & 3;
  
  if (entity_type == T_OBJ && is_builtin_or_system(offset, js)) return;
  if (entity_type == T_STR && offset < 0x1000) return;
  
  FreeListEntry entry = {0};
  entry.offset = offset;
  entry.size = size;
  entry.type = entity_type;
  entry.detail[0] = '\0';
  
  utarray_push_back(global_free_list, &entry);
}

static void js_fixup_offsets(struct js *js, jsoff_t start, jsoff_t size) {
  for (jsoff_t n, v, off = 0; off < js->brk; off += n) {
    v = loadoff(js, off);
    n = esize(v & ~(GCMASK | CONSTMASK));
    if (v & GCMASK) continue;
    
    jsoff_t flags = v & (GCMASK | CONSTMASK);
    jsoff_t cleaned = v & ~(GCMASK | CONSTMASK);
    if ((cleaned & 3) != T_OBJ && (cleaned & 3) != T_PROP) continue;
    jsoff_t adjusted = cleaned > start ? cleaned - size : cleaned;
    if (cleaned != adjusted) saveoff(js, off, adjusted | flags);
    
    if ((cleaned & 3) == T_OBJ) {
      jsoff_t u = loadoff(js, (jsoff_t) (off + sizeof(jsoff_t)));
      if (u > start) saveoff(js, (jsoff_t) (off + sizeof(jsoff_t)), u - size);
    }
    
    #define FIXUP_JSVAL(val) do { \
      if (is_mem_entity(vtype(val)) && vdata(val) > start) \
        (val) = mkval(vtype(val), vdata(val) - size); \
    } while (0)

    #define FIXUP_JSVAL_AT(mem_off) do { \
      jsval_t _v = loadval(js, mem_off); \
      if (is_mem_entity(vtype(_v)) && vdata(_v) > start) \
        saveval(js, mem_off, mkval(vtype(_v), vdata(_v) - size)); \
    } while (0)

    #define FIXUP_OFF(off_var) do { if ((off_var) > start) (off_var) -= size; } while (0)

    if ((cleaned & 3) == T_PROP) {
      jsoff_t koff = loadoff(js, (jsoff_t) (off + sizeof(off)));
      if (koff > start) saveoff(js, (jsoff_t) (off + sizeof(off)), koff - size);
      FIXUP_JSVAL_AT((jsoff_t) (off + sizeof(off) + sizeof(off)));
    }
  }
  
  FIXUP_JSVAL(js->scope);
  FIXUP_OFF(js->nogc);
  if (js->code > (char *) js->mem && js->code - (char *) js->mem < js->size && js->code - (char *) js->mem > start) {
    js->code -= size;
  }
  
  if (global_scope_stack) {
    jsoff_t *p = NULL;
    while ((p = (jsoff_t *)utarray_next(global_scope_stack, p)) != NULL) FIXUP_OFF(*p);
  }

  for (coroutine_t *coro = pending_coroutines.head; coro != NULL; coro = coro->next) {
    FIXUP_JSVAL(coro->scope);
    FIXUP_JSVAL(coro->this_val);
    FIXUP_JSVAL(coro->awaited_promise);
    FIXUP_JSVAL(coro->result);
    FIXUP_JSVAL(coro->async_func);
    FIXUP_JSVAL(coro->yield_value);
    for (int i = 0; i < coro->nargs; i++) FIXUP_JSVAL(coro->args[i]);
  }

  #undef FIXUP_JSVAL
  #undef FIXUP_JSVAL_AT
  #undef FIXUP_OFF
}

static void js_compact_from_end(struct js *js) {
  jsoff_t new_brk = js->brk;
  
  jsoff_t min_brk = (jsoff_t) vdata(js->scope) + 8;
  if (global_scope_stack) {
    jsoff_t *p = NULL;
    while ((p = (jsoff_t *)utarray_next(global_scope_stack, p)) != NULL) {
      if (*p + 8 > min_brk) min_brk = *p + 8;
    }
  }
  
  for (jsoff_t off = 0; off < js->brk; off += esize(loadoff(js, off) & ~(GCMASK | CONSTMASK))) {
    jsoff_t v = loadoff(js, off);
    if ((v & GCMASK) == 0) new_brk = off + esize(v & ~(GCMASK | CONSTMASK));
  }
  
  if (new_brk < min_brk) new_brk = min_brk;
  js->brk = new_brk;
}

static void js_clear_gc_marks(struct js *js) {
  for (jsoff_t v, off = 0; off < js->brk; off += esize(v & ~(GCMASK | CONSTMASK))) {
    v = loadoff(js, off);
    if (v & GCMASK) {
      jsoff_t size = esize(v & ~(GCMASK | CONSTMASK));
      free_list_add(off, size, js);
      saveoff(js, off, v & ~GCMASK);
    }
  }
}

jsoff_t js_gc(struct js *js) {
  setlwm(js);
  if (js->nogc == (jsoff_t) ~0) return 0;
  
  js_mark_all_entities_for_deletion(js);
  js_unmark_used_entities(js);
  js_compact_from_end(js);
  
  js_clear_gc_marks(js);
  free_list_compact();
  
  jsoff_t freed = free_list_zero_out(js);
  if (global_free_list != NULL) utarray_clear(global_free_list);
    
  return freed;
}

static jsoff_t skiptonext(const char *code, jsoff_t len, jsoff_t n) {
  while (n < len) {
    if (is_space(code[n])) {
      n++;
    } else if (n + 1 < len && code[n] == '/' && code[n + 1] == '/') {
      for (n += 2; n < len && code[n] != '\n';) n++;
    } else if (n + 3 < len && code[n] == '/' && code[n + 1] == '*') {
      for (n += 4; n < len && (code[n - 2] != '*' || code[n - 1] != '/');) n++;
    } else {
      break;
    }
  }
  return n;
}

static bool streq(const char *buf, size_t len, const char *p, size_t n) {
  return n == len && memcmp(buf, p, len) == 0;
}

static bool is_strict_reserved(const char *buf, size_t len) {
  switch (len) {
    case 3: return streq(buf, len, "let", 3);
    case 5: return streq(buf, len, "yield", 5);
    case 6: return streq(buf, len, "static", 6) || streq(buf, len, "public", 6);
    case 7: return streq(buf, len, "private", 7) || streq(buf, len, "package", 7);
    case 9: return streq(buf, len, "interface", 9) || streq(buf, len, "protected", 9);
    case 10: return streq(buf, len, "implements", 10);
    default: return false;
  }
}

static bool is_strict_restricted(const char *buf, size_t len) {
  return (len == 4 && streq(buf, len, "eval", 4)) || (len == 9 && streq(buf, len, "arguments", 9));
}

static uint8_t parsekeyword(const char *buf, size_t len) {
  switch (buf[0]) {
    case 'a': if (streq("async", 5, buf, len)) return TOK_ASYNC; if (streq("await", 5, buf, len)) return TOK_AWAIT; if (streq("as", 2, buf, len)) return TOK_AS; break;
    case 'b': if (streq("break", 5, buf, len)) return TOK_BREAK; break;
    case 'c': if (streq("class", 5, buf, len)) return TOK_CLASS; if (streq("case", 4, buf, len)) return TOK_CASE; if (streq("catch", 5, buf, len)) return TOK_CATCH; if (streq("const", 5, buf, len)) return TOK_CONST; if (streq("continue", 8, buf, len)) return TOK_CONTINUE; break;
    case 'd': if (streq("do", 2, buf, len)) return TOK_DO;  if (streq("default", 7, buf, len)) return TOK_DEFAULT; if (streq("delete", 6, buf, len)) return TOK_DELETE; break;
    case 'e': if (streq("else", 4, buf, len)) return TOK_ELSE; if (streq("export", 6, buf, len)) return TOK_EXPORT; break;
    case 'f': if (streq("for", 3, buf, len)) return TOK_FOR; if (streq("from", 4, buf, len)) return TOK_FROM; if (streq("function", 8, buf, len)) return TOK_FUNC; if (streq("finally", 7, buf, len)) return TOK_FINALLY; if (streq("false", 5, buf, len)) return TOK_FALSE; break;
    case 'i': if (streq("if", 2, buf, len)) return TOK_IF; if (streq("import", 6, buf, len)) return TOK_IMPORT; if (streq("in", 2, buf, len)) return TOK_IN; if (streq("instanceof", 10, buf, len)) return TOK_INSTANCEOF; break;
    case 'l': if (streq("let", 3, buf, len)) return TOK_LET; break;
    case 'n': if (streq("new", 3, buf, len)) return TOK_NEW; if (streq("null", 4, buf, len)) return TOK_NULL; break;
    case 'o': if (streq("of", 2, buf, len)) return TOK_OF; break;
    case 'r': if (streq("return", 6, buf, len)) return TOK_RETURN; break;
    case 's': if (streq("switch", 6, buf, len)) return TOK_SWITCH; if (streq("static", 6, buf, len)) return TOK_STATIC; break;
    case 't': if (streq("try", 3, buf, len)) return TOK_TRY; if (streq("this", 4, buf, len)) return TOK_THIS; if (streq("throw", 5, buf, len)) return TOK_THROW; if (streq("true", 4, buf, len)) return TOK_TRUE; if (streq("typeof", 6, buf, len)) return TOK_TYPEOF; break;
    case 'u': if (streq("undefined", 9, buf, len)) return TOK_UNDEF; break;
    case 'v': if (streq("var", 3, buf, len)) return TOK_VAR; if (streq("void", 4, buf, len)) return TOK_VOID; break;
    case 'w': if (streq("while", 5, buf, len)) return TOK_WHILE; if (streq("with", 4, buf, len)) return TOK_WITH; break;
    case 'y': if (streq("yield", 5, buf, len)) return TOK_YIELD; break;
  }
  return TOK_IDENTIFIER;
}

static uint8_t parseident(const char *buf, jsoff_t len, jsoff_t *tlen) {
  if (is_ident_begin(buf[0])) {
    while (*tlen < len && is_ident_continue(buf[*tlen])) (*tlen)++;
    return parsekeyword(buf, *tlen);
  }
  return TOK_ERR;
}

static uint8_t next(struct js *js) {
  if (js->consumed == 0) return js->tok;
  
  js->consumed = 0;
  js->tok = TOK_ERR;
  js->toff = js->pos = skiptonext(js->code, js->clen, js->pos);
  js->tlen = 0;
  
  const char *buf = js->code + js->toff;
  if (js->toff >= js->clen) { js->tok = TOK_EOF; return js->tok; }
  
  #define TOK(T, LEN) { js->tok = T; js->tlen = (LEN); break; }
  #define LOOK(OFS, CH) js->toff + OFS < js->clen && buf[OFS] == CH
  
  switch (buf[0]) {
    case '?': if (LOOK(1, '?')) TOK(TOK_NULLISH, 2); if (LOOK(1, '.')) TOK(TOK_OPTIONAL_CHAIN, 2); TOK(TOK_Q, 1);
    case ':': TOK(TOK_COLON, 1);
    case '(': TOK(TOK_LPAREN, 1);
    case ')': TOK(TOK_RPAREN, 1);
    case '{': TOK(TOK_LBRACE, 1);
    case '}': TOK(TOK_RBRACE, 1);
    case '[': TOK(TOK_LBRACKET, 1);
    case ']': TOK(TOK_RBRACKET, 1);
    case ';': TOK(TOK_SEMICOLON, 1);
    case ',': TOK(TOK_COMMA, 1);
    case '!': if (LOOK(1, '=') && LOOK(2, '=')) TOK(TOK_NE, 3); if (LOOK(1, '=')) TOK(TOK_NE, 2); TOK(TOK_NOT, 1);
    case '.': if (LOOK(1, '.') && LOOK(2, '.')) TOK(TOK_REST, 3); TOK(TOK_DOT, 1);
    case '~': TOK(TOK_TILDA, 1);
    case '-': if (LOOK(1, '-')) TOK(TOK_POSTDEC, 2); if (LOOK(1, '=')) TOK(TOK_MINUS_ASSIGN, 2); TOK(TOK_MINUS, 1);
    case '+': if (LOOK(1, '+')) TOK(TOK_POSTINC, 2); if (LOOK(1, '=')) TOK(TOK_PLUS_ASSIGN, 2); TOK(TOK_PLUS, 1);
    case '*': if (LOOK(1, '*')) TOK(TOK_EXP, 2); if (LOOK(1, '=')) TOK(TOK_MUL_ASSIGN, 2); TOK(TOK_MUL, 1);
    case '/': if (LOOK(1, '=')) TOK(TOK_DIV_ASSIGN, 2); TOK(TOK_DIV, 1);
    case '%': if (LOOK(1, '=')) TOK(TOK_REM_ASSIGN, 2); TOK(TOK_REM, 1);
    case '&': if (LOOK(1, '&')) TOK(TOK_LAND, 2); if (LOOK(1, '=')) TOK(TOK_AND_ASSIGN, 2); TOK(TOK_AND, 1);
    case '|': if (LOOK(1, '|')) TOK(TOK_LOR, 2); if (LOOK(1, '=')) TOK(TOK_OR_ASSIGN, 2); TOK(TOK_OR, 1);
    case '=': if (LOOK(1, '=') && LOOK(2, '=')) TOK(TOK_EQ, 3); if (LOOK(1, '=')) TOK(TOK_EQ, 2); if (LOOK(1, '>')) TOK(TOK_ARROW, 2); TOK(TOK_ASSIGN, 1);
    case '<': if (LOOK(1, '<') && LOOK(2, '=')) TOK(TOK_SHL_ASSIGN, 3); if (LOOK(1, '<')) TOK(TOK_SHL, 2); if (LOOK(1, '=')) TOK(TOK_LE, 2); TOK(TOK_LT, 1);
    case '>': if (LOOK(1, '>') && LOOK(2, '=')) TOK(TOK_SHR_ASSIGN, 3); if (LOOK(1, '>')) TOK(TOK_SHR, 2); if (LOOK(1, '=')) TOK(TOK_GE, 2); TOK(TOK_GT, 1);
    case '^': if (LOOK(1, '=')) TOK(TOK_XOR_ASSIGN, 2); TOK(TOK_XOR, 1);
    case '`':
      js->tlen++;
      while (js->toff + js->tlen < js->clen && buf[js->tlen] != '`') {
        uint8_t increment = 1;
        if (buf[js->tlen] == '\\') {
          if (js->toff + js->tlen + 2 > js->clen) break;
          increment = 2;
        }
        js->tlen += increment;
      }
      if (buf[js->tlen] == '`') js->tok = TOK_TEMPLATE, js->tlen++;
      break;
    case '"': case '\'':
      js->tlen++;
      while (js->toff + js->tlen < js->clen && buf[js->tlen] != buf[0]) {
        uint8_t increment = 1;
        if (buf[js->tlen] == '\\') {
          if (js->toff + js->tlen + 2 > js->clen) break;
          increment = 2;
          if (buf[js->tlen + 1] == 'x') {
            if (js->toff + js->tlen + 4 > js->clen) break;
            increment = 4;
          }
        }
        js->tlen += increment;
      }
      if (buf[0] == buf[js->tlen]) js->tok = TOK_STRING, js->tlen++;
      break;
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9': {
      if ((js->flags & F_STRICT) && buf[0] == '0' && js->toff + 1 < js->clen && 
          is_digit(buf[1]) && buf[1] != 'x' && buf[1] != 'X' && buf[1] != 'b' && buf[1] != 'B' && buf[1] != 'o' && buf[1] != 'O') {
        js->tok = TOK_ERR;
        js->tlen = 1;
        break;
      }
      
      char *end;
      double value = 0;
      jsoff_t numlen = 0;
      
      if (buf[0] == '0' && js->toff + 2 < js->clen) {
        if (buf[1] == 'b' || buf[1] == 'B') {
          numlen = 2;
          while (js->toff + numlen < js->clen && (buf[numlen] == '0' || buf[numlen] == '1' || buf[numlen] == '_')) {
            if (buf[numlen] != '_') value = value * 2 + (buf[numlen] - '0');
            numlen++;
          }
          js->tval = tov(value);
        } else if (buf[1] == 'o' || buf[1] == 'O') {
          numlen = 2;
          while (js->toff + numlen < js->clen && ((buf[numlen] >= '0' && buf[numlen] <= '7') || buf[numlen] == '_')) {
            if (buf[numlen] != '_') value = value * 8 + (buf[numlen] - '0');
            numlen++;
          }
          js->tval = tov(value);
        } else if (buf[1] == 'x' || buf[1] == 'X') {
          char clean[64];
          jsoff_t ci = 0;
          numlen = 0;
          while (js->toff + numlen < js->clen && (is_xdigit(buf[numlen]) || buf[numlen] == 'x' || buf[numlen] == 'X' || buf[numlen] == '_') && ci < 63) {
            if (buf[numlen] != '_') clean[ci++] = buf[numlen];
            numlen++;
          }
          clean[ci] = '\0';
          js->tval = tov(strtod(clean, &end));
        } else {
          char clean[64];
          jsoff_t ci = 0;
          numlen = 0;
          while (js->toff + numlen < js->clen && (is_digit(buf[numlen]) || buf[numlen] == '.' || buf[numlen] == 'e' || buf[numlen] == 'E' || buf[numlen] == '+' || buf[numlen] == '-' || buf[numlen] == '_') && ci < 63) {
            if (numlen > 0 && (buf[numlen] == '+' || buf[numlen] == '-') && buf[numlen-1] != 'e' && buf[numlen-1] != 'E') break;
            if (buf[numlen] != '_') clean[ci++] = buf[numlen];
            numlen++;
          }
          clean[ci] = '\0';
          js->tval = tov(strtod(clean, &end));
        }
      } else {
        char clean[64];
        jsoff_t ci = 0;
        numlen = 0;
        while (js->toff + numlen < js->clen && (is_digit(buf[numlen]) || buf[numlen] == '.' || buf[numlen] == 'e' || buf[numlen] == 'E' || buf[numlen] == '+' || buf[numlen] == '-' || buf[numlen] == '_') && ci < 63) {
          if (numlen > 0 && (buf[numlen] == '+' || buf[numlen] == '-') && buf[numlen-1] != 'e' && buf[numlen-1] != 'E') break;
          if (buf[numlen] != '_') clean[ci++] = buf[numlen];
          numlen++;
        }
        clean[ci] = '\0';
        js->tval = tov(strtod(clean, &end));
      }
      
      if (js->toff + numlen < js->clen && buf[numlen] == 'n') {
        js->tok = TOK_BIGINT;
        js->tlen = numlen + 1;
      } else {
        TOK(TOK_NUMBER, numlen);
      }
      break;
    }
    default: js->tok = parseident(buf, js->clen - js->toff, &js->tlen); break;
  }
  
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

void js_mkscope(struct js *js) {
  assert((js->flags & F_NOEXEC) == 0);
  if (global_scope_stack == NULL) utarray_new(global_scope_stack, &jsoff_icd);
  jsoff_t prev = (jsoff_t) vdata(js->scope);
  utarray_push_back(global_scope_stack, &prev);
  js->scope = mkobj(js, prev);
}

void js_delscope(struct js *js) {
  if (global_scope_stack && utarray_len(global_scope_stack) > 0) {
    jsoff_t *prev = (jsoff_t *)utarray_back(global_scope_stack);
    js->scope = mkval(T_OBJ, *prev);
    utarray_pop_back(global_scope_stack);
  } else {
    js->scope = upper(js, js->scope);
  }
}

static void mkscope(struct js *js) { js_mkscope(js); }
static void delscope(struct js *js) { js_delscope(js); }

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

static inline bool push_call_frame(const char *filename, const char *function_name, int line, int col) {
  if (global_call_stack.depth >= global_call_stack.capacity) {
    int new_capacity = global_call_stack.capacity == 0 ? 32 : global_call_stack.capacity * 2;
    call_frame_t *new_stack = (call_frame_t *) realloc(global_call_stack.frames, new_capacity * sizeof(call_frame_t));
    if (!new_stack) return false;
    global_call_stack.frames = new_stack;
    global_call_stack.capacity = new_capacity;
  }
  
  global_call_stack.frames[global_call_stack.depth].filename = filename;
  global_call_stack.frames[global_call_stack.depth].function_name = function_name;
  global_call_stack.frames[global_call_stack.depth].line = line;
  global_call_stack.frames[global_call_stack.depth].col = col;
  global_call_stack.depth++;
  
  return true;
}

static inline void pop_call_frame() {
  if (global_call_stack.depth > 0) {
    global_call_stack.depth--;
  }
}

static jsval_t js_block(struct js *js, bool create_scope) {
  jsval_t res = js_mkundef();
  if (create_scope) mkscope(js);
  js->consumed = 1;
  uint8_t peek;
  while ((peek = next(js)) != TOK_EOF && peek != TOK_RBRACE && !is_err(res)) {
    uint8_t t = js->tok;
    res = js_stmt(js);
    if (!is_err(res) && t != TOK_LBRACE && t != TOK_IF && t != TOK_WHILE &&
        t != TOK_DO && t != TOK_FUNC && t != TOK_FOR && t != TOK_IMPORT && 
        t != TOK_EXPORT && t != TOK_SEMICOLON &&
        js->tok != TOK_SEMICOLON && js->tok != TOK_RBRACE && js->tok != TOK_EOF) {
      res = js_mkerr(js, "; expected");
      break;
    }
    if (js->flags & (F_RETURN | F_THROW)) break;
  }
  if (js->tok == TOK_RBRACE) js->consumed = 1;
  if (create_scope) delscope(js);
  return res;
}

static inline jsoff_t lkp_inline(struct js *js, jsval_t obj, const char *buf, size_t len) {
  if (len <= 63) {
    jsoff_t cached = cache_lookup((jsoff_t)vdata(obj), buf, len);
    if (cached != 0) return cached;
  }
  
  jsoff_t off = loadoff(js, (jsoff_t) vdata(obj)) & ~(3U | CONSTMASK);
  while (off < js->brk && off != 0) {
    jsoff_t koff = loadoff(js, (jsoff_t) (off + sizeof(off)));
    jsoff_t klen = (loadoff(js, koff) >> 2) - 1;
    const char *p = (char *) &js->mem[koff + sizeof(koff)];
    if (streq(buf, len, p, klen)) {
      if (len <= 63) cache_property((jsoff_t)vdata(obj), buf, len, off);
      return off;
    }
    off = loadoff(js, off) & ~(3U | CONSTMASK);
  }
  return 0;
}

static jsoff_t lkp(struct js *js, jsval_t obj, const char *buf, size_t len) {
  return lkp_inline(js, obj, buf, len);
}

jsval_t js_get_proto(struct js *js, jsval_t obj) {
  uint8_t t = vtype(obj);

  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return js_mknull();
  jsval_t as_obj = (t == T_OBJ) ? obj : mkval(T_OBJ, vdata(obj));
  
  jsoff_t off = lkp(js, as_obj, "__proto__", 9);
  if (off == 0) {
    if (t == T_FUNC || t == T_ARR) return get_prototype_for_type(js, t);
    return js_mknull();
  }
  
  jsval_t val = resolveprop(js, mkval(T_PROP, off));
  uint8_t vt = vtype(val);
  
  return (vt == T_OBJ || vt == T_ARR || vt == T_FUNC) ? val : js_mknull();
}

static jsval_t get_proto(struct js *js, jsval_t obj) {
  return js_get_proto(js, obj);
}

void js_set_proto(struct js *js, jsval_t obj, jsval_t proto) {
  uint8_t t = vtype(obj);
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return;
  
  jsval_t as_obj = (t == T_OBJ) ? obj : mkval(T_OBJ, vdata(obj));
  jsval_t key = js_mkstr(js, "__proto__", 9);
  jsoff_t existing = lkp(js, as_obj, "__proto__", 9);
  if (existing > 0) {
    saveval(js, existing + sizeof(jsoff_t) * 2, proto);
  } else {
    mkprop(js, as_obj, key, proto, false);
  }
}

static void set_proto(struct js *js, jsval_t obj, jsval_t proto) {
  js_set_proto(js, obj, proto);
}

jsval_t js_get_ctor_proto(struct js *js, const char *name, size_t len) {
  jsoff_t ctor_off = lkp(js, js->scope, name, len);
  
  if (ctor_off == 0 && global_scope_stack) {
    int stack_len = utarray_len(global_scope_stack);
    for (int i = stack_len - 1; i >= 0 && ctor_off == 0; i--) {
      jsoff_t *scope_off = (jsoff_t *)utarray_eltptr(global_scope_stack, i);
      jsval_t scope = mkval(T_OBJ, *scope_off);
      ctor_off = lkp(js, scope, name, len);
    }
  } else if (ctor_off == 0) {
    for (jsval_t scope = upper(js, js->scope); vdata(scope) != 0 && ctor_off == 0; scope = upper(js, scope)) {
      ctor_off = lkp(js, scope, name, len);
    }
  }
  
  if (ctor_off == 0) return js_mknull();
  jsval_t ctor = resolveprop(js, mkval(T_PROP, ctor_off));
  if (vtype(ctor) != T_FUNC) return js_mknull();
  jsval_t ctor_obj = mkval(T_OBJ, vdata(ctor));
  jsoff_t proto_off = lkp(js, ctor_obj, "prototype", 9);
  
  if (proto_off == 0) return js_mknull();
  return resolveprop(js, mkval(T_PROP, proto_off));
}

static jsval_t get_ctor_proto(struct js *js, const char *name, size_t len) {
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
    default:        return js_mknull();
  }
}

static jsoff_t lkp_proto(struct js *js, jsval_t obj, const char *key, size_t len) {
  if (streq(key, len, "__proto__", 9)) {
    uint8_t t = vtype(obj);
    if (t == T_OBJ || t == T_ARR || t == T_FUNC) {
      jsval_t as_obj = (t == T_OBJ) ? obj : mkval(T_OBJ, vdata(obj));
      return lkp(js, as_obj, key, len);
    }
    return 0;
  }
  
  jsval_t cur = obj;
  int depth = 0;
  const int MAX_PROTO_DEPTH = 32;
  
  while (depth < MAX_PROTO_DEPTH) {
    uint8_t t = vtype(cur);
    
    if (t == T_OBJ || t == T_ARR || t == T_FUNC) {
      jsval_t as_obj = (t == T_OBJ) ? cur : mkval(T_OBJ, vdata(cur));
      jsoff_t off = lkp(js, as_obj, key, len);
      if (off != 0) return off;
      
      cur = get_proto(js, cur);
      if (vtype(cur) == T_NULL || vtype(cur) == T_UNDEF) break;
      depth++;
    } else if (t == T_STR || t == T_NUM || t == T_BOOL) {
      jsval_t proto = get_prototype_for_type(js, t);
      if (vtype(proto) == T_NULL || vtype(proto) == T_UNDEF) break;
      cur = proto;
      depth++;
    } else if (t == T_CFUNC) {
      jsval_t func_proto = get_ctor_proto(js, "Function", 8);
      if (vtype(func_proto) == T_OBJ || vtype(func_proto) == T_ARR || vtype(func_proto) == T_FUNC) {
        jsval_t as_obj = (vtype(func_proto) == T_OBJ) ? func_proto : mkval(T_OBJ, vdata(func_proto));
        jsoff_t off = lkp(js, as_obj, key, len);
        if (off != 0) return off;
      }
      break;
    }
    else break;
  }
  
  return 0;
}

static jsval_t try_dynamic_getter(struct js *js, jsval_t obj, const char *key, size_t key_len) {
  if (streq(key, key_len, "__getter", 8)) return js_mkundef();
  
  jsoff_t getter_off = lkp(js, obj, "__getter", 8);
  if (getter_off == 0) return js_mkundef();
  
  jsval_t getter_val = resolveprop(js, mkval(T_PROP, getter_off));
  if (vtype(getter_val) != T_CFUNC) return js_mkundef();
  
  js_getter_fn getter = (js_getter_fn)(void *)vdata(getter_val);
  jsval_t result = getter(js, obj, key, key_len);
  
  if (vtype(result) != T_UNDEF) {
    jsval_t key_str = js_mkstr(js, key, key_len);
    setprop(js, obj, key_str, result);
  }
  
  return result;
}

static jsval_t lookup(struct js *js, const char *buf, size_t len) {
  if (js->flags & F_NOEXEC) return 0;
  
  for (jsval_t scope = js->scope;;) {
    jsoff_t with_marker_off = lkp(js, scope, "__with_object__", 15);
    if (with_marker_off != 0) {
      jsval_t with_obj_val = resolveprop(js, mkval(T_PROP, with_marker_off));
      
      jsval_t with_obj = (
        vtype(with_obj_val) == T_OBJ ||
        vtype(with_obj_val) == T_ARR || 
        vtype(with_obj_val) == T_FUNC) ? 
        with_obj_val : mkval(T_OBJ, vdata(with_obj_val)
      );
      
      jsoff_t prop_off = lkp(js, with_obj, buf, len);
      if (prop_off != 0) {
        jsval_t key = js_mkstr(js, buf, len);
        if (is_err(key)) return key;
        return mkpropref((jsoff_t)vdata(with_obj), (jsoff_t)vdata(key));
      }
    }
    
    jsoff_t off = lkp(js, scope, buf, len);
    if (off != 0) return mkval(T_PROP, off);
    if (vdata(scope) == 0) break;
    scope = upper(js, scope);
  }
  
  if (global_scope_stack && utarray_len(global_scope_stack) > 0) {
    jsoff_t *root_off = (jsoff_t *)utarray_eltptr(global_scope_stack, 0);
    if (root_off && *root_off != 0) {
      jsval_t root_scope = mkval(T_OBJ, *root_off);
      jsoff_t off = lkp(js, root_scope, buf, len);
      if (off != 0) return mkval(T_PROP, off);
    }
  }
  
  if (js->flags & F_STRICT) {
    return js_mkerr(js, "ReferenceError: '%.*s' is not defined", (int) len, buf);
  }
  
  jsval_t global_scope = js->scope;
  while (vdata(upper(js, global_scope)) != 0) {
    global_scope = upper(js, global_scope);
  }
  
  jsval_t key = js_mkstr(js, buf, len);
  if (is_err(key)) return key;
  
  jsval_t undef = js_mkundef();
  return setprop(js, global_scope, key, undef);
}

static jsval_t resolveprop(struct js *js, jsval_t v) {
  if (vtype(v) == T_PROPREF) {
    jsoff_t obj_off = propref_obj(v);
    jsoff_t key_off = propref_key(v);
    jsval_t obj = mkval(T_OBJ, obj_off);
    jsval_t key = mkval(T_STR, key_off);
    jsoff_t len;
    const char *key_str = (const char *)&js->mem[vstr(js, key, &len)];
    jsoff_t prop_off = lkp(js, obj, key_str, len);
    if (prop_off == 0) return js_mkundef();
    return resolveprop(js, mkval(T_PROP, prop_off));
  }
  if (vtype(v) != T_PROP) return v;
  return resolveprop(js, loadval(js, (jsoff_t) (vdata(v) + sizeof(jsoff_t) * 2)));
}

static jsval_t assign(struct js *js, jsval_t lhs, jsval_t val) {
  if (vtype(lhs) == T_PROPREF) {
    jsoff_t obj_off = propref_obj(lhs);
    jsoff_t key_off = propref_key(lhs);
    jsval_t obj = mkval(T_OBJ, obj_off);
    jsval_t key = mkval(T_STR, key_off);
    return setprop(js, obj, key, val);
  }
  
  jsoff_t propoff = (jsoff_t) vdata(lhs);
  if (is_const_prop(js, propoff)) {
    return js_mkerr(js, "assignment to constant");
  }
  
  if (js->flags & F_STRICT) {
    jsval_t prop_val = loadval(js, (jsoff_t) (propoff + sizeof(jsoff_t) * 2));
    uint8_t prop_type = vtype(prop_val);
    
    if (prop_type == T_STR || prop_type == T_NUM || prop_type == T_BOOL || 
        prop_type == T_NULL || prop_type == T_UNDEF) {
      return js_mkerr(js, "TypeError: cannot set property on primitive value in strict mode");
    }
  }
  
  saveval(js, (jsoff_t) ((vdata(lhs) & ~3U) + sizeof(jsoff_t) * 2), val);
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
    
    char *new_buffer = (char *)ANT_GC_MALLOC(new_capacity);
    if (!new_buffer) return false;
    
    if (sb->size > 0) memcpy(new_buffer, sb->buffer, sb->size);
    if (sb->is_dynamic) ANT_GC_FREE(sb->buffer);
    
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
  if (sb->is_dynamic && sb->buffer) ANT_GC_FREE(sb->buffer);
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
  char keybuf[32];
  const char *keystr;
  size_t keylen;
  if (vtype(key_val) == T_NUM) {
    snprintf(keybuf, sizeof(keybuf), "%.0f", tod(key_val));
    keystr = keybuf;
    keylen = strlen(keybuf);
  } else if (vtype(key_val) == T_STR) {
    jsoff_t slen;
    jsoff_t off = vstr(js, key_val, &slen);
    keystr = (char *) &js->mem[off];
    keylen = slen;
  } else {
    return js_mkerr(js, "invalid index type");
  }
  if ((vtype(obj) == T_STR || vtype(obj) == T_ARR) && streq(keystr, keylen, "length", 6)) {
    if (vtype(obj) == T_STR) {
      return tov(offtolen(loadoff(js, (jsoff_t) vdata(obj))));
    }
  }
  if (vtype(obj) == T_STR) {
    if (vtype(key_val) != T_NUM) {
      return js_mkerr(js, "string indices must be numbers");
    }
    double idx_d = tod(key_val);
    if (idx_d < 0 || idx_d != (double)(long)idx_d) {
      return js_mkundef();
    }
    jsoff_t idx = (jsoff_t) idx_d;
    jsoff_t str_len = offtolen(loadoff(js, (jsoff_t) vdata(obj)));
    if (idx >= str_len) {
      return js_mkundef();
    }
    jsoff_t str_off = (jsoff_t) vdata(obj) + sizeof(jsoff_t);
    char ch[2] = {js->mem[str_off + idx], 0};
    return js_mkstr(js, ch, 1);
  }
  if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR) {
    return js_mkerr(js, "cannot index non-object");
  }
  jsoff_t off = lkp(js, obj, keystr, keylen);
  if (off == 0) {
    jsval_t key = js_mkstr(js, keystr, keylen);
    jsval_t prop = setprop(js, obj, key, js_mkundef());
    return prop;
  }
  return mkval(T_PROP, off);
}

static jsval_t do_dot_op(struct js *js, jsval_t l, jsval_t r) {
  const char *ptr = (char *) &js->code[coderefoff(r)];
  size_t plen = codereflen(r);
  
  if (vtype(r) != T_CODEREF) return js_mkerr(js, "ident expected");
  uint8_t t = vtype(l);
  
  if (t == T_STR && streq(ptr, plen, "length", 6)) {
    return tov(offtolen(loadoff(js, (jsoff_t) vdata(l))));
  }
  
  if (t == T_ARR && streq(ptr, plen, "length", 6)) {
    jsoff_t off = lkp(js, l, "length", 6);
    if (off == 0) return tov(0);
    return resolveprop(js, mkval(T_PROP, off));
  }
  
  if (t == T_STR || t == T_NUM || t == T_BOOL) {
    jsoff_t off = lkp_proto(js, l, ptr, plen);
    if (off != 0) {
      return resolveprop(js, mkval(T_PROP, off));
    }
    return js_mkundef();
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
    jsval_t func_obj = mkval(T_OBJ, vdata(l));
    jsoff_t off = lkp_proto(js, l, ptr, plen);
    if (off != 0) return mkval(T_PROP, off);
    if (streq(ptr, plen, "name", 4)) return js_mkstr(js, "", 0);
    jsval_t key = js_mkstr(js, ptr, plen);
    jsval_t prop = setprop(js, func_obj, key, js_mkundef());
    return prop;
  }
  
  if (t == T_CFUNC) {
    jsoff_t off = lkp_proto(js, l, ptr, plen);
    if (off != 0) return resolveprop(js, mkval(T_PROP, off));
    if (streq(ptr, plen, "name", 4)) return js_mkstr(js, "", 0);
    return js_mkundef();
  }
  
  if (t != T_OBJ && t != T_ARR) {
    return js_mkerr(js, "lookup in non-obj");
  }
  
  jsoff_t own_off = lkp(js, l, ptr, plen);
  if (own_off != 0) {
    return mkval(T_PROP, own_off);
  }
  
  jsval_t result = try_dynamic_getter(js, l, ptr, plen);
  if (vtype(result) != T_UNDEF) {
    own_off = lkp(js, l, ptr, plen);
    if (own_off != 0) return mkval(T_PROP, own_off);
  }
  
  jsoff_t proto_off = lkp_proto(js, l, ptr, plen);
  if (proto_off != 0) return resolveprop(js, mkval(T_PROP, proto_off));
  
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
  
  for (bool comma = false; next(js) != TOK_EOF; comma = true) {
    if (!comma && next(js) == TOK_RPAREN) break;
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

static jsval_t call_c(struct js *js,
  jsval_t (*fn)(struct js *, jsval_t *, int)) {
  int argc = 0;
  jsval_t target_this = peek_this();
  
  while (js->pos < js->clen) {
    if (next(js) == TOK_RPAREN) break;
    bool is_spread = (next(js) == TOK_REST);
    if (is_spread) js->consumed = 1;
    jsval_t arg = resolveprop(js, js_expr(js));
    if (is_spread && vtype(arg) == T_ARR) {
      jsoff_t len = arr_length(js, arg);
      for (jsoff_t i = 0; i < len; i++) {
        jsval_t elem = arr_get(js, arg, i);
        if (js->brk + sizeof(elem) > js->size) return js_mkerr(js, "call oom");
        js->size -= (jsoff_t) sizeof(elem);
        memcpy(&js->mem[js->size], &elem, sizeof(elem));
        argc++;
      }
    } else {
      if (js->brk + sizeof(arg) > js->size) return js_mkerr(js, "call oom");
      js->size -= (jsoff_t) sizeof(arg);
      memcpy(&js->mem[js->size], &arg, sizeof(arg));
      argc++;
    }
    if (next(js) == TOK_COMMA) js->consumed = 1;
  }
  
  jsval_t saved_this = js->this_val;
  js->this_val = target_this;
  reverse((jsval_t *) &js->mem[js->size], argc);
  jsval_t res = fn(js, (jsval_t *) &js->mem[js->size], argc);
  js->this_val = saved_this;
  setlwm(js);
  
  js->size += (jsoff_t) sizeof(jsval_t) * (jsoff_t) argc;
  return res;
}

static jsoff_t extract_default_param_value(const char *fn, jsoff_t fnlen, jsoff_t start_pos, jsoff_t *out_start, jsoff_t *out_len) {
  jsoff_t after_ident = skiptonext(fn, fnlen, start_pos);
  if (after_ident >= fnlen || fn[after_ident] != '=') {
    *out_start = 0;
    *out_len = 0;
    return after_ident;
  }
  
  jsoff_t default_start = skiptonext(fn, fnlen, after_ident + 1);
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
  return skiptonext(fn, fnlen, default_start + default_len);
}

static jsval_t call_js(struct js *js, const char *fn, jsoff_t fnlen, jsval_t closure_scope) {
  jsoff_t fnpos = 1;
  jsval_t saved_scope = js->scope;
  jsval_t target_this = peek_this();
  jsoff_t parent_scope_offset;
  
  if (vtype(closure_scope) == T_OBJ) {
    parent_scope_offset = (jsoff_t) vdata(closure_scope);
  } else {
    parent_scope_offset = (jsoff_t) vdata(js->scope);
  }
  
  if (global_scope_stack == NULL) utarray_new(global_scope_stack, &jsoff_icd);
  utarray_push_back(global_scope_stack, &parent_scope_offset);
  jsval_t function_scope = mkobj(js, parent_scope_offset);
  
  const char *caller_code = js->code;
  jsoff_t caller_clen = js->clen;
  jsoff_t caller_pos = js->pos;
  
  jsval_t args[64];
  int argc = 0;
  caller_pos = skiptonext(caller_code, caller_clen, caller_pos);
  while (caller_pos < caller_clen && caller_code[caller_pos] != ')' && argc < 64) {
    bool is_spread = (caller_code[caller_pos] == '.' && caller_pos + 2 < caller_clen &&
                      caller_code[caller_pos + 1] == '.' && caller_code[caller_pos + 2] == '.');
    if (is_spread) caller_pos += 3;
    js->pos = caller_pos;
    js->consumed = 1;
    jsval_t arg = resolveprop(js, js_expr(js));
    caller_pos = js->pos;
    if (is_spread && vtype(arg) == T_ARR) {
      jsoff_t len = arr_length(js, arg);
      for (jsoff_t i = 0; i < len && argc < 64; i++) {
        args[argc++] = arr_get(js, arg, i);
      }
    } else {
      args[argc++] = arg;
    }
    caller_pos = skiptonext(caller_code, caller_clen, caller_pos);
    if (caller_pos < caller_clen && caller_code[caller_pos] == ',') caller_pos++;
    caller_pos = skiptonext(caller_code, caller_clen, caller_pos);
  }
  js->pos = caller_pos;
  
  int argi = 0;
  bool has_rest = false;
  jsoff_t rest_param_start = 0, rest_param_len = 0;
  
  while (fnpos < fnlen) {
    fnpos = skiptonext(fn, fnlen, fnpos);
    if (fnpos < fnlen && fn[fnpos] == ')') break;
    
    bool is_rest = false;
    if (fnpos + 3 < fnlen && fn[fnpos] == '.' && fn[fnpos + 1] == '.' && fn[fnpos + 2] == '.') {
      is_rest = true;
      has_rest = true;
      fnpos += 3;
      fnpos = skiptonext(fn, fnlen, fnpos);
    }
    
    jsoff_t identlen = 0;
    uint8_t tok = parseident(&fn[fnpos], fnlen - fnpos, &identlen);
    if (tok != TOK_IDENTIFIER) break;
    
    if (is_rest) {
      rest_param_start = fnpos;
      rest_param_len = identlen;
      fnpos = skiptonext(fn, fnlen, fnpos + identlen);
      break;
    }
    
    jsoff_t param_name_pos = fnpos;
    jsoff_t default_start = 0, default_len = 0;
    fnpos = extract_default_param_value(fn, fnlen, fnpos + identlen, &default_start, &default_len);
    
    jsval_t v;
    if (argi < argc) {
      v = args[argi++];
    } else if (default_len > 0) {
      const char *saved_code = js->code;
      jsoff_t saved_clen = js->clen, saved_pos = js->pos;
      js->code = &fn[default_start];
      js->clen = default_len;
      js->pos = 0;
      js->consumed = 1;
      v = js_expr(js);
      js->code = saved_code;
      js->clen = saved_clen;
      js->pos = saved_pos;
    } else {
      v = js_mkundef();
    }
    setprop(js, function_scope, js_mkstr(js, &fn[param_name_pos], identlen), v);
    if (fnpos < fnlen && fn[fnpos] == ',') fnpos++;
  }
  
  if (has_rest && rest_param_len > 0) {
    jsval_t rest_array = mkarr(js);
    if (!is_err(rest_array)) {
      jsoff_t idx = 0;
      while (argi < argc) {
        char idxstr[16];
        snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) idx);
        jsval_t key = js_mkstr(js, idxstr, strlen(idxstr));
        setprop(js, rest_array, key, args[argi++]);
        idx++;
      }
      jsval_t len_key = js_mkstr(js, "length", 6);
      setprop(js, rest_array, len_key, tov((double) idx));
      rest_array = mkval(T_ARR, vdata(rest_array));
      setprop(js, function_scope, js_mkstr(js, &fn[rest_param_start], rest_param_len), rest_array);
    }
  }
  
  js->scope = function_scope;
  if (fnpos < fnlen && fn[fnpos] == ')') fnpos++;
  fnpos = skiptonext(fn, fnlen, fnpos);
  if (fnpos < fnlen && fn[fnpos] == '{') fnpos++;
  size_t n = fnlen - fnpos - 1U;
  js->this_val = target_this;
  js->flags = F_CALL;
  
  jsval_t res = js_eval(js, &fn[fnpos], n);
  if (!is_err(res) && !(js->flags & F_RETURN)) res = js_mkundef();
  if (global_scope_stack && utarray_len(global_scope_stack) > 0)  utarray_pop_back(global_scope_stack);
  
  js->scope = saved_scope;
  return res;
}

static jsval_t call_js_with_args(struct js *js, jsval_t func, jsval_t *args, int nargs) {
  if (vtype(func) == T_CFUNC) {
    jsval_t (*fn)(struct js *, jsval_t *, int) = (jsval_t(*)(struct js *, jsval_t *, int)) vdata(func);
    return fn(js, args, nargs);
  }
  
  if (vtype(func) != T_FUNC) return js_mkerr(js, "not a function");
  jsval_t func_obj = mkval(T_OBJ, vdata(func));

  jsoff_t native_off = lkp(js, func_obj, "__native_func", 13);
  if (native_off != 0) {
    jsval_t native_val = resolveprop(js, mkval(T_PROP, native_off));
    if (vtype(native_val) == T_CFUNC) {
      jsoff_t this_off = lkp(js, func_obj, "__this", 6);
      jsval_t bound_this = js_mkundef();
      if (this_off != 0) {
        bound_this = resolveprop(js, mkval(T_PROP, this_off));
      }
      
      jsval_t saved_this = js->this_val;
      if (vtype(bound_this) != JS_UNDEF) {
        push_this(bound_this);
        js->this_val = bound_this;
      }
      
      jsval_t (*fn)(struct js *, jsval_t *, int) = (jsval_t(*)(struct js *, jsval_t *, int)) vdata(native_val);
      jsval_t result = fn(js, args, nargs);
      
      if (vtype(bound_this) != JS_UNDEF) {
        pop_this();
        js->this_val = saved_this;
      }
      
      return result;
    }
  }
  
  jsoff_t code_off = lkp(js, func_obj, "__code", 6);
  if (code_off == 0) return js_mkerr(js, "function has no code");
  jsval_t code_val = resolveprop(js, mkval(T_PROP, code_off));
  if (vtype(code_val) != T_STR) return js_mkerr(js, "function code not string");
  
  jsoff_t fnlen, fnoff = vstr(js, code_val, &fnlen);
  const char *fn = (const char *) (&js->mem[fnoff]);
  
  jsval_t closure_scope = js_mkundef();
  jsoff_t scope_off = lkp(js, func_obj, "__scope", 7);
  if (scope_off != 0) {
    closure_scope = resolveprop(js, mkval(T_PROP, scope_off));
  }
  
  jsoff_t this_off = lkp(js, func_obj, "__this", 6);
  if (this_off != 0) {
    jsval_t captured_this = resolveprop(js, mkval(T_PROP, this_off));
    pop_this();
    push_this(captured_this);
  }
  
  return call_js_code_with_args(js, fn, fnlen, closure_scope, args, nargs);
}

static jsval_t call_js_code_with_args(struct js *js, const char *fn, jsoff_t fnlen, jsval_t closure_scope, jsval_t *args, int nargs) {
  jsoff_t parent_scope_offset;
  if (vtype(closure_scope) == T_OBJ) {
    parent_scope_offset = (jsoff_t) vdata(closure_scope);
  } else {
    parent_scope_offset = (jsoff_t) vdata(js->scope);
  }
  
  jsval_t saved_scope = js->scope;
  if (global_scope_stack == NULL) utarray_new(global_scope_stack, &jsoff_icd);
  utarray_push_back(global_scope_stack, &parent_scope_offset);
  jsval_t function_scope = mkobj(js, parent_scope_offset);
  js->scope = function_scope;
  
  jsoff_t fnpos = 1;
  int arg_idx = 0;
  bool has_rest = false;
  jsoff_t rest_param_start = 0, rest_param_len = 0;
  
  while (fnpos < fnlen) {
    fnpos = skiptonext(fn, fnlen, fnpos);
    if (fnpos < fnlen && fn[fnpos] == ')') break;
    
    bool is_rest = false;
    if (fnpos + 3 < fnlen && fn[fnpos] == '.' && fn[fnpos + 1] == '.' && fn[fnpos + 2] == '.') {
      is_rest = true;
      has_rest = true;
      fnpos += 3;
      fnpos = skiptonext(fn, fnlen, fnpos);
    }
    
    jsoff_t identlen = 0;
    uint8_t tok = parseident(&fn[fnpos], fnlen - fnpos, &identlen);
    if (tok != TOK_IDENTIFIER) break;
    
    if (is_rest) {
      rest_param_start = fnpos;
      rest_param_len = identlen;
      fnpos = skiptonext(fn, fnlen, fnpos + identlen);
      break;
    }
    
    jsoff_t param_name_pos = fnpos;
    jsoff_t default_start = 0, default_len = 0;
    fnpos = extract_default_param_value(fn, fnlen, fnpos + identlen, &default_start, &default_len);
    
    jsval_t v;
    if (arg_idx < nargs) {
      v = args[arg_idx];
    } else if (default_len > 0) {
      const char *saved_code = js->code;
      jsoff_t saved_clen = js->clen, saved_pos = js->pos;
      uint8_t saved_consumed = js->consumed;
      js->code = &fn[default_start];
      js->clen = default_len;
      js->pos = 0;
      js->consumed = 1;
      v = js_expr(js);
      js->code = saved_code;
      js->clen = saved_clen;
      js->pos = saved_pos;
      js->consumed = saved_consumed;
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
        snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) idx);
        jsval_t key = js_mkstr(js, idxstr, strlen(idxstr));
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
  fnpos = skiptonext(fn, fnlen, fnpos);
  if (fnpos < fnlen && fn[fnpos] == '{') fnpos++;
  size_t body_len = fnlen - fnpos - 1;
  
  jsval_t saved_this = js->this_val;
  jsval_t target_this = peek_this();
  js->this_val = target_this;
  js->flags = F_CALL;
  
  jsval_t res = js_eval(js, &fn[fnpos], body_len);
  if (!is_err(res) && !(js->flags & F_RETURN)) res = js_mkundef();
  
  js->this_val = saved_this;
  if (global_scope_stack && utarray_len(global_scope_stack) > 0) utarray_pop_back(global_scope_stack);
  
  js->scope = saved_scope;  
  return res;
}

static jsval_t do_call_op(struct js *js, jsval_t func, jsval_t args) {
  if (vtype(args) != T_CODEREF) return js_mkerr(js, "bad call");
  if (vtype(func) != T_FUNC && vtype(func) != T_CFUNC)
    return js_mkerr(js, "calling non-function");
  
  jsval_t target_this = peek_this();
  if (vtype(func) == T_FUNC && vtype(target_this) == T_OBJ) {
    jsoff_t proto_check = lkp(js, target_this, "__proto__", 9);
    if (proto_check == 0) {
      jsval_t func_obj = mkval(T_OBJ, vdata(func));
      jsoff_t proto_off = lkp(js, func_obj, "prototype", 9);
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
  js->pos = skiptonext(js->code, js->clen, 0);
  uint8_t tok = js->tok, flags = js->flags;
  jsoff_t nogc = js->nogc;
  jsval_t res = js_mkundef();
  
  if (vtype(func) == T_FUNC) {
    jsval_t func_obj = mkval(T_OBJ, vdata(func));
    jsoff_t native_off = lkp(js, func_obj, "__native_func", 13);
    if (native_off != 0) {
      jsval_t native_val = resolveprop(js, mkval(T_PROP, native_off));
      if (vtype(native_val) == T_CFUNC) {
        jsval_t saved_func = js->current_func;
        js->current_func = func;
        res = call_c(js, (jsval_t(*)(struct js *, jsval_t *, int)) vdata(native_val));
        js->current_func = saved_func;
      }
    } else {
      jsoff_t code_off = lkp(js, func_obj, "__code", 6);
      if (code_off == 0) return js_mkerr(js, "function has no code");
      jsval_t code_val = resolveprop(js, mkval(T_PROP, code_off));
      if (vtype(code_val) != T_STR) return js_mkerr(js, "function code not string");
      jsval_t closure_scope = js_mkundef();
      jsoff_t scope_off = lkp(js, func_obj, "__scope", 7);
      if (scope_off != 0) {
        closure_scope = resolveprop(js, mkval(T_PROP, scope_off));
      }
      jsoff_t fnlen, fnoff = vstr(js, code_val, &fnlen);
      const char *code_str = (const char *) (&js->mem[fnoff]);
      bool is_async = false;
      jsoff_t async_off = lkp(js, func_obj, "__async", 7);
      if (async_off != 0) {
        jsval_t async_val = resolveprop(js, mkval(T_PROP, async_off));
        is_async = vtype(async_val) == T_BOOL && vdata(async_val) == 1;
      }
      
      jsval_t captured_this = js_mkundef();
      bool is_arrow = false;
      jsoff_t this_off = lkp(js, func_obj, "__this", 6);
      if (this_off != 0) {
        captured_this = resolveprop(js, mkval(T_PROP, this_off));
        is_arrow = true;
      }
      
      if (fnlen == 16 && memcmp(code_str, "__builtin_Object", 16) == 0) {
        res = call_c(js, builtin_Object);
      } else {
        int call_line = 0, call_col = 0;
        get_line_col(code, pos, &call_line, &call_col);
        
        const char *func_name = NULL;
        jsoff_t name_off = lkp(js, func_obj, "name", 4);
        if (name_off != 0) {
          jsval_t name_val = resolveprop(js, mkval(T_PROP, name_off));
          if (vtype(name_val) == T_STR) {
            jsoff_t name_len, name_offset = vstr(js, name_val, &name_len);
            func_name = (const char *)&js->mem[name_offset];
          }
        }
        
        push_call_frame(
          js->filename,
          func_name ? func_name : "<anonymous>", 
          call_line, call_col
        );
        
        jsval_t saved_func = js->current_func;
        js->current_func = func;
        js->nogc = (jsoff_t) (fnoff - sizeof(jsoff_t));
        
        if (is_arrow) {
          pop_this();
          push_this(captured_this);
        }
        
        jsoff_t count_off = lkp(js, func_obj, "__field_count", 13);
        if (count_off == 0 || vtype(target_this) != T_OBJ) goto skip_fields;
        
        jsval_t count_val = resolveprop(js, mkval(T_PROP, count_off));
        if (vtype(count_val) != T_NUM) goto skip_fields;
        
        int field_count = (int)tod(count_val);
        jsoff_t src_off = lkp(js, func_obj, "__source", 8);
        jsoff_t fields_off = lkp(js, func_obj, "__fields", 8);
        if (src_off == 0 || fields_off == 0) goto skip_fields;
        
        jsval_t src_val = resolveprop(js, mkval(T_PROP, src_off));
        jsval_t fields_meta = resolveprop(js, mkval(T_PROP, fields_off));
        if (vtype(src_val) != T_STR || vtype(fields_meta) != T_STR) goto skip_fields;
        
        jsoff_t src_len, src_ptr_off = vstr(js, src_val, &src_len);
        const char *source = (const char *)(&js->mem[src_ptr_off]);
        
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
            jsoff_t init_len = init_end - init_start;
            const char *init_code = &source[init_start];
            
            const char *saved_code = js->code;
            jsoff_t saved_clen = js->clen;
            jsoff_t saved_pos = js->pos;
            uint8_t saved_tok = js->tok;
            uint8_t saved_consumed = js->consumed;
            
            js->code = init_code;
            js->clen = init_len;
            js->pos = 0;
            js->consumed = 1;
            
            field_val = js_expr(js);
            field_val = resolveprop(js, field_val);
            
            js->code = saved_code;
            js->clen = saved_clen;
            js->pos = saved_pos;
            js->tok = saved_tok;
            js->consumed = saved_consumed;
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
          res = start_async_in_coroutine(js, code_str, fnlen, closure_scope, NULL, 0);
          pop_call_frame();
        } else {
          res = call_js(js, code_str, fnlen, closure_scope);
          pop_call_frame();
        }
        
        js->current_func = saved_func;
      }
    }
  } else {
    res = call_c(js, (jsval_t(*)(struct js *, jsval_t *, int)) vdata(func));
  }
  
  js->code = code, js->clen = clen, js->pos = pos;
  js->flags = flags, js->tok = tok, js->nogc = nogc;
  js->consumed = 1;
  return res;
}

static jsval_t do_op(struct js *js, uint8_t op, jsval_t lhs, jsval_t rhs) {
  if (js->flags & F_NOEXEC) return 0;
  jsval_t l = resolveprop(js, lhs), r = resolveprop(js, rhs);
  setlwm(js);
  
  if (is_err(l)) return l;
  if (is_err(r)) return r;
  if (is_assign(op) && vtype(lhs) != T_PROP && vtype(lhs) != T_PROPREF) return js_mkerr(js, "bad lhs");
  
  switch (op) {
    case TOK_TYPEOF:     return js_mkstr(js, typestr(vtype(r)), strlen(typestr(vtype(r))));
    case TOK_VOID:       return js_mkundef();
    case TOK_INSTANCEOF: return do_instanceof(js, l, r);
    case TOK_IN:         return do_in(js, l, r);
    case TOK_CALL:       return do_call_op(js, l, r);
    case TOK_BRACKET:    return do_bracket_op(js, l, rhs);
    case TOK_ASSIGN:     return assign(js, lhs, r);
    case TOK_POSTINC: {
      if (vtype(lhs) != T_PROP) return js_mkerr(js, "bad lhs for ++");
      do_assign_op(js, TOK_PLUS_ASSIGN, lhs, tov(1)); return l;
    }
    case TOK_POSTDEC: {
      if (vtype(lhs) != T_PROP) return js_mkerr(js, "bad lhs for --");
      do_assign_op(js, TOK_MINUS_ASSIGN, lhs, tov(1)); return l;
    }
    case TOK_NOT:     return mkval(T_BOOL, !js_truthy(js, r));
    case TOK_UMINUS:
      if (vtype(r) == T_BIGINT) return bigint_neg(js, r);
      break;
    case TOK_UPLUS:
      if (vtype(r) == T_BIGINT) return js_mkerr(js, "Cannot convert BigInt to number");
      break;
  }
  if (is_assign(op))    return do_assign_op(js, op, lhs, r);
  if (op == TOK_EQ || op == TOK_NE) {
    bool eq = false;
    if (vtype(l) == vtype(r)) {
      if (vtype(l) == T_STR) {
        jsoff_t n1, off1 = vstr(js, l, &n1);
        jsoff_t n2, off2 = vstr(js, r, &n2);
        eq = n1 == n2 && memcmp(&js->mem[off1], &js->mem[off2], n1) == 0;
      } else if (vtype(l) == T_NUM) {
        eq = tod(l) == tod(r);
      } else if (vtype(l) == T_BOOL) {
        eq = vdata(l) == vdata(r);
      } else if (vtype(l) == T_BIGINT) {
        eq = bigint_compare(js, l, r) == 0;
      } else {
        eq = vdata(l) == vdata(r);
      }
    }
    return mkval(T_BOOL, op == TOK_EQ ? eq : !eq);
  }
  if (vtype(l) == T_BIGINT || vtype(r) == T_BIGINT) {
    if (vtype(l) != T_BIGINT || vtype(r) != T_BIGINT) {
      return js_mkerr(js, "Cannot mix BigInt and other types");
    }
    switch (op) {
      case TOK_PLUS:  return bigint_add(js, l, r);
      case TOK_MINUS: return bigint_sub(js, l, r);
      case TOK_MUL:   return bigint_mul(js, l, r);
      case TOK_DIV:   return bigint_div(js, l, r);
      case TOK_REM:   return bigint_mod(js, l, r);
      case TOK_LT:    return mkval(T_BOOL, bigint_compare(js, l, r) < 0);
      case TOK_GT:    return mkval(T_BOOL, bigint_compare(js, l, r) > 0);
      case TOK_LE:    return mkval(T_BOOL, bigint_compare(js, l, r) <= 0);
      case TOK_GE:    return mkval(T_BOOL, bigint_compare(js, l, r) >= 0);
      default: return js_mkerr(js, "unsupported BigInt operation");
    }
  }
  if (op == TOK_PLUS && (vtype(l) == T_STR || vtype(r) == T_STR || vtype(l) == T_ARR || vtype(r) == T_ARR)) {
    jsval_t l_str = l, r_str = r;
    
    if (vtype(l) == T_ARR) {
      char buf[1024];
      size_t len = array_to_string(js, l, buf, sizeof(buf));
      l_str = js_mkstr(js, buf, len);
      if (is_err(l_str)) return l_str;
    } else if (vtype(l) != T_STR) {
      const char *str = js_str(js, l);
      l_str = js_mkstr(js, str, strlen(str));
      if (is_err(l_str)) return l_str;
    }
    
    if (vtype(r) == T_ARR) {
      char buf[1024];
      size_t len = array_to_string(js, r, buf, sizeof(buf));
      r_str = js_mkstr(js, buf, len);
      if (is_err(r_str)) return r_str;
    } else if (vtype(r) != T_STR) {
      const char *str = js_str(js, r);
      r_str = js_mkstr(js, str, strlen(str));
      if (is_err(r_str)) return r_str;
    }
    
    return do_string_op(js, op, l_str, r_str);
  }
  if (vtype(l) == T_STR && vtype(r) == T_STR) {
    return do_string_op(js, op, l, r);
  }
  
  double a = 0.0, b = 0.0;
  
  if (vtype(l) == T_NUM) {
    a = tod(l);
  } else if (vtype(l) == T_STR) {
    jsoff_t slen, off = vstr(js, l, &slen);
    char *endptr;
    char temp[256];
    size_t copy_len = slen < sizeof(temp) - 1 ? slen : sizeof(temp) - 1;
    memcpy(temp, &js->mem[off], copy_len);
    temp[copy_len] = '\0';
    a = strtod(temp, &endptr);
    if (endptr == temp || *endptr != '\0') a = NAN;
  } else if (vtype(l) == T_BOOL) {
    a = vdata(l) ? 1.0 : 0.0;
  } else if (vtype(l) == T_NULL) {
    a = 0.0;
  } else if (vtype(l) == T_UNDEF) {
    a = NAN;
  } else if (vtype(l) == T_ARR) {
    a = NAN;
  } else {
    a = NAN;
  }
  
  if (vtype(r) == T_NUM) {
    b = tod(r);
  } else if (vtype(r) == T_STR) {
    jsoff_t slen, off = vstr(js, r, &slen);
    char *endptr;
    char temp[256];
    size_t copy_len = slen < sizeof(temp) - 1 ? slen : sizeof(temp) - 1;
    memcpy(temp, &js->mem[off], copy_len);
    temp[copy_len] = '\0';
    b = strtod(temp, &endptr);
    if (endptr == temp || *endptr != '\0') b = NAN;
  } else if (vtype(r) == T_BOOL) {
    b = vdata(r) ? 1.0 : 0.0;
  } else if (vtype(r) == T_NULL) {
    b = 0.0;
  } else if (vtype(r) == T_UNDEF) {
    b = NAN;
  } else if (vtype(r) == T_ARR) {
    b = NAN;
  } else {
    b = NAN;
  }
  
  switch (op) {
    case TOK_DIV:     return tov(a / b);
    case TOK_REM:     return tov(a - b * ((double) (long) (a / b)));
    case TOK_MUL:     return tov(a * b);
    case TOK_PLUS:    return tov(a + b);
    case TOK_MINUS:   return tov(a - b);
    case TOK_EXP:     return tov(pow(a, b));
    case TOK_XOR:     return tov((double)((long) a ^ (long) b));
    case TOK_AND:     return tov((double)((long) a & (long) b));
    case TOK_OR:      return tov((double)((long) a | (long) b));
    case TOK_UMINUS:  return tov(-b);
    case TOK_UPLUS:   return r;
    case TOK_TILDA:   return tov((double)(~(long) b));
    case TOK_SHL:     return tov((double)((long) a << (long) b));
    case TOK_SHR:     return tov((double)((long) a >> (long) b));
    case TOK_DOT:     return do_dot_op(js, l, r);
    case TOK_OPTIONAL_CHAIN: return do_optional_chain_op(js, l, r);
    case TOK_LT:      return mkval(T_BOOL, a < b);
    case TOK_LE:      return mkval(T_BOOL, a <= b);
    case TOK_GT:      return mkval(T_BOOL, a > b);
    case TOK_GE:      return mkval(T_BOOL, a >= b);
    default:          return js_mkerr(js, "unknown op %d", (int) op);
  }
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
      uint8_t *out = &js->mem[js->brk + sizeof(jsoff_t)];
      size_t out_len = 0;
      
      if (js->brk + sizeof(jsoff_t) + part_len > js->size)
        return js_mkerr(js, "oom");
      
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
      
      if (brace_count != 0) return js_mkerr(js, "unclosed ${");
      
      const char *saved_code = js->code;
      jsoff_t saved_clen = js->clen;
      jsoff_t saved_pos = js->pos;
      uint8_t saved_tok = js->tok;
      uint8_t saved_consumed = js->consumed;
      
      js->code = (const char *)&in[expr_start];
      js->clen = n - expr_start;
      js->pos = 0;
      js->consumed = 1;
      
      jsval_t expr_result = js_expr(js);
      
      js->code = saved_code;
      js->clen = saved_clen;
      js->pos = saved_pos;
      js->tok = saved_tok;
      js->consumed = saved_consumed;
      
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
  
  const char *saved_code = js->code;
  jsoff_t saved_clen = js->clen, saved_pos = js->pos;
  uint8_t saved_tok = js->tok;
  
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
    
    uint8_t *out = &js->mem[js->brk + sizeof(jsoff_t)];
    size_t out_len = 0;
    if (js->brk + sizeof(jsoff_t) + (n - part_start) > js->size) return js_mkerr(js, "oom");
    
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
    if (brace_count != 0) return js_mkerr(js, "unclosed ${");
    
    const char *saved_code = js->code;
    jsoff_t saved_clen = js->clen, saved_pos = js->pos;
    uint8_t saved_tok = js->tok, saved_consumed = js->consumed;
    
    js->code = (const char *)&in[expr_start];
    js->clen = n - expr_start;
    js->pos = 0;
    js->consumed = 1;
    
    jsval_t expr_result = resolveprop(js, js_expr(js));
    
    js->code = saved_code;
    js->clen = saved_clen;
    js->pos = saved_pos;
    js->tok = saved_tok;
    js->consumed = saved_consumed;
    
    if (is_err(expr_result)) return expr_result;
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
  
  js->code = saved_code;
  js->clen = saved_clen;
  js->pos = saved_pos;
  js->tok = saved_tok;
  js->flags = saved_flags;
  js->consumed = 1;
  
  return result;
}

static jsval_t js_str_literal(struct js *js) {
  uint8_t *in = (uint8_t *) &js->code[js->toff];
  uint8_t *out = &js->mem[js->brk + sizeof(jsoff_t)];
  size_t n1 = 0, n2 = 0;
  if (js->brk + sizeof(jsoff_t) + js->tlen > js->size)
    return js_mkerr(js, "oom");
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
      } else if (in[n2 + 1] == 'x' && is_xdigit(in[n2 + 2]) &&
                 is_xdigit(in[n2 + 3])) {
        out[n1++] = (uint8_t) ((unhex(in[n2 + 2]) << 4U) | unhex(in[n2 + 3]));
        n2 += 2;
      } else if (in[n2 + 1] == '\\') {
        out[n1++] = '\\';
      } else {
        return js_mkerr(js, "bad str literal");
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
  return mkbigint(js, start, len, neg);
}

static jsval_t js_arr_literal(struct js *js) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  jsval_t arr = exe ? mkarr(js) : js_mkundef();
  if (is_err(arr)) return arr;
  
  js->consumed = 1;
  jsoff_t idx = 0;
  while (next(js) != TOK_RBRACKET) {
    jsval_t val = js_expr(js);
    if (exe) {
      if (is_err(val)) return val;
      char idxstr[16];
      snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) idx);
      jsval_t key = js_mkstr(js, idxstr, strlen(idxstr));
      jsval_t res = setprop(js, arr, key, resolveprop(js, val));
      if (is_err(res)) return res;
    }
    idx++;
    if (next(js) == TOK_RBRACKET) break;
    EXPECT(TOK_COMMA, );
  }
  
  EXPECT(TOK_RBRACKET, );
  if (exe) {
    jsval_t len_key = js_mkstr(js, "length", 6);
    jsval_t len_val = tov((double) idx);
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
    return js_mkerr(js, "unterminated regex");
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
  setprop(js, regexp_obj, js_mkstr(js, "source", 6), pattern);
  setprop(js, regexp_obj, js_mkstr(js, "flags", 5), flags);
  
  jsoff_t flen = flags_end - flags_start;
  const char *fstr = &js->code[flags_start];
  bool global = false, ignoreCase = false, multiline = false;
  for (jsoff_t i = 0; i < flen; i++) {
    if (fstr[i] == 'g') global = true;
    if (fstr[i] == 'i') ignoreCase = true;
    if (fstr[i] == 'm') multiline = true;
  }
  
  setprop(js, regexp_obj, js_mkstr(js, "global", 6), mkval(T_BOOL, global ? 1 : 0));
  setprop(js, regexp_obj, js_mkstr(js, "ignoreCase", 10), mkval(T_BOOL, ignoreCase ? 1 : 0));
  setprop(js, regexp_obj, js_mkstr(js, "multiline", 9), mkval(T_BOOL, multiline ? 1 : 0));
  
  return regexp_obj;
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
  
  while (next(js) != TOK_RBRACE) {
    jsval_t key = 0;
    jsoff_t id_off = 0, id_len = 0;
    
    if (js->tok == TOK_IDENTIFIER) {
      id_off = js->toff;
      id_len = js->tlen;
      if (exe) key = js_mkstr(js, js->code + js->toff, js->tlen);
    } else if (js->tok == TOK_STRING) {
      if (exe) key = js_str_literal(js);
    } else if (js->tok == TOK_NUMBER) {
      if (exe) key = js_mkstr(js, js->code + js->toff, js->tlen);
    } else {
      return js_mkerr(js, "parse error");
    }
    js->consumed = 1;
    
    if (id_len > 0 && (next(js) == TOK_COMMA || next(js) == TOK_RBRACE)) {
      jsval_t val = lookup(js, js->code + id_off, id_len);
      if (exe) {
        if (is_err(val)) return val;
        if (is_err(key)) return key;
        jsval_t res = setprop(js, obj, key, resolveprop(js, val));
        if (is_err(res)) return res;
      }
    } else if (id_len > 0 && next(js) == TOK_LPAREN) {
      uint8_t flags = js->flags;
      jsoff_t pos = js->pos - 1;
      js->consumed = 1;
      if (!parse_func_params(js, &flags)) {
        js->flags = flags;
        return js_mkerr(js, "invalid parameters");
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
        jsval_t str = js_mkstr(js, &js->code[pos], js->pos - pos);
        jsval_t func_obj = mkobj(js, 0);
        if (is_err(func_obj)) return func_obj;
        jsval_t code_key = js_mkstr(js, "__code", 6);
        setprop(js, func_obj, code_key, str);
        jsval_t name_key = js_mkstr(js, "name", 4);
        setprop(js, func_obj, name_key, key);
        jsval_t scope_key = js_mkstr(js, "__scope", 7);
        setprop(js, func_obj, scope_key, js->scope);
        jsval_t val = mkval(T_FUNC, (unsigned long) vdata(func_obj));
        jsval_t res = setprop(js, obj, key, val);
        if (is_err(res)) return res;
      }
    } else {
      EXPECT(TOK_COLON, );
      jsval_t val = js_expr(js);
      if (exe) {
        if (is_err(val)) return val;
        if (is_err(key)) return key;
        jsval_t res = setprop(js, obj, key, resolveprop(js, val));
        if (is_err(res)) return res;
      }
    }
    
    if (next(js) == TOK_RBRACE) break;
    EXPECT(TOK_COMMA, );
  }
  
  EXPECT(TOK_RBRACE, );
  return obj;
}

static bool parse_func_params(struct js *js, uint8_t *flags) {
  const char *param_names[32];
  size_t param_lens[32];
  int param_count = 0;
  
  for (bool comma = false; next(js) != TOK_EOF; comma = true) {
    if (!comma && next(js) == TOK_RPAREN) break;
    
    bool is_rest = false;
    if (next(js) == TOK_REST) {
      is_rest = true;
      js->consumed = 1;
      next(js);
    }
    
    if (next(js) != TOK_IDENTIFIER) {
      if (flags) js->flags = *flags;
      js_mkerr(js, "identifier expected");
      return false;
    }
    
    const char *param_name = &js->code[js->toff];
    size_t param_len = js->tlen;
    
    if ((js->flags & F_STRICT) && is_strict_restricted(param_name, param_len)) {
      if (flags) js->flags = *flags;
      js_mkerr(js, "cannot use '%.*s' as parameter name in strict mode", (int) param_len, param_name);
      return false;
    }
    
    if (js->flags & F_STRICT) {
      for (int i = 0; i < param_count; i++) {
        if (param_lens[i] == param_len && memcmp(param_names[i], param_name, param_len) == 0) {
          if (flags) js->flags = *flags;
          js_mkerr(js, "duplicate parameter name '%.*s' in strict mode", (int) param_len, param_name);
          return false;
        }
      }
    }
    
    if (param_count < 32) {
      param_names[param_count] = param_name;
      param_lens[param_count] = param_len;
      param_count++;
    }
    
    js->consumed = 1;
    
    if (next(js) == TOK_ASSIGN) {
      js->consumed = 1;
      int depth = 0;
      bool done = false;
      while (!done && next(js) != TOK_EOF) {
        uint8_t tok = next(js);
        if (depth == 0 && (tok == TOK_RPAREN || tok == TOK_COMMA)) {
          done = true;
        } else if (tok == TOK_LPAREN || tok == TOK_LBRACKET || tok == TOK_LBRACE) {
          depth++;
          js->consumed = 1;
        } else if (tok == TOK_RPAREN || tok == TOK_RBRACKET || tok == TOK_RBRACE) {
          depth--;
          js->consumed = 1;
        } else {
          js->consumed = 1;
        }
      }
    }
    
    if (is_rest && next(js) != TOK_RPAREN) {
      if (flags) js->flags = *flags;
      js_mkerr(js, "rest parameter must be last");
      return false;
    }
    if (next(js) == TOK_RPAREN) break;
    
    if (next(js) != TOK_COMMA) {
      if (flags) js->flags = *flags;
      js_mkerr(js, "parse error");
      return false;
    }
    js->consumed = 1;
  }
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
  if (!parse_func_params(js, &flags)) {
    js->flags = flags;
    return js_mkerr(js, "invalid parameters");
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
  jsval_t str = js_mkstr(js, &js->code[pos], js->pos - pos);
  js->consumed = 1;
  jsval_t func_obj = mkobj(js, 0);
  if (is_err(func_obj)) return func_obj;
  jsval_t code_key = js_mkstr(js, "__code", 6);
  if (is_err(code_key)) return code_key;
  jsval_t res2 = setprop(js, func_obj, code_key, str);
  if (is_err(res2)) return res2;
  
  if (is_async) {
    jsval_t async_key = js_mkstr(js, "__async", 7);
    if (is_err(async_key)) return async_key;
    jsval_t res_async = setprop(js, func_obj, async_key, js_mktrue());
    if (is_err(res_async)) return res_async;
  }
  
  if (name_len > 0) {
    jsval_t name_key = js_mkstr(js, "name", 4);
    if (is_err(name_key)) return name_key;
    jsval_t name_val = js_mkstr(js, &js->code[name_off], name_len);
    if (is_err(name_val)) return name_val;
    jsval_t res3 = setprop(js, func_obj, name_key, name_val);
    if (is_err(res3)) return res3;
  }
  
  if (!(flags & F_NOEXEC)) {
    jsval_t scope_key = js_mkstr(js, "__scope", 7);
    if (is_err(scope_key)) return scope_key;
    jsval_t res4 = setprop(js, func_obj, scope_key, js->scope);
    if (is_err(res4)) return res4;
  }
  
  return mkval(T_FUNC, (unsigned long) vdata(func_obj));
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

static jsval_t js_literal(struct js *js) {
  next(js);
  setlwm(js);
  if (js->maxcss > 0 && js->css > js->maxcss) return js_mkerr(js, "C stack");
  js->consumed = 1;
  
  switch (js->tok) {
    case TOK_ERR:
      if ((js->flags & F_STRICT) && js->toff < js->clen && js->code[js->toff] == '0' && 
          js->toff + 1 < js->clen && is_digit(js->code[js->toff + 1])) {
        return js_mkerr(js, "octal literals are not allowed in strict mode");
      }
      return js_mkerr(js, "parse error");
    case TOK_NUMBER:      return js->tval;
    case TOK_BIGINT:      return js_bigint_literal(js);
    case TOK_STRING:      return js_str_literal(js);
    case TOK_TEMPLATE:    return js_template_literal(js);
    case TOK_LBRACE:      return js_obj_literal(js);
    case TOK_LBRACKET:    return js_arr_literal(js);
    case TOK_DIV:         return js_regex_literal(js);
    case TOK_FUNC:        return js_func_literal(js, false);
    case TOK_ASYNC: {
      js->consumed = 1;
      uint8_t next_tok = next(js);
      if (next_tok == TOK_FUNC) {
        return js_func_literal(js, true);
      } else if (next_tok == TOK_LPAREN) {
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
          if (next(js) != TOK_RPAREN) return js_mkerr(js, ") expected");
          js->consumed = 1;
          js->flags = saved_flags;
          if (next(js) != TOK_ARROW) return js_mkerr(js, "=> expected");
          js->consumed = 1;
          return js_arrow_func(js, paren_start, paren_end, true);
        }
        return js_mkerr(js, "async ( must be arrow function");
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
          jsval_t body_result;
          if (is_expr) {
            js->flags |= F_NOEXEC;
            body_result = js_assignment(js);
            if (is_err(body_result)) { js->flags = flags; return body_result; }
          } else {
            body_start = js->toff;
            js->flags |= F_NOEXEC;
            js->consumed = 1;
            body_result = js_block(js, false);
            if (is_err(body_result)) { js->flags = flags; return body_result; }
            if (js->tok == TOK_RBRACE && js->consumed) {
            } else if (next(js) == TOK_RBRACE) js->consumed = 1;
          }
          js->flags = flags;
          jsoff_t body_end = js->pos;
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
          jsval_t str = js_mkstr(js, fn_str, fn_pos);
          free(fn_str);
          if (is_err(str)) return str;
          jsval_t func_obj = mkobj(js, 0);
          if (is_err(func_obj)) return func_obj;
          jsval_t code_key = js_mkstr(js, "__code", 6);
          if (is_err(code_key)) return code_key;
          jsval_t res = setprop(js, func_obj, code_key, str);
          if (is_err(res)) return res;
          jsval_t async_key = js_mkstr(js, "__async", 7);
          if (is_err(async_key)) return async_key;
          jsval_t res_async = setprop(js, func_obj, async_key, js_mktrue());
          if (is_err(res_async)) return res_async;
          if (!(flags & F_NOEXEC)) {
            jsval_t scope_key = js_mkstr(js, "__scope", 7);
            if (is_err(scope_key)) return scope_key;
            jsval_t res2 = setprop(js, func_obj, scope_key, js->scope);
            if (is_err(res2)) return res2;
          }
          return mkval(T_FUNC, (unsigned long) vdata(func_obj));
        }
        return mkcoderef((jsoff_t) id_start, (jsoff_t) id_len);
      }
      return js_mkerr(js, "unexpected token after async");
    }
    
    case TOK_NULL:        return js_mknull();
    case TOK_UNDEF:       return js_mkundef();
    case TOK_TRUE:        return js_mktrue();
    case TOK_FALSE:       return js_mkfalse();
    case TOK_THIS:        return js->this_val;
    
    case TOK_TYPEOF:      return mkcoderef((jsoff_t) js->toff, (jsoff_t) js->tlen);
    case TOK_FROM:        return mkcoderef((jsoff_t) js->toff, (jsoff_t) js->tlen);
    case TOK_VOID:        return mkcoderef((jsoff_t) js->toff, (jsoff_t) js->tlen);
    case TOK_DELETE:      return mkcoderef((jsoff_t) js->toff, (jsoff_t) js->tlen);
    case TOK_IMPORT:      return mkcoderef((jsoff_t) js->toff, (jsoff_t) js->tlen);
    case TOK_IDENTIFIER:  return mkcoderef((jsoff_t) js->toff, (jsoff_t) js->tlen);
    case TOK_CATCH:       return mkcoderef((jsoff_t) js->toff, (jsoff_t) js->tlen);
    case TOK_TRY:         return mkcoderef((jsoff_t) js->toff, (jsoff_t) js->tlen);
    case TOK_FINALLY:     return mkcoderef((jsoff_t) js->toff, (jsoff_t) js->tlen);
    
    default:              return js_mkerr(js, "bad expr");
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
    if (tok == TOK_RPAREN || tok == TOK_RBRACE || tok == TOK_SEMICOLON || 
        tok == TOK_COMMA || tok == TOK_EOF) {
      body_end_actual = js->toff;
    } else {
      body_end_actual = js->pos;
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

  jsval_t str = js_mkstr(js, fn_str, fn_pos);
  free(fn_str);
  if (is_err(str)) return str;

  jsval_t func_obj = mkobj(js, 0);
  if (is_err(func_obj)) return func_obj;
  
  jsval_t code_key = js_mkstr(js, "__code", 6);
  if (is_err(code_key)) return code_key;
  
  jsval_t res = setprop(js, func_obj, code_key, str);
  if (is_err(res)) return res;
  
  if (is_async) {
    jsval_t async_key = js_mkstr(js, "__async", 7);
    if (is_err(async_key)) return async_key;
    jsval_t res_async = setprop(js, func_obj, async_key, js_mktrue());
    if (is_err(res_async)) return res_async;
  }
  
  if (!(flags & F_NOEXEC)) {
    jsval_t scope_key = js_mkstr(js, "__scope", 7);
    if (is_err(scope_key)) return scope_key;
    jsval_t res2 = setprop(js, func_obj, scope_key, js->scope);
    if (is_err(res2)) return res2;
    
    jsval_t this_key = js_mkstr(js, "__this", 6);
    if (is_err(this_key)) return this_key;
    jsval_t res3 = setprop(js, func_obj, this_key, js->this_val);
    if (is_err(res3)) return res3;
  }
  
  return mkval(T_FUNC, (unsigned long) vdata(func_obj));
}

static jsval_t js_group(struct js *js) {
  if (next(js) == TOK_LPAREN) {
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
      
      if (paren_depth > 0) {
        if (js->tok != TOK_IDENTIFIER && js->tok != TOK_COMMA && js->tok != TOK_REST &&
            js->tok != TOK_ASSIGN && js->tok != TOK_NUMBER && js->tok != TOK_STRING &&
            js->tok != TOK_TRUE && js->tok != TOK_FALSE && js->tok != TOK_NULL &&
            js->tok != TOK_UNDEF && js->tok != TOK_LBRACKET && js->tok != TOK_RBRACKET &&
            js->tok != TOK_LBRACE && js->tok != TOK_RBRACE && js->tok != TOK_DOT &&
            js->tok != TOK_PLUS && js->tok != TOK_MINUS && js->tok != TOK_MUL && js->tok != TOK_DIV) {
          could_be_arrow = false;
        }
      }
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
      while (next(js) != TOK_RPAREN && next(js) != TOK_EOF) {
        js->consumed = 1;
      }
      if (next(js) != TOK_RPAREN) return js_mkerr(js, ") expected");
      js->consumed = 1;
      js->flags = saved_flags;
      
      if (next(js) != TOK_ARROW) return js_mkerr(js, "=> expected");
      js->consumed = 1;
      
      return js_arrow_func(js, paren_start, paren_end, false);
    } else {
      jsval_t v = js_expr(js);
      if (is_err(v)) return v;
      while (next(js) == TOK_COMMA) {
        js->consumed = 1;
        v = js_expr(js);
        if (is_err(v)) return v;
      }
      if (next(js) != TOK_RPAREN) return js_mkerr(js, ") expected");
      js->consumed = 1;
      return v;
    }
  } else {
    return js_literal(js);
  }
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
      jsval_t result = js_tagged_template(js, tag_func);
      return result;
    }
    res = lookup(js, &js->code[coderefoff(res)], codereflen(res));
  }
  while (next(js) == TOK_LPAREN || next(js) == TOK_DOT || next(js) == TOK_OPTIONAL_CHAIN || next(js) == TOK_LBRACKET || next(js) == TOK_TEMPLATE) {
    if (js->tok == TOK_TEMPLATE) {
      if (vtype(res) == T_PROP) res = resolveprop(js, res);
      if (is_err(res)) return res;
      js->consumed = 1;
      return js_tagged_template(js, res);
    } else if (js->tok == TOK_DOT || js->tok == TOK_OPTIONAL_CHAIN) {
      uint8_t op = js->tok;
      js->consumed = 1;
      if (vtype(res) != T_PROP) {
        obj = res;
      } else {
        obj = resolveprop(js, res);
      }
      if (op == TOK_OPTIONAL_CHAIN && (vtype(obj) == T_NULL || vtype(obj) == T_UNDEF)) {
        js_group(js);
        res = js_mkundef();
      } else {
        res = do_op(js, op, res, js_group(js));
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
      if (next(js) != TOK_RBRACKET) return js_mkerr(js, "] expected");
      js->consumed = 1;
      res = do_op(js, TOK_BRACKET, res, idx);
    } else {
      jsval_t func_this = obj;
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
      obj = js_mkundef();
    }
  }
  return res;
}

static jsval_t js_postfix(struct js *js) {
  jsval_t res = js_call_dot(js);
  if (is_err(res)) return res;
  next(js);
  if (js->tok == TOK_POSTINC || js->tok == TOK_POSTDEC) {
    js->consumed = 1;
    res = do_op(js, js->tok, res, 0);
  }
  return res;
}

static jsval_t js_unary(struct js *js) {
  if (next(js) == TOK_NEW) {
    js->consumed = 1;
    jsval_t obj = mkobj(js, 0);
    jsval_t saved_this = js->this_val;
    js->this_val = obj;
    jsval_t result = js_postfix(js);
    jsval_t constructed_obj = js->this_val;
    js->this_val = saved_this;
    if (vtype(result) == T_OBJ || vtype(result) == T_ARR || vtype(result) == T_PROMISE || vtype(result) == T_FUNC) return result;
    return constructed_obj;
  } else if (next(js) == TOK_DELETE) {
    js->consumed = 1;
    jsoff_t save_pos = js->pos;
    uint8_t save_tok = js->tok;
    jsval_t operand = js_postfix(js);
    if (js->flags & F_NOEXEC) return js_mktrue();
    if (vtype(operand) != T_PROP) {
      return js_mktrue();
    }
    jsoff_t prop_off = (jsoff_t) vdata(operand);
    if (is_const_prop(js, prop_off)) {
      return js_mkerr(js, "cannot delete constant property");
    }
    jsval_t owner_obj = js_mkundef();
    for (jsoff_t off = 0; off < js->brk;) {
      jsoff_t v = loadoff(js, off);
      jsoff_t cleaned = v & ~(GCMASK | CONSTMASK);
      jsoff_t n = esize(cleaned);
      if ((cleaned & 3) == T_OBJ) {
        jsoff_t first_prop = cleaned & ~3U;
        if (first_prop == prop_off) {
          owner_obj = mkval(T_OBJ, off);
          break;
        }
      } else if ((cleaned & 3) == T_PROP) {
        jsoff_t next_prop = cleaned & ~3U;
        if (next_prop == prop_off) {
          jsoff_t deleted_next = loadoff(js, prop_off) & ~(GCMASK | CONSTMASK);
          jsoff_t current = loadoff(js, off);
          saveoff(js, off, (deleted_next & ~3U) | (current & (GCMASK | CONSTMASK | 3U)));
          saveoff(js, prop_off, loadoff(js, prop_off) | GCMASK);
          js_gc(js);
          return js_mktrue();
        }
      }
      off += n;
    }
    if (vtype(owner_obj) == T_OBJ) {
      jsoff_t obj_off = (jsoff_t) vdata(owner_obj);
      jsoff_t deleted_next = loadoff(js, prop_off) & ~(GCMASK | CONSTMASK);
      jsoff_t current = loadoff(js, obj_off);
      saveoff(js, obj_off, (deleted_next & ~3U) | (current & (GCMASK | CONSTMASK | 3U)));
      saveoff(js, prop_off, loadoff(js, prop_off) | GCMASK);
      js_gc(js);
    }
    (void) save_pos;
    (void) save_tok;
    return js_mktrue();
  } else if (next(js) == TOK_AWAIT) {
    js->consumed = 1;
    jsval_t expr = js_unary(js);
    if (is_err(expr)) return expr;
    if (js->flags & F_NOEXEC) return expr;
    jsval_t resolved = resolveprop(js, expr);
    if (vtype(resolved) != T_PROMISE) {
      return resolved;
    }
    
    jsval_t p_obj = mkval(T_OBJ, vdata(resolved));
    jsoff_t state_off = lkp(js, p_obj, "__state", 7);
    if (state_off == 0) return js_mkerr(js, "invalid promise state");
    
    int state = (int)tod(resolveprop(js, mkval(T_PROP, state_off)));
    
    if (state != 0) {
      jsoff_t val_off = lkp(js, p_obj, "__value", 7);
      if (val_off == 0) return js_mkerr(js, "invalid promise value");
      jsval_t val = resolveprop(js, mkval(T_PROP, val_off));
      if (state == 1) {
        return val;
      } else if (state == 2) {
        return js_throw(js, val);
      }
    }
    
    mco_coro* current_mco = mco_running();
    if (!current_mco) return js_mkerr(js, "await can only be used inside async functions");
    
    async_exec_context_t *ctx = (async_exec_context_t *)mco_get_user_data(current_mco);
    if (!ctx || !ctx->coro) return js_mkerr(js, "invalid async context");
    
    coroutine_t *coro = ctx->coro;
    coro->awaited_promise = resolved;
    coro->is_settled = false;
    coro->is_ready = false;
    
    jsval_t resume_obj = mkobj(js, 0);
    setprop(js, resume_obj, js_mkstr(js, "__native_func", 13), js_mkfun(resume_coroutine_wrapper));
    setprop(js, resume_obj, js_mkstr(js, "__coroutine", 11), tov((double)(uintptr_t)coro));
    jsval_t resume_fn = mkval(T_FUNC, vdata(resume_obj));
    
    jsval_t reject_obj = mkobj(js, 0);
    setprop(js, reject_obj, js_mkstr(js, "__native_func", 13), js_mkfun(reject_coroutine_wrapper));
    setprop(js, reject_obj, js_mkstr(js, "__coroutine", 11), tov((double)(uintptr_t)coro));
    jsval_t reject_fn = mkval(T_FUNC, vdata(reject_obj));
    
    jsval_t then_args[] = { resume_fn, reject_fn };
    jsval_t saved_this = js->this_val;
    js->this_val = resolved;
    (void)builtin_promise_then(js, then_args, 2);
    js->this_val = saved_this;
    
    uint8_t saved_flags = js->flags;
    const char *saved_code = js->code;
    jsoff_t saved_clen = js->clen;
    jsoff_t saved_pos = js->pos;
    uint8_t saved_tok = js->tok;
    uint8_t saved_consumed = js->consumed;
    
    mco_result mco_res = mco_yield(current_mco);
    js->flags = saved_flags;
    js->code = saved_code;
    js->clen = saved_clen;
    js->pos = saved_pos;
    js->tok = saved_tok;
    js->consumed = saved_consumed;
    
    if (mco_res != MCO_SUCCESS) {
      return js_mkerr(js, "failed to yield coroutine");
    }
    
    jsval_t result = coro->result;
    bool is_error = coro->is_error;
    
    coro->is_settled = false;
    coro->awaited_promise = js_mkundef();
    
    if (is_error) {
      return js_throw(js, result);
    }
    return result;
  } else if (next(js) == TOK_NOT || js->tok == TOK_TILDA || js->tok == TOK_TYPEOF ||
      js->tok == TOK_VOID || js->tok == TOK_MINUS || js->tok == TOK_PLUS) {
    uint8_t t = js->tok;
    if (t == TOK_MINUS) t = TOK_UMINUS;
    if (t == TOK_PLUS) t = TOK_UPLUS;
    js->consumed = 1;
    return do_op(js, t, js_mkundef(), js_unary(js));
  } else {
    return js_postfix(js);
  }
}

static jsval_t js_exp(struct js *js) {
  jsval_t base = js_unary(js);
  if (is_err(base)) return base;
  if (next(js) == TOK_EXP) {
    js->consumed = 1;
    jsval_t exponent = js_exp(js);
    if (is_err(exponent)) return exponent;
    return do_op(js, TOK_EXP, base, exponent);
  }
  return base;
}

static jsval_t js_mul_div_rem(struct js *js) {
  LTR_BINOP(js_exp, (next(js) == TOK_MUL || js->tok == TOK_DIV || js->tok == TOK_REM));
}

static jsval_t js_plus_minus(struct js *js) {
  LTR_BINOP(js_mul_div_rem, (next(js) == TOK_PLUS || js->tok == TOK_MINUS));
}

static jsval_t js_shifts(struct js *js) {
  LTR_BINOP(js_plus_minus, 
  (next(js) == TOK_SHR || next(js) == TOK_SHL ||
   next(js) == TOK_ZSHR));
}

static jsval_t js_comparison(struct js *js) {
  LTR_BINOP(js_shifts, 
  (next(js) == TOK_LT || next(js) == TOK_LE ||
   next(js) == TOK_GT || next(js) == TOK_GE ||
   next(js) == TOK_INSTANCEOF || next(js) == TOK_IN));
}

static jsval_t js_equality(struct js *js) {
  LTR_BINOP(js_comparison, (next(js) == TOK_EQ || next(js) == TOK_NE));
}

static jsval_t js_bitwise_and(struct js *js) {
  LTR_BINOP(js_equality, (next(js) == TOK_AND));
}

static jsval_t js_bitwise_xor(struct js *js) {
  LTR_BINOP(js_bitwise_and, (next(js) == TOK_XOR));
}

static jsval_t js_bitwise_or(struct js *js) {
  LTR_BINOP(js_bitwise_xor, (next(js) == TOK_OR));
}

static jsval_t js_nullish_coalesce(struct js *js) {
  jsval_t res = js_bitwise_or(js);
  if (is_err(res)) return res;
  uint8_t flags = js->flags;
  while (next(js) == TOK_NULLISH) {
    js->consumed = 1;
    res = resolveprop(js, res);
    uint8_t res_type = vtype(res);
    if (res_type != T_NULL && res_type != T_UNDEF) {
      js->flags |= F_NOEXEC;
    }
    if (js->flags & F_NOEXEC) {
      js_nullish_coalesce(js);
    } else {
      res = js_nullish_coalesce(js);
    }
  }
  js->flags = flags;
  return res;
}

static jsval_t js_logical_and(struct js *js) {
  jsval_t res = js_nullish_coalesce(js);
  if (is_err(res)) return res;
  uint8_t flags = js->flags;
  while (next(js) == TOK_LAND) {
    js->consumed = 1;
    res = resolveprop(js, res);
    if (!js_truthy(js, res)) js->flags |= F_NOEXEC;
    if (js->flags & F_NOEXEC) {
      js_logical_and(js);
    } else {
      res = js_logical_and(js);
    }
  }
  js->flags = flags;
  return res;
}

static jsval_t js_logical_or(struct js *js) {
  jsval_t res = js_logical_and(js);
  if (is_err(res)) return res;
  uint8_t flags = js->flags;
  while (next(js) == TOK_LOR) {
    js->consumed = 1;
    res = resolveprop(js, res);
    if (js_truthy(js, res)) js->flags |= F_NOEXEC;
    if (js->flags & F_NOEXEC) {
      js_logical_or(js);
    } else {
      res = js_logical_or(js);
    }
  }
  js->flags = flags;
  return res;
}

static jsval_t js_ternary(struct js *js) {
  jsval_t res = js_logical_or(js);
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
    jsoff_t body_end = js->pos;

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
    
    jsval_t str = js_mkstr(js, fn_str, fn_pos);
    free(fn_str);
    if (is_err(str)) return str;
    
    jsval_t func_obj = mkobj(js, 0);
    if (is_err(func_obj)) return func_obj;
    
    jsval_t code_key = js_mkstr(js, "__code", 6);
    if (is_err(code_key)) return code_key;
    
    jsval_t res2 = setprop(js, func_obj, code_key, str);
    if (is_err(res2)) return res2;
    
    if (!(flags & F_NOEXEC)) {
      jsval_t scope_key = js_mkstr(js, "__scope", 7);
      if (is_err(scope_key)) return scope_key;
      jsval_t res3 = setprop(js, func_obj, scope_key, js->scope);
      if (is_err(res3)) return res3;
    }
    
    return mkval(T_FUNC, (unsigned long) vdata(func_obj));
  }
  
  while (!is_err(res) && (next(js) == TOK_ASSIGN || js->tok == TOK_PLUS_ASSIGN ||
   js->tok == TOK_MINUS_ASSIGN || js->tok == TOK_MUL_ASSIGN ||
   js->tok == TOK_DIV_ASSIGN || js->tok == TOK_REM_ASSIGN ||
   js->tok == TOK_SHL_ASSIGN || js->tok == TOK_SHR_ASSIGN ||
   js->tok == TOK_ZSHR_ASSIGN || js->tok == TOK_AND_ASSIGN ||
   js->tok == TOK_XOR_ASSIGN || js->tok == TOK_OR_ASSIGN)) {
    uint8_t op = js->tok;
    js->consumed = 1;
    jsval_t rhs = js_assignment(js);
    if (is_err(rhs)) return rhs;
    res = do_op(js, op, res, rhs);
  }
  
  return res;
}

static jsval_t js_decl(struct js *js, bool is_const) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  js->consumed = 1;
  for (;;) {
    if (next(js) == TOK_LBRACE) {
      js->consumed = 1;
      
      typedef struct { jsoff_t name_off; jsoff_t name_len; } PropName;
      PropName props[32];
      int prop_count = 0;
      
      while (next(js) != TOK_RBRACE && next(js) != TOK_EOF && prop_count < 32) {
        EXPECT(TOK_IDENTIFIER, );
        props[prop_count].name_off = js->toff;
        props[prop_count].name_len = js->tlen;
        prop_count++;
        js->consumed = 1;
        
        if (next(js) == TOK_COLON) {
          js->consumed = 1;
          EXPECT(TOK_IDENTIFIER, );
          js->consumed = 1;
        }
        
        if (next(js) == TOK_RBRACE) break;
        EXPECT(TOK_COMMA, );
      }
      
      EXPECT(TOK_RBRACE, );
      
      jsval_t v = js_mkundef();
      if (next(js) == TOK_ASSIGN) {
        js->consumed = 1;
        v = js_expr(js);
        if (is_err(v)) return v;
      } else {
        return js_mkerr(js, "destructuring requires assignment");
      }
      
      if (exe) {
        jsval_t obj = resolveprop(js, v);
        if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR) {
          return js_mkerr(js, "cannot destructure non-object");
        }
        
        for (int i = 0; i < prop_count; i++) {
          const char *prop_name = &js->code[props[i].name_off];
          jsoff_t prop_len = props[i].name_len;
          
          if (lkp(js, js->scope, prop_name, prop_len) > 0) {
            return js_mkerr(js, "'%.*s' already declared", (int) prop_len, prop_name);
          }
          
          jsoff_t prop_off = lkp(js, obj, prop_name, prop_len);
          jsval_t prop_val = js_mkundef();
          
          if (prop_off > 0) {
            prop_val = resolveprop(js, mkval(T_PROP, prop_off));
          }
          
          jsval_t x = mkprop(js, js->scope, js_mkstr(js, prop_name, prop_len), prop_val, is_const);
          if (is_err(x)) return x;
        }
      }
    } else {
      EXPECT(TOK_IDENTIFIER, );
      js->consumed = 0;
      jsoff_t noff = js->toff, nlen = js->tlen;
      char *name = (char *) &js->code[noff];
      
      if (exe && (js->flags & F_STRICT) && is_strict_restricted(name, nlen)) {
        return js_mkerr(js, "cannot use '%.*s' as variable name in strict mode", (int) nlen, name);
      }
      
      if (exe && (js->flags & F_STRICT) && is_strict_reserved(name, nlen)) {
        return js_mkerr(js, "'%.*s' is reserved in strict mode", (int) nlen, name);
      }
      
      jsval_t v = js_mkundef();
      js->consumed = 1;
      if (next(js) == TOK_ASSIGN) {
        js->consumed = 1;
        v = js_expr(js);
        if (is_err(v)) return v;
      }
      if (exe) {
        if (lkp(js, js->scope, name, nlen) > 0) return js_mkerr(js, "'%.*s' already declared", (int) nlen, name);
        jsval_t x = mkprop(js, js->scope, js_mkstr(js, name, nlen), resolveprop(js, v), is_const);
        if (is_err(x)) return x;
      }
    }
    
    uint8_t decl_next = next(js);
    if (decl_next == TOK_SEMICOLON || decl_next == TOK_EOF || decl_next == TOK_RBRACE) break;
    EXPECT(TOK_COMMA, );
  }
  return js_mkundef();
}

static jsval_t js_expr(struct js *js) {
  return js_assignment(js);
}

static jsval_t js_let(struct js *js) {
  return js_decl(js, false);
}

static jsval_t js_const(struct js *js) {
  return js_decl(js, true);
}

static jsval_t js_func_decl(struct js *js) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  js->consumed = 1;
  EXPECT(TOK_IDENTIFIER, );
  js->consumed = 0;
  jsoff_t noff = js->toff, nlen = js->tlen;
  char *name = (char *) &js->code[noff];
  js->consumed = 1;
  EXPECT(TOK_LPAREN, );
  jsoff_t pos = js->pos - 1;
  for (bool comma = false; next(js) != TOK_EOF; comma = true) {
    if (!comma && next(js) == TOK_RPAREN) break;
    
    bool is_rest = false;
    if (next(js) == TOK_REST) {
      is_rest = true;
      js->consumed = 1;
      next(js);
    }
    
    if (next(js) != TOK_IDENTIFIER) return js_mkerr(js, "identifier expected");
    js->consumed = 1;
    
    if (next(js) == TOK_ASSIGN) {
      js->consumed = 1;
      int depth = 0;
      bool done = false;
      while (!done && next(js) != TOK_EOF) {
        uint8_t tok = next(js);
        if (depth == 0 && (tok == TOK_RPAREN || tok == TOK_COMMA)) {
          done = true;
        } else if (tok == TOK_LPAREN || tok == TOK_LBRACKET || tok == TOK_LBRACE) {
          depth++;
          js->consumed = 1;
        } else if (tok == TOK_RPAREN || tok == TOK_RBRACKET || tok == TOK_RBRACE) {
          depth--;
          js->consumed = 1;
        } else {
          js->consumed = 1;
        }
      }
    }
    
    if (is_rest && next(js) != TOK_RPAREN) {
      return js_mkerr(js, "rest parameter must be last");
    }
    if (next(js) == TOK_RPAREN) break;
    EXPECT(TOK_COMMA, );
  }
  EXPECT(TOK_RPAREN, );
  EXPECT(TOK_LBRACE, );
  js->consumed = 0;
  uint8_t flags = js->flags;
  js->flags |= F_NOEXEC;
  jsval_t res = js_block(js, false);
  if (is_err(res)) {
    js->flags = flags;
    return res;
  }
  js->flags = flags;
  jsval_t str = js_mkstr(js, &js->code[pos], js->pos - pos);
  jsval_t func_obj = mkobj(js, 0);
  if (is_err(func_obj)) return func_obj;
  jsval_t code_key = js_mkstr(js, "__code", 6);
  if (is_err(code_key)) return code_key;
  jsval_t res2 = setprop(js, func_obj, code_key, str);
  if (is_err(res2)) return res2;
  jsval_t name_key = js_mkstr(js, "name", 4);
  if (is_err(name_key)) return name_key;
  jsval_t name_val = js_mkstr(js, name, nlen);
  if (is_err(name_val)) return name_val;
  jsval_t res3 = setprop(js, func_obj, name_key, name_val);
  if (is_err(res3)) return res3;
  if (exe) {
    jsval_t scope_key = js_mkstr(js, "__scope", 7);
    if (is_err(scope_key)) return scope_key;
    jsval_t res4 = setprop(js, func_obj, scope_key, js->scope);
    if (is_err(res4)) return res4;
  }
  jsval_t func = mkval(T_FUNC, (unsigned long) vdata(func_obj));
  if (exe) {
    if (lkp(js, js->scope, name, nlen) > 0)
      return js_mkerr(js, "'%.*s' already declared", (int) nlen, name);
    jsval_t x = mkprop(js, js->scope, js_mkstr(js, name, nlen), func, false);
    if (is_err(x)) return x;
  }
  
  return js_mkundef();
}

static jsval_t js_func_decl_async(struct js *js) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  js->consumed = 1;
  EXPECT(TOK_IDENTIFIER, );
  js->consumed = 0;
  jsoff_t noff = js->toff, nlen = js->tlen;
  char *name = (char *) &js->code[noff];
  js->consumed = 1;
  EXPECT(TOK_LPAREN, );
  jsoff_t pos = js->pos - 1;
  if (!parse_func_params(js, NULL)) {
    return js_mkerr(js, "invalid parameters");
  }
  EXPECT(TOK_RPAREN, );
  EXPECT(TOK_LBRACE, );
  js->consumed = 0;
  uint8_t flags = js->flags;
  js->flags |= F_NOEXEC;
  jsval_t res = js_block(js, false);
  if (is_err(res)) {
    js->flags = flags;
    return res;
  }
  js->flags = flags;
  jsval_t str = js_mkstr(js, &js->code[pos], js->pos - pos);
  jsval_t func_obj = mkobj(js, 0);
  if (is_err(func_obj)) return func_obj;
  jsval_t code_key = js_mkstr(js, "__code", 6);
  if (is_err(code_key)) return code_key;
  jsval_t res2 = setprop(js, func_obj, code_key, str);
  if (is_err(res2)) return res2;
  jsval_t async_key = js_mkstr(js, "__async", 7);
  if (is_err(async_key)) return async_key;
  jsval_t res_async = setprop(js, func_obj, async_key, js_mktrue());
  if (is_err(res_async)) return res_async;
  jsval_t name_key = js_mkstr(js, "name", 4);
  if (is_err(name_key)) return name_key;
  jsval_t name_val = js_mkstr(js, name, nlen);
  if (is_err(name_val)) return name_val;
  jsval_t res3 = setprop(js, func_obj, name_key, name_val);
  if (is_err(res3)) return res3;
  if (exe) {
    jsval_t scope_key = js_mkstr(js, "__scope", 7);
    if (is_err(scope_key)) return scope_key;
    jsval_t res4 = setprop(js, func_obj, scope_key, js->scope);
    if (is_err(res4)) return res4;
  }
  jsval_t func = mkval(T_FUNC, (unsigned long) vdata(func_obj));
  if (exe) {
    if (lkp(js, js->scope, name, nlen) > 0)
      return js_mkerr(js, "'%.*s' already declared", (int) nlen, name);
    jsval_t x = mkprop(js, js->scope, js_mkstr(js, name, nlen), func, false);
    if (is_err(x)) return x;
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

static jsval_t js_if(struct js *js) {
  js->consumed = 1;
  EXPECT(TOK_LPAREN, );
  jsval_t res = js_mkundef(), cond = resolveprop(js, js_expr(js));
  EXPECT(TOK_RPAREN, );
  
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
    *res = js_mkerr(js, "parse error");
    return false;
  } else {
    js->consumed = 1;
    return true;
  }
}

static inline bool is_err2(jsval_t *v, jsval_t *res) {
  bool r = is_err(*v);
  if (r) *res = *v;
  return r;
}

static jsval_t js_for(struct js *js) {
  uint8_t flags = js->flags, exe = !(flags & F_NOEXEC);
  jsval_t v, res = js_mkundef();
  jsoff_t pos1 = 0, pos2 = 0, pos3 = 0, pos4 = 0;
  if (exe) mkscope(js);
  if (!expect(js, TOK_FOR, &res)) goto done;
  if (!expect(js, TOK_LPAREN, &res)) goto done;
  
  bool is_for_in = false;
  bool is_for_of = false;
  jsoff_t var_name_off = 0, var_name_len = 0;
  bool is_const_var = false;
  bool is_var_decl = false;
  
  if (next(js) == TOK_LET || next(js) == TOK_CONST || next(js) == TOK_VAR) {
    if (js->tok == TOK_VAR) {
      is_var_decl = true;
      if ((js->flags & F_STRICT) && !js->var_warning_shown) {
        fprintf(stderr, "Warning: 'var' is deprecated, use 'let' or 'const' instead\n");
        js->var_warning_shown = true;
      }
    }
    is_const_var = (js->tok == TOK_CONST);
    js->consumed = 1;
    if (next(js) == TOK_IDENTIFIER) {
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
      v = js_expr(js);
      if (is_err2(&v, &res)) goto done;
    }
  } else if (next(js) == TOK_SEMICOLON) {
  } else {
    v = js_expr(js);
    if (is_err2(&v, &res)) goto done;
  }
  
  if (is_for_in) {
    jsval_t obj_expr = js_expr(js);
    if (is_err2(&obj_expr, &res)) goto done;
    if (!expect(js, TOK_RPAREN, &res)) goto done;
    
    jsoff_t body_start = js->pos;
    js->flags |= F_NOEXEC;
    v = js_block_or_stmt(js);
    if (is_err2(&v, &res)) goto done;
    jsoff_t body_end = js->pos;
    
    if (exe) {
      jsval_t obj = resolveprop(js, obj_expr);
      if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR && vtype(obj) != T_FUNC) {
        res = js_mkerr(js, "for-in requires object");
        goto done;
      }
      
      jsval_t iter_obj = obj;
      if (vtype(obj) == T_FUNC) {
        iter_obj = mkval(T_OBJ, vdata(obj));
      }
      
      jsoff_t prop_off = loadoff(js, (jsoff_t) vdata(iter_obj)) & ~(3U | CONSTMASK);
      
      while (prop_off < js->brk && prop_off != 0) {
        jsoff_t koff = loadoff(js, prop_off + (jsoff_t) sizeof(prop_off));
        jsoff_t klen = offtolen(loadoff(js, koff));
        const char *key = (char *) &js->mem[koff + sizeof(koff)];
        
        if (!streq(key, klen, "__proto__", 9)) {
          jsval_t key_str = js_mkstr(js, key, klen);
          
          const char *var_name = &js->code[var_name_off];
          jsoff_t existing = lkp(js, js->scope, var_name, var_name_len);
          if (existing > 0) {
            saveval(js, existing + sizeof(jsoff_t) * 2, key_str);
          } else {
            jsval_t x = mkprop(js, js->scope, js_mkstr(js, var_name, var_name_len), key_str, is_const_var);
            if (is_err(x)) {
              res = x;
              goto done;
            }
          }
          
          js->pos = body_start;
          js->consumed = 1;
          js->flags = (flags & ~F_NOEXEC) | F_LOOP;
          v = js_block_or_stmt(js);
          if (is_err(v)) {
            res = v;
            goto done;
          }
          
          if (js->flags & F_BREAK) break;
          if (js->flags & F_RETURN) {
            res = v;
            goto done;
          }
        }
        
        prop_off = loadoff(js, prop_off) & ~(3U | CONSTMASK);
      }
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
    js->flags |= F_NOEXEC;
    v = js_block_or_stmt(js);
    if (is_err2(&v, &res)) goto done;
    jsoff_t body_end = js->pos;
    
    if (exe) {
      jsval_t iterable = resolveprop(js, iter_expr);
      uint8_t itype = vtype(iterable);
      
      if (itype == T_ARR) {
        jsoff_t next_prop = loadoff(js, (jsoff_t) vdata(iterable)) & ~(3U | CONSTMASK);
        jsoff_t length = 0;
        jsoff_t scan = next_prop;
        
        while (scan < js->brk && scan != 0) {
          jsoff_t koff = loadoff(js, scan + (jsoff_t) sizeof(scan));
          jsoff_t klen = offtolen(loadoff(js, koff));
          const char *key = (char *) &js->mem[koff + sizeof(koff)];
          
          if (streq(key, klen, "length", 6)) {
            jsval_t val = loadval(js, scan + (jsoff_t) (sizeof(scan) + sizeof(koff)));
            if (vtype(val) == T_NUM) length = (jsoff_t) tod(val);
            break;
          }
          scan = loadoff(js, scan) & ~(3U | CONSTMASK);
        }
        
        for (jsoff_t i = 0; i < length; i++) {
          char idx[16];
          snprintf(idx, sizeof(idx), "%u", (unsigned) i);
          jsoff_t idxlen = (jsoff_t) strlen(idx);
          jsoff_t prop = next_prop;
          jsval_t val = js_mkundef();
          
          while (prop < js->brk && prop != 0) {
            jsoff_t koff = loadoff(js, prop + (jsoff_t) sizeof(prop));
            jsoff_t klen = offtolen(loadoff(js, koff));
            const char *key = (char *) &js->mem[koff + sizeof(koff)];
            if (streq(key, klen, idx, idxlen)) {
              val = loadval(js, prop + (jsoff_t) (sizeof(prop) + sizeof(koff)));
              break;
            }
            prop = loadoff(js, prop) & ~(3U | CONSTMASK);
          }
          
          const char *var_name = &js->code[var_name_off];
          jsoff_t existing = lkp(js, js->scope, var_name, var_name_len);
          if (existing > 0) {
            saveval(js, existing + sizeof(jsoff_t) * 2, val);
          } else {
            jsval_t x = mkprop(js, js->scope, js_mkstr(js, var_name, var_name_len), val, is_const_var);
            if (is_err(x)) {
              res = x;
              goto done;
            }
          }
          
          js->pos = body_start;
          js->consumed = 1;
          js->flags = (flags & ~F_NOEXEC) | F_LOOP;
          v = js_block_or_stmt(js);
          if (is_err(v)) {
            res = v;
            goto done;
          }
          
          if (js->flags & F_BREAK) break;
          if (js->flags & F_RETURN) {
            res = v;
            goto done;
          }
        }
      } else if (itype == T_STR) {
        jsoff_t slen, soff = vstr(js, iterable, &slen);
        const char *str = (char *) &js->mem[soff];
        
        for (jsoff_t i = 0; i < slen; i++) {
          jsval_t char_str = js_mkstr(js, &str[i], 1);
          
          const char *var_name = &js->code[var_name_off];
          jsoff_t existing = lkp(js, js->scope, var_name, var_name_len);
          if (existing > 0) {
            saveval(js, existing + sizeof(jsoff_t) * 2, char_str);
          } else {
            jsval_t x = mkprop(js, js->scope, js_mkstr(js, var_name, var_name_len), char_str, is_const_var);
            if (is_err(x)) {
              res = x;
              goto done;
            }
          }
          
          js->pos = body_start;
          js->consumed = 1;
          js->flags = (flags & ~F_NOEXEC) | F_LOOP;
          v = js_block_or_stmt(js);
          if (is_err(v)) {
            res = v;
            goto done;
          }
          
          if (js->flags & F_BREAK) break;
          if (js->flags & F_RETURN) {
            res = v;
            goto done;
          }
        }
      } else {
        res = js_mkerr(js, "for-of requires iterable");
        goto done;
      }
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
    v = js_expr(js);
    if (is_err2(&v, &res)) goto done;
  }
  if (!expect(js, TOK_RPAREN, &res)) goto done;
  pos3 = js->pos;
  v = js_block_or_stmt(js);
  if (is_err2(&v, &res)) goto done;
  pos4 = js->pos;
  while (!(flags & F_NOEXEC)) {
    js->flags = flags, js->pos = pos1, js->consumed = 1;
    if (next(js) != TOK_SEMICOLON) {
      v = resolveprop(js, js_expr(js));
      if (is_err2(&v, &res)) goto done;
      if (!js_truthy(js, v)) break;
    }
    js->pos = pos3, js->consumed = 1, js->flags |= F_LOOP;
    v = js_block_or_stmt(js);
    if (is_err2(&v, &res)) goto done;
    if (js->flags & F_BREAK) break;
    if (js->flags & F_RETURN) {
      res = v;
      break;
    }
    js->flags = flags, js->pos = pos2, js->consumed = 1;
    if (next(js) != TOK_RPAREN) {
      v = js_expr(js);
      if (is_err2(&v, &res)) goto done;
    }
  }
  js->pos = pos4, js->tok = TOK_SEMICOLON, js->consumed = 0;
done:
  if (exe) delscope(js);
  uint8_t preserve = 0;
  if (js->flags & F_RETURN) {
    preserve = js->flags & (F_RETURN | F_NOEXEC);
  }
  js->flags = flags | preserve;
  return res;
}

static jsval_t js_while(struct js *js) {
  uint8_t flags = js->flags, exe = !(flags & F_NOEXEC);
  jsval_t res = js_mkundef(), v;
  
  js->consumed = 1;
  if (!expect(js, TOK_LPAREN, &res)) return res;
  
  jsoff_t cond_start = js->pos;
  js->flags |= F_NOEXEC;
  v = js_expr(js);
  if (is_err(v)) return v;
  
  if (!expect(js, TOK_RPAREN, &res)) return res;
  
  jsoff_t body_start = js->pos;
  v = js_block_or_stmt(js);
  if (is_err(v)) return v;
  jsoff_t body_end = js->pos;
  
  if (exe) {
    while (true) {
      js->flags = flags;
      js->pos = cond_start;
      js->consumed = 1;
      
      v = resolveprop(js, js_expr(js));
      if (is_err(v)) {
        res = v;
        break;
      }
      
      if (!js_truthy(js, v)) break;
      
      js->pos = body_start;
      js->consumed = 1;
      js->flags = (flags & ~F_NOEXEC) | F_LOOP;
      
      v = js_block_or_stmt(js);
      if (is_err(v)) {
        res = v;
        break;
      }
      
      if (js->flags & F_BREAK) {
        break;
      }
      
      if (js->flags & F_RETURN) {
        res = v;
        break;
      }
    }
  }
  
  js->pos = body_end;
  js->tok = TOK_SEMICOLON;
  js->consumed = 0;
  
  uint8_t preserve = 0;
  if (js->flags & F_RETURN) {
    preserve = js->flags & (F_RETURN | F_NOEXEC);
  }
  js->flags = flags | preserve;
  
  return res;
}

static jsval_t js_do_while(struct js *js) {
  uint8_t flags = js->flags, exe = !(flags & F_NOEXEC);
  jsval_t res = js_mkundef(), v;
  
  js->consumed = 1;
  
  jsoff_t body_start = js->pos;
  bool is_block = (next(js) == TOK_LBRACE);
  js->flags |= F_NOEXEC;
  v = js_block_or_stmt(js);
  if (is_err(v)) return v;
  
  if (is_block && next(js) == TOK_RBRACE) {
    js->consumed = 1;
  }
  (void) js->pos;
  
  if (!expect(js, TOK_WHILE, &res)) return res;
  if (!expect(js, TOK_LPAREN, &res)) return res;
  
  jsoff_t cond_start = js->pos;
  v = js_expr(js);
  if (is_err(v)) return v;
  
  if (!expect(js, TOK_RPAREN, &res)) return res;
  jsoff_t cond_end = js->pos;
  
  if (exe) {
    do {
      js->pos = body_start;
      js->consumed = 1;
      js->flags = (flags & ~F_NOEXEC) | F_LOOP;
      
      v = js_block_or_stmt(js);
      if (is_err(v)) {
        res = v;
        break;
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
  }
  
  js->pos = cond_end;
  js->consumed = 1;
  
  uint8_t preserve = 0;
  if (js->flags & F_RETURN) {
    preserve = js->flags & (F_RETURN | F_NOEXEC);
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
        js->consumed = 1;
      }
      if (next(js) != TOK_RPAREN) {
        return js_mkerr(js, ") expected in catch");
      }
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
    
    while (next(js) != TOK_EOF && next(js) != TOK_RBRACE && !(js->flags & (F_RETURN | F_THROW | F_BREAK))) {
      try_result = js_stmt(js);
      if (is_err(try_result)) {
        had_exception = true;
        break;
      }
    }
    
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
      
      jsval_t err_obj = mkobj(js, 0);
      jsval_t msg_key = js_mkstr(js, "message", 7);
      jsval_t name_key = js_mkstr(js, "name", 4);
      
      char *colon = strchr(saved_errmsg, ':');
      if (colon && strncmp(saved_errmsg, "Uncaught ", 9) == 0) {
        char *type_start = saved_errmsg + 9;
        size_t type_len = colon - type_start;
        char *msg_start = colon + 2;
        char *newline = strchr(msg_start, '\n');
        size_t msg_len = newline ? (size_t)(newline - msg_start) : strlen(msg_start);
        
        jsval_t name_val = js_mkstr(js, type_start, type_len);
        jsval_t msg_val = js_mkstr(js, msg_start, msg_len);
        setprop(js, err_obj, name_key, name_val);
        setprop(js, err_obj, msg_key, msg_val);
      } else {
        jsval_t msg_val = js_mkstr(js, saved_errmsg, strlen(saved_errmsg));
        jsval_t name_val = js_mkstr(js, "Error", 5);
        setprop(js, err_obj, name_key, name_val);
        setprop(js, err_obj, msg_key, msg_val);
      }
      
      exception_value = err_obj;
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
        mkprop(js, js->scope, key, exception_value, false);
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

static jsval_t js_break(struct js *js) {
  if (js->flags & F_NOEXEC) {
  } else {
    if (!(js->flags & (F_LOOP | F_SWITCH))) return js_mkerr(js, "not in loop or switch");
    js->flags |= F_BREAK | F_NOEXEC;
  }
  js->consumed = 1;
  return js_mkundef();
}

static jsval_t js_continue(struct js *js) {
  if (js->flags & F_NOEXEC) {
  } else {
    if (!(js->flags & F_LOOP)) return js_mkerr(js, "not in loop");
    js->flags |= F_NOEXEC;
  }
  js->consumed = 1;
  return js_mkundef();
}

static jsval_t js_return(struct js *js) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  uint8_t in_func = js->flags & F_CALL;
  js->consumed = 1;
  jsval_t res = js_mkundef();
  
  uint8_t nxt = next(js);
  if (nxt != TOK_SEMICOLON && nxt != TOK_RBRACE && nxt != TOK_EOF) {
    res = resolveprop(js, js_expr(js));
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
        
        if (js->flags & F_BREAK) js->flags &= ~F_BREAK; break;
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
    return js_mkerr(js, "with statement not allowed in strict mode");
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
    jsval_t with_scope = mkentity(js, 0 | T_OBJ, &parent_scope_offset, sizeof(parent_scope_offset));
    
    jsval_t with_marker = js_mkstr(js, "__with_object__", 15);
    jsval_t with_ref = mkprop(js, with_scope, with_marker, with_obj, false);
    if (is_err(with_ref)) return with_ref;
    
    jsval_t saved_scope = js->scope;
    js->scope = with_scope;
    
    res = js_block_or_stmt(js);
    if (global_scope_stack && utarray_len(global_scope_stack) > 0) utarray_pop_back(global_scope_stack);
    js->scope = saved_scope;
  } else {
    res = js_block_or_stmt(js);
  }
  
  js->flags = flags;
  return res;
}

static jsval_t js_class_decl(struct js *js) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  js->consumed = 1;
  EXPECT(TOK_IDENTIFIER, );
  js->consumed = 0;
  
  jsoff_t class_name_off = js->toff, class_name_len = js->tlen;
  char *class_name = (char *) &js->code[class_name_off];
  js->consumed = 1;
  
  jsoff_t super_off = 0, super_len = 0;
  if (next(js) == TOK_IDENTIFIER && js->tlen == 7 &&
      streq(&js->code[js->toff], js->tlen, "extends", 7)) {
    js->consumed = 1;
    EXPECT(TOK_IDENTIFIER, );
    super_off = js->toff;
    super_len = js->tlen;
    js->consumed = 1;
  }
  
  EXPECT(TOK_LBRACE, );
  jsoff_t class_body_start = js->pos;
  jsoff_t constructor_params_start = 0;
  jsoff_t constructor_body_start = 0, constructor_body_end = 0;
  uint8_t save_flags = js->flags;
  js->flags |= F_NOEXEC;
  
  typedef struct { 
    jsoff_t name_off, name_len, fn_start, fn_end; 
    bool is_async;
    bool is_static;
    bool is_field;
    jsoff_t field_start, field_end; 
  } MethodInfo;
  
  MethodInfo methods[32];
  int method_count = 0;
  
  uint8_t class_tok;
  while ((class_tok = next(js)) != TOK_RBRACE && class_tok != TOK_EOF && method_count < 32) {
    bool is_async_method = false;
    bool is_static_member = false;
    
    if (next(js) == TOK_STATIC) {
      is_static_member = true;
      js->consumed = 1;
    }
    if (next(js) == TOK_ASYNC) {
      is_async_method = true;
      js->consumed = 1;
    }
    if (next(js) != TOK_IDENTIFIER && (next(js) < TOK_ASYNC || next(js) > TOK_STATIC)) {
      js->flags = save_flags;
      return js_mkerr(js, "method name expected");
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
      
      methods[method_count].name_off = method_name_off;
      methods[method_count].name_len = method_name_len;
      methods[method_count].is_static = is_static_member;
      methods[method_count].is_async = false;
      methods[method_count].is_field = true;
      methods[method_count].field_start = field_start;
      methods[method_count].field_end = field_end;
      methods[method_count].fn_start = 0;
      methods[method_count].fn_end = 0;
      method_count++;
      continue;
    }
    
    if (next(js) == TOK_SEMICOLON || (next(js) != TOK_LPAREN && next(js) == TOK_IDENTIFIER)) {
      if (next(js) == TOK_SEMICOLON) js->consumed = 1;
      methods[method_count].name_off = method_name_off;
      methods[method_count].name_len = method_name_len;
      methods[method_count].is_static = is_static_member;
      methods[method_count].is_async = false;
      methods[method_count].is_field = true;
      methods[method_count].field_start = 0;
      methods[method_count].field_end = 0;
      methods[method_count].fn_start = 0;
      methods[method_count].fn_end = 0;
      method_count++;
      continue;
    }
    
    EXPECT(TOK_LPAREN, js->flags = save_flags);
    jsoff_t method_params_start = js->pos - 1;
    for (bool comma = false; next(js) != TOK_EOF; comma = true) {
      if (!comma && next(js) == TOK_RPAREN) break;
      EXPECT(TOK_IDENTIFIER, js->flags = save_flags);
      js->consumed = 1;
      
      if (next(js) == TOK_ASSIGN) {
        js->consumed = 1;
        int depth = 0;
        bool done = false;
        while (!done && next(js) != TOK_EOF) {
          uint8_t tok = next(js);
          if (depth == 0 && (tok == TOK_RPAREN || tok == TOK_COMMA)) {
            done = true;
          } else if (tok == TOK_LPAREN || tok == TOK_LBRACKET || tok == TOK_LBRACE) {
            depth++;
            js->consumed = 1;
          } else if (tok == TOK_RPAREN || tok == TOK_RBRACKET || tok == TOK_RBRACE) {
            depth--;
            js->consumed = 1;
          } else {
            js->consumed = 1;
          }
        }
      }
      
      if (next(js) == TOK_RPAREN) break;
      EXPECT(TOK_COMMA, js->flags = save_flags);
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
      methods[method_count].name_off = method_name_off;
      methods[method_count].name_len = method_name_len;
      methods[method_count].fn_start = method_params_start;
      methods[method_count].fn_end = method_body_end;
      methods[method_count].is_async = is_async_method;
      methods[method_count].is_static = is_static_member;
      methods[method_count].is_field = false;
      methods[method_count].field_start = 0;
      methods[method_count].field_end = 0;
      method_count++;
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
      jsoff_t super_proto_off = lkp(js, super_obj, "prototype", 9);
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
    if (super_len > 0) {
      jsval_t super_key = js_mkstr(js, "super", 5);
      if (is_err(super_key)) return super_key;
      jsval_t res_super = setprop(js, func_scope, super_key, super_constructor);
      if (is_err(res_super)) return res_super;
    }
    
    for (int i = 0; i < method_count; i++) {
      if (methods[i].is_static) continue;
      if (methods[i].is_field) continue;
      
      jsval_t method_name = js_mkstr(js, &js->code[methods[i].name_off], methods[i].name_len);
      if (is_err(method_name)) return method_name;
      
      jsoff_t mlen = methods[i].fn_end - methods[i].fn_start;
      jsval_t method_code = js_mkstr(js, &js->code[methods[i].fn_start], mlen);
      if (is_err(method_code)) return method_code;
      
      jsval_t method_obj = mkobj(js, 0);
      if (is_err(method_obj)) return method_obj;
      
      jsval_t mcode_key = js_mkstr(js, "__code", 6);
      if (is_err(mcode_key)) return mcode_key;
      jsval_t mres = setprop(js, method_obj, mcode_key, method_code);
      if (is_err(mres)) return mres;
      
      if (methods[i].is_async) {
        jsval_t async_key = js_mkstr(js, "__async", 7);
        if (is_err(async_key)) return async_key;
        jsval_t async_val = mkval(T_BOOL, 1);
        jsval_t res_async = setprop(js, method_obj, async_key, async_val);
        if (is_err(res_async)) return res_async;
      }
      
      jsval_t mscope_key = js_mkstr(js, "__scope", 7);
      if (is_err(mscope_key)) return mscope_key;
      jsval_t mscope_res = setprop(js, method_obj, mscope_key, func_scope);
      if (is_err(mscope_res)) return mscope_res;
      
      jsval_t method_func = mkval(T_FUNC, (unsigned long) vdata(method_obj));
      jsval_t set_res = setprop(js, proto, method_name, method_func);
      if (is_err(set_res)) return set_res;
    }
    
    jsval_t func_obj = mkobj(js, 0);
    if (is_err(func_obj)) return func_obj;
    
    if (constructor_params_start > 0 && constructor_body_start > 0) {
      jsoff_t code_len = constructor_body_end - constructor_params_start;
      
      jsval_t code_key = js_mkstr(js, "__code", 6);
      if (is_err(code_key)) return code_key;
      jsval_t ctor_str = js_mkstr(js, &js->code[constructor_params_start], code_len);
      if (is_err(ctor_str)) return ctor_str;
      
      jsval_t res2 = setprop(js, func_obj, code_key, ctor_str);
      if (is_err(res2)) return res2;
    } else {
      jsval_t code_key = js_mkstr(js, "__code", 6);
      if (is_err(code_key)) return code_key;
      jsval_t default_ctor = js_mkstr(js, "(){}", 4);
      if (is_err(default_ctor)) return default_ctor;
      jsval_t res2 = setprop(js, func_obj, code_key, default_ctor);
      if (is_err(res2)) return res2;
    }
    
    int field_count = 0;
    for (int i = 0; i < method_count; i++) {
      if (methods[i].is_static) continue;
      if (methods[i].is_field) field_count++;
    }
    
    if (field_count > 0) {
      size_t metadata_size = field_count * sizeof(jsoff_t) * 4;
      jsoff_t meta_len = (jsoff_t) (metadata_size + 1);
      jsoff_t meta_header = (jsoff_t) ((meta_len << 2) | T_STR);
      jsoff_t meta_off = js_alloc(js, meta_len + sizeof(meta_header));
      if (meta_off == (jsoff_t) ~0) return js_mkerr(js, "oom");
      
      memcpy(&js->mem[meta_off], &meta_header, sizeof(meta_header));
      jsoff_t *metadata = (jsoff_t *)(&js->mem[meta_off + sizeof(meta_header)]);
      
      int meta_idx = 0;
      for (int i = 0; i < method_count; i++) {
        if (methods[i].is_static) continue;
        if (!methods[i].is_field) continue;
        
        metadata[meta_idx * 4 + 0] = methods[i].name_off;
        metadata[meta_idx * 4 + 1] = methods[i].name_len;
        metadata[meta_idx * 4 + 2] = methods[i].field_start;
        metadata[meta_idx * 4 + 3] = methods[i].field_end;
        meta_idx++;
      }
      
      js->mem[meta_off + sizeof(meta_header) + metadata_size] = 0;
      jsval_t fields_meta = mkval(T_STR, meta_off);
      
      jsval_t fields_key = js_mkstr(js, "__fields", 8);
      if (is_err(fields_key)) return fields_key;
      jsval_t res_fields = setprop(js, func_obj, fields_key, fields_meta);
      if (is_err(res_fields)) return res_fields;
      
      jsval_t count_key = js_mkstr(js, "__field_count", 13);
      if (is_err(count_key)) return count_key;
      jsval_t res_count = setprop(js, func_obj, count_key, tov((double)field_count));
      if (is_err(res_count)) return res_count;
      
      jsval_t src_key = js_mkstr(js, "__source", 8);
      if (is_err(src_key)) return src_key;
      jsval_t src_ref = js_mkstr(js, js->code, js->clen);
      if (is_err(src_ref)) return src_ref;
      jsval_t res_src = setprop(js, func_obj, src_key, src_ref);
      if (is_err(res_src)) return res_src;
    }
    
    jsval_t scope_key = js_mkstr(js, "__scope", 7);
    if (is_err(scope_key)) return scope_key;
    jsval_t res3 = setprop(js, func_obj, scope_key, func_scope);
    if (is_err(res3)) return res3;
    
    jsval_t name_key = js_mkstr(js, "name", 4);
    if (is_err(name_key)) return name_key;
    jsval_t name_val = js_mkstr(js, class_name, class_name_len);
    if (is_err(name_val)) return name_val;
    jsval_t res_name = setprop(js, func_obj, name_key, name_val);
    if (is_err(res_name)) return res_name;
    
    jsval_t proto_key = js_mkstr(js, "prototype", 9);
    if (is_err(proto_key)) return proto_key;
    jsval_t proto_res = setprop(js, func_obj, proto_key, proto);
    if (is_err(proto_res)) return proto_res;
    
    jsval_t constructor = mkval(T_FUNC, (unsigned long) vdata(func_obj));
    if (lkp(js, js->scope, class_name, class_name_len) > 0) {
      return js_mkerr(js, "'%.*s' already declared", (int) class_name_len, class_name);
    }
    jsval_t x = mkprop(js, js->scope, js_mkstr(js, class_name, class_name_len), constructor, false);
    if (is_err(x)) return x;
    
    for (int i = 0; i < method_count; i++) {
      if (!methods[i].is_static) continue;
      
      jsval_t member_name = js_mkstr(js, &js->code[methods[i].name_off], methods[i].name_len);
      if (is_err(member_name)) return member_name;
      
      if (methods[i].is_field) {
        jsval_t field_val = js_mkundef();
        if (methods[i].field_start > 0 && methods[i].field_end > methods[i].field_start) {
          const char *saved_code = js->code;
          jsoff_t saved_clen = js->clen;
          jsoff_t saved_pos = js->pos;
          uint8_t saved_tok = js->tok;
          uint8_t saved_consumed = js->consumed;
          jsoff_t saved_toff = js->toff;
          jsoff_t saved_tlen = js->tlen;
          
          jsoff_t field_len = methods[i].field_end - methods[i].field_start;
          js->code = &saved_code[methods[i].field_start];
          js->clen = field_len;
          js->pos = 0;
          js->consumed = 1;
          
          field_val = js_expr(js);
          field_val = resolveprop(js, field_val);
          
          js->code = saved_code;
          js->clen = saved_clen;
          js->pos = saved_pos;
          js->tok = saved_tok;
          js->consumed = saved_consumed;
          js->toff = saved_toff;
          js->tlen = saved_tlen;
        }
        jsval_t set_res = setprop(js, func_obj, member_name, field_val);
        if (is_err(set_res)) return set_res;
      } else {
        jsoff_t mlen = methods[i].fn_end - methods[i].fn_start;
        jsval_t method_code = js_mkstr(js, &js->code[methods[i].fn_start], mlen);
        if (is_err(method_code)) return method_code;
        
        jsval_t method_obj = mkobj(js, 0);
        if (is_err(method_obj)) return method_obj;
        
        jsval_t mcode_key = js_mkstr(js, "__code", 6);
        if (is_err(mcode_key)) return mcode_key;
        jsval_t mres = setprop(js, method_obj, mcode_key, method_code);
        if (is_err(mres)) return mres;
        
        jsval_t mscope_key = js_mkstr(js, "__scope", 7);
        if (is_err(mscope_key)) return mscope_key;
        jsval_t mscope_res = setprop(js, method_obj, mscope_key, func_scope);
        if (is_err(mscope_res)) return mscope_res;
        
        jsval_t method_func = mkval(T_FUNC, (unsigned long) vdata(method_obj));
        jsval_t set_res = setprop(js, func_obj, member_name, method_func);
        if (is_err(set_res)) return set_res;
      }
    }
    
    (void) class_body_start;
    return constructor;
  }
  
  (void) class_body_start;
  return js_mkundef();
}

static void js_throw_handle(struct js *js, jsval_t *res) {
  js->consumed = 1;
  jsval_t throw_val = js_expr(js);
  if (js->flags & F_NOEXEC) {
    *res = js_mkundef();
  } else {
    throw_val = resolveprop(js, throw_val);
    if (is_err(throw_val)) {
      *res = throw_val;
    } else {
      *res = js_throw(js, throw_val);
    }
  }
}

static jsval_t find_var_scope(struct js *js) {
  if ((js->flags & F_CALL) && global_scope_stack && utarray_len(global_scope_stack) > 0) {
    jsoff_t *scope_off = (jsoff_t *)utarray_eltptr(global_scope_stack, 0);
    if (scope_off && *scope_off != 0) return mkval(T_OBJ, *scope_off);
  }
  
  jsval_t scope = js->scope;
  while (vdata(upper(js, scope)) != 0) {
    scope = upper(js, scope);
  }
  return scope;
}

static jsval_t js_var_decl(struct js *js) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  jsval_t var_scope = find_var_scope(js);
  
  js->consumed = 1;
  for (;;) {
    EXPECT(TOK_IDENTIFIER, );
    js->consumed = 0;
    jsoff_t noff = js->toff, nlen = js->tlen;
    char *name = (char *) &js->code[noff];
    
    if (exe && (js->flags & F_STRICT) && is_strict_restricted(name, nlen)) {
      return js_mkerr(js, "cannot use '%.*s' as variable name in strict mode", (int) nlen, name);
    }
    
    if (exe && (js->flags & F_STRICT) && is_strict_reserved(name, nlen)) {
      return js_mkerr(js, "'%.*s' is reserved in strict mode", (int) nlen, name);
    }
    
    jsval_t v = js_mkundef();
    js->consumed = 1;
    if (next(js) == TOK_ASSIGN) {
      js->consumed = 1;
      v = js_expr(js);
      if (is_err(v)) return v;
    }
    
    if (exe) {
      jsoff_t existing_off = lkp(js, var_scope, name, nlen);
      if (existing_off > 0) {
        jsval_t key_val = js_mkstr(js, name, nlen);
        if (!is_err(v) && vtype(v) != T_UNDEF) {
          setprop(js, var_scope, key_val, resolveprop(js, v));
        }
      } else {
        jsval_t x = mkprop(js, var_scope, js_mkstr(js, name, nlen), resolveprop(js, v), false);
        if (is_err(x)) return x;
      }
    }
    
    uint8_t var_next = next(js);
    if (var_next == TOK_SEMICOLON || var_next == TOK_EOF || var_next == TOK_RBRACE) break;
    EXPECT(TOK_COMMA, );
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
  if (next(js) == TOK_FUNC) {
    *res = js_func_decl_async(js);
    return;
  }
  *res = js_mkerr(js, "async must be followed by function");
}

static jsval_t js_stmt(struct js *js) {
  jsval_t res;
  if (js->brk > js->gct) js_gc(js);
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
  
  if (!is_block_statement) {
    int next_tok = next(js);
    bool missing_semicolon = next_tok != TOK_SEMICOLON && next_tok != TOK_EOF && next_tok != TOK_RBRACE;
    if (missing_semicolon) return js_mkerr(js, "; expected");
    if (next_tok == TOK_SEMICOLON) js->consumed = 1;
  }
  
  return res;
}

static jsval_t builtin_String(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_mkstr(js, "", 0);
  jsval_t arg = args[0];
  if (vtype(arg) == T_STR) return arg;
  const char *str = js_str(js, arg);
  return js_mkstr(js, str, strlen(str));
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

static jsval_t builtin_Number(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return tov(0.0);
  jsval_t arg = args[0];
  
  if (vtype(arg) == T_NUM) return arg;
  if (vtype(arg) == T_BOOL) {
    return tov(vdata(arg) ? 1.0 : 0.0);
  } else if (vtype(arg) == T_STR) {
    jsoff_t len;
    jsoff_t off = vstr(js, arg, &len);
    const char *str = (char *) &js->mem[off];
    char *end;
    double val = strtod(str, &end);
    return tov(val);
  } else if (vtype(arg) == T_NULL || vtype(arg) == T_UNDEF) {
    return tov(0.0);
  }
  
  return tov(0.0);
}

static jsval_t builtin_Boolean(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkval(T_BOOL, 0);
  return mkval(T_BOOL, js_truthy(js, args[0]) ? 1 : 0);
}

static jsval_t builtin_Object(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_mkobj(js);
  jsval_t arg = args[0];
  if (vtype(arg) == T_NULL || vtype(arg) == T_UNDEF) {
    return js_mkobj(js);
  }
  return arg;
}

static jsval_t builtin_eval(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_mkundef();
  
  jsval_t code_arg = args[0];
  if (vtype(code_arg) != T_STR) return code_arg;
  
  jsoff_t code_len, code_off = vstr(js, code_arg, &code_len);
  const char *code_str = (const char *)&js->mem[code_off];
  
  const char *saved_code = js->code;
  jsoff_t saved_clen = js->clen;
  jsoff_t saved_pos = js->pos;
  uint8_t saved_tok = js->tok;
  uint8_t saved_consumed = js->consumed;
  uint8_t saved_flags = js->flags;
  
  jsval_t result = js_eval(js, code_str, code_len);
  
  js->code = saved_code;
  js->clen = saved_clen;
  js->pos = saved_pos;
  js->tok = saved_tok;
  js->consumed = saved_consumed;
  js->flags = saved_flags;
  
  return result;
}

static jsval_t builtin_Function(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) {
    jsval_t code_str = js_mkstr(js, "(){}", 4);
    if (is_err(code_str)) return code_str;
    
    jsval_t func_obj = mkobj(js, 0);
    if (is_err(func_obj)) return func_obj;
    
    jsval_t code_key = js_mkstr(js, "__code", 6);
    if (is_err(code_key)) return code_key;
    
    jsval_t res = setprop(js, func_obj, code_key, code_str);
    if (is_err(res)) return res;
    
    jsval_t scope_key = js_mkstr(js, "__scope", 7);
    if (is_err(scope_key)) return scope_key;
    
    res = setprop(js, func_obj, scope_key, js_glob(js));
    if (is_err(res)) return res;
    
    return mkval(T_FUNC, (unsigned long) vdata(func_obj));
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
  
  jsval_t code_str = js_mkstr(js, NULL, total_len);
  if (is_err(code_str)) return code_str;
  
  jsoff_t code_len, code_off = vstr(js, code_str, &code_len);
  char *code_ptr = (char *)&js->mem[code_off];
  size_t pos = 0;
  
  code_ptr[pos++] = '(';
  
  for (int i = 0; i < nargs - 1; i++) {
    jsoff_t param_len, param_off = vstr(js, args[i], &param_len);
    memcpy(code_ptr + pos, &js->mem[param_off], param_len);
    pos += param_len;
    if (i < nargs - 2) {
      code_ptr[pos++] = ',';
    }
  }
  
  code_ptr[pos++] = ')';
  code_ptr[pos++] = '{';
  
  jsoff_t body_len, body_off = vstr(js, body, &body_len);
  memcpy(code_ptr + pos, &js->mem[body_off], body_len);
  pos += body_len;
  
  code_ptr[pos++] = '}';

  jsval_t func_obj = mkobj(js, 0);
  if (is_err(func_obj)) return func_obj;
  
  jsval_t code_key = js_mkstr(js, "__code", 6);
  if (is_err(code_key)) return code_key;
  
  jsval_t res = setprop(js, func_obj, code_key, code_str);
  if (is_err(res)) return res;
  
  jsval_t scope_key = js_mkstr(js, "__scope", 7);
  if (is_err(scope_key)) return scope_key;
  
  res = setprop(js, func_obj, scope_key, js_glob(js));
  if (is_err(res)) return res;
  
  return mkval(T_FUNC, (unsigned long) vdata(func_obj));
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
  jsoff_t len_off = lkp(js, arr, "length", 6);
  if (len_off == 0) return 0;
  
  jsval_t len_val = resolveprop(js, mkval(T_PROP, len_off));
  if (vtype(len_val) != T_NUM) return 0;
  
  int len = (int) tod(len_val);
  if (len <= 0) return 0;
  
  jsval_t *args_out = (jsval_t *)ANT_GC_MALLOC(sizeof(jsval_t) * len);
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

static jsval_t builtin_function_apply(struct js *js, jsval_t *args, int nargs) {
  jsval_t func = js->this_val;
  if (vtype(func) != T_FUNC && vtype(func) != T_CFUNC) {
    return js_mkerr(js, "apply requires a function");
  }
  
  jsval_t this_arg = (nargs > 0) ? args[0] : js_mkundef();
  jsval_t *call_args = NULL;
  int call_nargs = 0;
  
  if (nargs > 1 && vtype(args[1]) == T_ARR) {
    call_nargs = extract_array_args(js, args[1], &call_args);
  }
  
  jsval_t saved_this = js->this_val;
  push_this(this_arg);
  
  js->this_val = this_arg;
  jsval_t result = call_js_with_args(js, func, call_args, call_nargs);

  pop_this();
  js->this_val = saved_this;

  if (call_args) ANT_GC_FREE(call_args);

  return result;
}

static jsval_t builtin_function_bind(struct js *js, jsval_t *args, int nargs) {
  jsval_t func = js->this_val;
  
  if (vtype(func) != T_FUNC && vtype(func) != T_CFUNC) {
    return js_mkerr(js, "bind requires a function");
  }

  jsval_t this_arg = (nargs > 0) ? args[0] : js_mkundef();

  if (vtype(func) == T_CFUNC) {
    jsval_t bound_func = mkobj(js, 0);
    if (is_err(bound_func)) return bound_func;
    
    setprop(js, bound_func, js_mkstr(js, "__native_func", 13), func);
    setprop(js, bound_func, js_mkstr(js, "__this", 6), this_arg);
    
    return mkval(T_FUNC, (unsigned long) vdata(bound_func));
  }

  jsval_t func_obj = mkval(T_OBJ, vdata(func));
  jsval_t bound_func = mkobj(js, 0);
  if (is_err(bound_func)) return bound_func;

  jsoff_t code_off = lkp(js, func_obj, "__code", 6);
  if (code_off != 0) {
    jsval_t code_val = resolveprop(js, mkval(T_PROP, code_off));
    setprop(js, bound_func, js_mkstr(js, "__code", 6), code_val);
  }

  jsoff_t scope_off = lkp(js, func_obj, "__scope", 7);
  if (scope_off != 0) {
    jsval_t scope_val = resolveprop(js, mkval(T_PROP, scope_off));
    setprop(js, bound_func, js_mkstr(js, "__scope", 7), scope_val);
  }

  jsoff_t async_off = lkp(js, func_obj, "__async", 7);
  if (async_off != 0) {
    jsval_t async_val = resolveprop(js, mkval(T_PROP, async_off));
    setprop(js, bound_func, js_mkstr(js, "__async", 7), async_val);
  }

  setprop(js, bound_func, js_mkstr(js, "__this", 6), this_arg);
  return mkval(T_FUNC, (unsigned long) vdata(bound_func));
}

static jsval_t builtin_Array(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = mkarr(js);
  
  if (nargs == 1 && vtype(args[0]) == T_NUM) {
    jsoff_t len = (jsoff_t) tod(args[0]);
    jsval_t len_key = js_mkstr(js, "length", 6);
    jsval_t len_val = tov((double) len);
    setprop(js, arr, len_key, len_val);
  } else {
    for (int i = 0; i < nargs; i++) {
      char idxstr[16];
      snprintf(idxstr, sizeof(idxstr), "%d", i);
      jsval_t key = js_mkstr(js, idxstr, strlen(idxstr));
      setprop(js, arr, key, args[i]);
    }
    jsval_t len_key = js_mkstr(js, "length", 6);
    jsval_t len_val = tov((double) nargs);
    setprop(js, arr, len_key, len_val);
  }
  
  return mkval(T_ARR, vdata(arr));
}

static jsval_t builtin_Error(struct js *js, jsval_t *args, int nargs) {
  jsval_t err_obj = js->this_val;
  bool use_this = (vtype(err_obj) == T_OBJ);
  
  if (!use_this) {
    err_obj = mkobj(js, 0);
  }
  
  jsval_t message = js_mkstr(js, "", 0);
  if (nargs > 0) {
    if (vtype(args[0]) == T_STR) {
      message = args[0];
    } else {
      const char *str = js_str(js, args[0]);
      message = js_mkstr(js, str, strlen(str));
    }
  }
  
  jsval_t message_key = js_mkstr(js, "message", 7);
  setprop(js, err_obj, message_key, message);
  
  jsval_t name_key = js_mkstr(js, "name", 4);
  jsval_t name_val = js_mkstr(js, "Error", 5);
  setprop(js, err_obj, name_key, name_val);
  
  return err_obj;
}

static jsval_t builtin_RegExp(struct js *js, jsval_t *args, int nargs) {
  jsval_t regexp_obj = js->this_val;
  bool use_this = (vtype(regexp_obj) == T_OBJ);
  
  if (!use_this) {
    regexp_obj = mkobj(js, 0);
  }
  
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
  
  bool global = false, ignoreCase = false, multiline = false;
  for (jsoff_t i = 0; i < flags_len; i++) {
    if (flags_str[i] == 'g') global = true;
    if (flags_str[i] == 'i') ignoreCase = true;
    if (flags_str[i] == 'm') multiline = true;
  }
  
  setprop(js, regexp_obj, js_mkstr(js, "global", 6), mkval(T_BOOL, global ? 1 : 0));
  setprop(js, regexp_obj, js_mkstr(js, "ignoreCase", 10), mkval(T_BOOL, ignoreCase ? 1 : 0));
  setprop(js, regexp_obj, js_mkstr(js, "multiline", 9), mkval(T_BOOL, multiline ? 1 : 0));
  
  return regexp_obj;
}

static jsval_t builtin_Date(struct js *js, jsval_t *args, int nargs) {
  jsval_t date_obj = js->this_val;
  bool use_this = (vtype(date_obj) == T_OBJ);
  
  if (!use_this) {
    date_obj = mkobj(js, 0);
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
    } else {
      timestamp_ms = 0;
    }
  } else {
    timestamp_ms = 0;
  }
  
  jsval_t time_key = js_mkstr(js, "__time", 6);
  jsval_t time_val = tov(timestamp_ms);
  setprop(js, date_obj, time_key, time_val);
  
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

static jsval_t builtin_Math_abs(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  return tov(fabs(tod(args[0])));
}

static jsval_t builtin_Math_acos(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  return tov(acos(tod(args[0])));
}

static jsval_t builtin_Math_acosh(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  return tov(acosh(tod(args[0])));
}

static jsval_t builtin_Math_asin(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  return tov(asin(tod(args[0])));
}

static jsval_t builtin_Math_asinh(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  return tov(asinh(tod(args[0])));
}

static jsval_t builtin_Math_atan(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  return tov(atan(tod(args[0])));
}

static jsval_t builtin_Math_atanh(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  return tov(atanh(tod(args[0])));
}

static jsval_t builtin_Math_atan2(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 2 || vtype(args[0]) != T_NUM || vtype(args[1]) != T_NUM) return tov(NAN);
  return tov(atan2(tod(args[0]), tod(args[1])));
}

static jsval_t builtin_Math_cbrt(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  return tov(cbrt(tod(args[0])));
}

static jsval_t builtin_Math_ceil(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  return tov(ceil(tod(args[0])));
}

static jsval_t builtin_Math_clz32(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(32);
  uint32_t n = (uint32_t) tod(args[0]);
  if (n == 0) return tov(32);
  int count = 0;
  while ((n & 0x80000000U) == 0) { count++; n <<= 1; }
  return tov((double) count);
}

static jsval_t builtin_Math_cos(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  return tov(cos(tod(args[0])));
}

static jsval_t builtin_Math_cosh(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  return tov(cosh(tod(args[0])));
}

static jsval_t builtin_Math_exp(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  return tov(exp(tod(args[0])));
}

static jsval_t builtin_Math_expm1(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  return tov(expm1(tod(args[0])));
}

static jsval_t builtin_Math_floor(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  return tov(floor(tod(args[0])));
}

static jsval_t builtin_Math_fround(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  return tov((double)(float)tod(args[0]));
}

static jsval_t builtin_Math_hypot(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs == 0) return tov(0.0);
  double sum = 0.0;
  for (int i = 0; i < nargs; i++) {
    if (vtype(args[i]) != T_NUM) return tov(NAN);
    double v = tod(args[i]);
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
  (void) js;
  if (nargs < 2) return tov(0);
  int32_t a = toInt32(tod(args[0]));
  int32_t b = toInt32(tod(args[1]));
  return tov((double)((int32_t)((uint32_t)a * (uint32_t)b)));
}

static jsval_t builtin_Math_log(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  return tov(log(tod(args[0])));
}

static jsval_t builtin_Math_log1p(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  return tov(log1p(tod(args[0])));
}

static jsval_t builtin_Math_log10(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  return tov(log10(tod(args[0])));
}

static jsval_t builtin_Math_log2(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  return tov(log2(tod(args[0])));
}

static jsval_t builtin_Math_max(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs == 0) return tov(-INFINITY);
  double max_val = -INFINITY;
  for (int i = 0; i < nargs; i++) {
    if (vtype(args[i]) != T_NUM) return tov(NAN);
    double v = tod(args[i]);
    if (isnan(v)) return tov(NAN);
    if (v > max_val) max_val = v;
  }
  return tov(max_val);
}

static jsval_t builtin_Math_min(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs == 0) return tov(INFINITY);
  double min_val = INFINITY;
  for (int i = 0; i < nargs; i++) {
    if (vtype(args[i]) != T_NUM) return tov(NAN);
    double v = tod(args[i]);
    if (isnan(v)) return tov(NAN);
    if (v < min_val) min_val = v;
  }
  return tov(min_val);
}

static jsval_t builtin_Math_pow(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 2 || vtype(args[0]) != T_NUM || vtype(args[1]) != T_NUM) return tov(NAN);
  return tov(pow(tod(args[0]), tod(args[1])));
}

static bool random_seeded = false;

static jsval_t builtin_Math_random(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  (void) args;
  (void) nargs;
  if (!random_seeded) {
    srand((unsigned int) time(NULL));
    random_seeded = true;
  }
  return tov((double) rand() / ((double) RAND_MAX + 1.0));
}

static jsval_t builtin_Math_round(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  double x = tod(args[0]);
  if (isnan(x) || isinf(x)) return tov(x);
  return tov(floor(x + 0.5));
}

static jsval_t builtin_Math_sign(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  double v = tod(args[0]);
  if (isnan(v)) return tov(NAN);
  if (v > 0) return tov(1.0);
  if (v < 0) return tov(-1.0);
  return tov(v);
}

static jsval_t builtin_Math_sin(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  return tov(sin(tod(args[0])));
}

static jsval_t builtin_Math_sinh(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  return tov(sinh(tod(args[0])));
}

static jsval_t builtin_Math_sqrt(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  return tov(sqrt(tod(args[0])));
}

static jsval_t builtin_Math_tan(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  return tov(tan(tod(args[0])));
}

static jsval_t builtin_Math_tanh(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  return tov(tanh(tod(args[0])));
}

static jsval_t builtin_Math_trunc(struct js *js, jsval_t *args, int nargs) {
  (void) js;
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  return tov(trunc(tod(args[0])));
}

static jsval_t builtin_object_keys(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkarr(js);
  jsval_t obj = args[0];
  
  if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR && vtype(obj) != T_FUNC) return mkarr(js);
  if (vtype(obj) == T_FUNC) obj = mkval(T_OBJ, vdata(obj));
  
  jsval_t arr = mkarr(js);
  jsoff_t idx = 0;
  jsoff_t next = loadoff(js, (jsoff_t) vdata(obj)) & ~(3U | CONSTMASK);
  
  while (next < js->brk && next != 0) {
    jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
    jsoff_t klen = offtolen(loadoff(js, koff));
    const char *key = (char *) &js->mem[koff + sizeof(koff)];
    
    next = loadoff(js, next) & ~(3U | CONSTMASK);
    
    if (streq(key, klen, "__proto__", 9)) continue;
    if (klen > 7 && memcmp(key, "__desc_", 7) == 0) continue;
    
    bool should_include = true;
    char desc_key[128];
    snprintf(desc_key, sizeof(desc_key), "__desc_%.*s", (int)klen, key);
    jsoff_t desc_off = lkp(js, obj, desc_key, strlen(desc_key));
    
    if (desc_off != 0) {
      jsval_t desc_obj = resolveprop(js, mkval(T_PROP, desc_off));
      if (vtype(desc_obj) == T_OBJ) {
        jsoff_t enum_off = lkp(js, desc_obj, "enumerable", 10);
        if (enum_off != 0) {
          jsval_t enum_val = resolveprop(js, mkval(T_PROP, enum_off));
          should_include = js_truthy(js, enum_val);
        }
      }
    }
    
    if (should_include) {
      char idxstr[16];
      snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) idx);
      jsval_t idx_key = js_mkstr(js, idxstr, strlen(idxstr));
      jsval_t key_val = js_mkstr(js, key, klen);
      setprop(js, arr, idx_key, key_val);
      idx++;
    }
  }
  
  for (jsoff_t i = 0; i < idx / 2; i++) {
    jsoff_t j = idx - 1 - i;
    char istr[16], jstr[16];
    snprintf(istr, sizeof(istr), "%u", (unsigned) i);
    snprintf(jstr, sizeof(jstr), "%u", (unsigned) j);
    jsoff_t ioff = lkp(js, arr, istr, strlen(istr));
    jsoff_t joff = lkp(js, arr, jstr, strlen(jstr));
    jsval_t iv = loadval(js, ioff + sizeof(jsoff_t) * 2);
    jsval_t jv = loadval(js, joff + sizeof(jsoff_t) * 2);
    saveval(js, ioff + sizeof(jsoff_t) * 2, jv);
    saveval(js, joff + sizeof(jsoff_t) * 2, iv);
  }
  
  jsval_t len_key = js_mkstr(js, "length", 6);
  jsval_t len_val = tov((double) idx);
  setprop(js, arr, len_key, len_val);
  return mkval(T_ARR, vdata(arr));
}

static jsval_t builtin_object_values(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkarr(js);
  jsval_t obj = args[0];
  
  if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR && vtype(obj) != T_FUNC) return mkarr(js);
  if (vtype(obj) == T_FUNC) obj = mkval(T_OBJ, vdata(obj));
  
  jsval_t arr = mkarr(js);
  jsoff_t idx = 0;
  jsoff_t next = loadoff(js, (jsoff_t) vdata(obj)) & ~(3U | CONSTMASK);
  
  while (next < js->brk && next != 0) {
    jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
    jsoff_t klen = offtolen(loadoff(js, koff));
    const char *key = (char *) &js->mem[koff + sizeof(koff)];
    jsval_t val = loadval(js, next + (jsoff_t) (sizeof(next) + sizeof(koff)));
    
    next = loadoff(js, next) & ~(3U | CONSTMASK);
    
    if (streq(key, klen, "__proto__", 9)) continue;
    if (klen > 7 && memcmp(key, "__desc_", 7) == 0) continue;
    
    bool should_include = true;
    char desc_key[128];
    snprintf(desc_key, sizeof(desc_key), "__desc_%.*s", (int)klen, key);
    jsoff_t desc_off = lkp(js, obj, desc_key, strlen(desc_key));
    
    if (desc_off != 0) {
      jsval_t desc_obj = resolveprop(js, mkval(T_PROP, desc_off));
      if (vtype(desc_obj) == T_OBJ) {
        jsoff_t enum_off = lkp(js, desc_obj, "enumerable", 10);
        if (enum_off != 0) {
          jsval_t enum_val = resolveprop(js, mkval(T_PROP, enum_off));
          should_include = js_truthy(js, enum_val);
        }
      }
    }
    
    if (should_include) {
      char idxstr[16];
      snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) idx);
      jsval_t idx_key = js_mkstr(js, idxstr, strlen(idxstr));
      setprop(js, arr, idx_key, val);
      idx++;
    }
  }
  
  for (jsoff_t i = 0; i < idx / 2; i++) {
    jsoff_t j = idx - 1 - i;
    char istr[16], jstr[16];
    snprintf(istr, sizeof(istr), "%u", (unsigned) i);
    snprintf(jstr, sizeof(jstr), "%u", (unsigned) j);
    jsoff_t ioff = lkp(js, arr, istr, strlen(istr));
    jsoff_t joff = lkp(js, arr, jstr, strlen(jstr));
    jsval_t iv = loadval(js, ioff + sizeof(jsoff_t) * 2);
    jsval_t jv = loadval(js, joff + sizeof(jsoff_t) * 2);
    saveval(js, ioff + sizeof(jsoff_t) * 2, jv);
    saveval(js, joff + sizeof(jsoff_t) * 2, iv);
  }
  
  jsval_t len_key = js_mkstr(js, "length", 6);
  jsval_t len_val = tov((double) idx);
  setprop(js, arr, len_key, len_val);
  return mkval(T_ARR, vdata(arr));
}

static jsval_t builtin_object_entries(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkarr(js);
  jsval_t obj = args[0];
  
  if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR && vtype(obj) != T_FUNC) return mkarr(js);
  if (vtype(obj) == T_FUNC) obj = mkval(T_OBJ, vdata(obj));
  
  jsval_t arr = mkarr(js);
  jsoff_t idx = 0;
  jsoff_t next = loadoff(js, (jsoff_t) vdata(obj)) & ~(3U | CONSTMASK);
  
  while (next < js->brk && next != 0) {
    jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
    jsoff_t klen = offtolen(loadoff(js, koff));
    const char *key = (char *) &js->mem[koff + sizeof(koff)];
    jsval_t val = loadval(js, next + (jsoff_t) (sizeof(next) + sizeof(koff)));
    
    next = loadoff(js, next) & ~(3U | CONSTMASK);
    
    if (streq(key, klen, "__proto__", 9)) continue;
    if (klen > 7 && memcmp(key, "__desc_", 7) == 0) continue;
    
    bool should_include = true;
    char desc_key[128];
    snprintf(desc_key, sizeof(desc_key), "__desc_%.*s", (int)klen, key);
    jsoff_t desc_off = lkp(js, obj, desc_key, strlen(desc_key));
    
    if (desc_off != 0) {
      jsval_t desc_obj = resolveprop(js, mkval(T_PROP, desc_off));
      if (vtype(desc_obj) == T_OBJ) {
        jsoff_t enum_off = lkp(js, desc_obj, "enumerable", 10);
        if (enum_off != 0) {
          jsval_t enum_val = resolveprop(js, mkval(T_PROP, enum_off));
          should_include = js_truthy(js, enum_val);
        }
      }
    }
    
    if (should_include) {
      jsval_t pair = mkarr(js);
      jsval_t key_val = js_mkstr(js, key, klen);
      setprop(js, pair, js_mkstr(js, "0", 1), key_val);
      setprop(js, pair, js_mkstr(js, "1", 1), val);
      setprop(js, pair, js_mkstr(js, "length", 6), tov(2.0));
      
      char idxstr[16];
      snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) idx);
      jsval_t idx_key = js_mkstr(js, idxstr, strlen(idxstr));
      setprop(js, arr, idx_key, mkval(T_ARR, vdata(pair)));
      idx++;
    }
  }
  
  for (jsoff_t i = 0; i < idx / 2; i++) {
    jsoff_t j = idx - 1 - i;
    char istr[16], jstr[16];
    snprintf(istr, sizeof(istr), "%u", (unsigned) i);
    snprintf(jstr, sizeof(jstr), "%u", (unsigned) j);
    jsoff_t ioff = lkp(js, arr, istr, strlen(istr));
    jsoff_t joff = lkp(js, arr, jstr, strlen(jstr));
    jsval_t iv = loadval(js, ioff + sizeof(jsoff_t) * 2);
    jsval_t jv = loadval(js, joff + sizeof(jsoff_t) * 2);
    saveval(js, ioff + sizeof(jsoff_t) * 2, jv);
    saveval(js, joff + sizeof(jsoff_t) * 2, iv);
  }
  
  jsval_t len_key = js_mkstr(js, "length", 6);
  jsval_t len_val = tov((double) idx);
  setprop(js, arr, len_key, len_val);
  return mkval(T_ARR, vdata(arr));
}

static jsval_t builtin_object_getPrototypeOf(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_mkerr(js, "Object.getPrototypeOf requires an argument");
  jsval_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t == T_STR || t == T_NUM || t == T_BOOL) return get_prototype_for_type(js, t);
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
  
  if (pt != T_NULL) {
    jsval_t cur = proto;
    int depth = 0;
    while (vtype(cur) != T_NULL && depth < 32) {
      if (vdata(cur) == vdata(obj)) return js_mkerr(js, "Cyclic __proto__ value");
      cur = get_proto(js, cur);
      depth++;
    }
  }
  
  set_proto(js, obj, proto);
  return obj;
}

static jsval_t builtin_object_create(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_mkerr(js, "Object.create requires a prototype argument");
  
  jsval_t proto = args[0];
  uint8_t pt = vtype(proto);
  
  if (pt != T_OBJ && pt != T_ARR && pt != T_FUNC && pt != T_NULL) {
    return js_mkerr(js, "Object.create: prototype must be an object or null");
  }
  
  jsval_t obj = js_mkobj(js);
  if (pt != T_NULL) {
    set_proto(js, obj, proto);
  }
  
  if (nargs >= 2 && vtype(args[1]) == T_OBJ) {
    jsval_t props = args[1];
    jsoff_t next = loadoff(js, (jsoff_t) vdata(props)) & ~(3U | CONSTMASK);
    
    while (next < js->brk && next != 0) {
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
      
      next = loadoff(js, next) & ~(3U | CONSTMASK);
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
  
  if (vtype(prop) != T_STR) {
    return js_mkerr(js, "Property key must be a string");
  }
  
  if (vtype(descriptor) != T_OBJ) {
    return js_mkerr(js, "Property descriptor must be an object");
  }
  
  jsval_t as_obj = (t == T_OBJ) ? obj : mkval(T_OBJ, vdata(obj));
  
  jsoff_t prop_len, prop_off = vstr(js, prop, &prop_len);
  const char *prop_str = (char *) &js->mem[prop_off];
  
  if (streq(prop_str, prop_len, "__proto__", 9)) {
    return js_mkerr(js, "Cannot define __proto__ property");
  }
  
  bool has_value = false, has_get = false, has_set = false, has_writable = false;
  jsval_t value = js_mkundef();
  bool writable = true, enumerable = false, configurable = false;
  
  jsoff_t value_off = lkp(js, descriptor, "value", 5);
  if (value_off != 0) {
    has_value = true;
    value = resolveprop(js, mkval(T_PROP, value_off));
  }
  
  jsoff_t get_off = lkp(js, descriptor, "get", 3);
  if (get_off != 0) {
    has_get = true;
    jsval_t getter = resolveprop(js, mkval(T_PROP, get_off));
    if (vtype(getter) != T_FUNC && vtype(getter) != T_UNDEF) {
      return js_mkerr(js, "Getter must be a function");
    }
  }
  
  jsoff_t set_off = lkp(js, descriptor, "set", 3);
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
  
  if (existing_off > 0) {
    if (is_const_prop(js, existing_off)) {
      return js_mkerr(js, "Cannot redefine non-configurable property");
    }
    
    if (has_value) {
      saveval(js, existing_off + sizeof(jsoff_t) * 2, value);
    }
    
    char desc_key[128];
    snprintf(desc_key, sizeof(desc_key), "__desc_%.*s", (int)prop_len, prop_str);
    jsval_t desc_key_str = js_mkstr(js, desc_key, strlen(desc_key));
    jsval_t desc_obj = js_mkobj(js);
    setprop(js, desc_obj, js_mkstr(js, "writable", 8), mkval(T_BOOL, writable ? 1 : 0));
    setprop(js, desc_obj, js_mkstr(js, "enumerable", 10), mkval(T_BOOL, enumerable ? 1 : 0));
    setprop(js, desc_obj, js_mkstr(js, "configurable", 12), mkval(T_BOOL, configurable ? 1 : 0));
    setprop(js, as_obj, desc_key_str, desc_obj);
    
    if (!configurable) {
      jsoff_t head = (jsoff_t) vdata(as_obj);
      jsoff_t firstprop = loadoff(js, head);
      if ((firstprop & ~(3U | CONSTMASK)) == existing_off) {
        saveoff(js, head, firstprop | CONSTMASK);
      }
    }
  } else {
    if (has_get || has_set) {
      return js_mkerr(js, "Accessor properties not fully supported");
    }
    
    if (!has_value) {
      value = js_mkundef();
    }
    
    jsval_t prop_key = js_mkstr(js, prop_str, prop_len);
    bool mark_const = (!writable || !configurable);
    mkprop(js, as_obj, prop_key, value, mark_const);
    
    char desc_key[128];
    snprintf(desc_key, sizeof(desc_key), "__desc_%.*s", (int)prop_len, prop_str);
    jsval_t desc_key_str = js_mkstr(js, desc_key, strlen(desc_key));
    jsval_t desc_obj = js_mkobj(js);
    setprop(js, desc_obj, js_mkstr(js, "writable", 8), mkval(T_BOOL, writable ? 1 : 0));
    setprop(js, desc_obj, js_mkstr(js, "enumerable", 10), mkval(T_BOOL, enumerable ? 1 : 0));
    setprop(js, desc_obj, js_mkstr(js, "configurable", 12), mkval(T_BOOL, configurable ? 1 : 0));
    setprop(js, as_obj, desc_key_str, desc_obj);
  }
  
  return obj;
}

static jsval_t builtin_object_toString(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t obj = js->this_val;
  
  obj = resolveprop(js, obj);
  uint8_t t = vtype(obj);
  
  if (t == T_OBJ || t == T_ARR || t == T_FUNC) {
    jsval_t check_obj = (t == T_FUNC) ? mkval(T_OBJ, vdata(obj)) : obj;
    jsoff_t tag_off = lkp(js, check_obj, "@@toStringTag", 13);
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

static jsval_t builtin_array_push(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  arr = resolveprop(js, arr);
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "push called on non-array");
  }
  
  jsoff_t off = lkp(js, arr, "length", 6);
  jsoff_t len = 0;
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  for (int i = 0; i < nargs; i++) {
    char idxstr[16];
    snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) len);
    jsval_t key = js_mkstr(js, idxstr, strlen(idxstr));
    setprop(js, arr, key, args[i]);
    len++;
  }
  
  jsval_t len_key = js_mkstr(js, "length", 6);
  jsval_t len_val = tov((double) len);
  
  setprop(js, arr, len_key, len_val);
  return len_val;
}

static jsval_t builtin_array_pop(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  jsval_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "pop called on non-array");
  }
  
  jsoff_t off = lkp(js, arr, "length", 6);
  jsoff_t len = 0;
  
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  if (len == 0) return js_mkundef();
  len--;
  char idxstr[16];
  snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) len);
  
  jsoff_t elem_off = lkp(js, arr, idxstr, strlen(idxstr));
  jsval_t result = js_mkundef();
  if (elem_off != 0) {
    result = resolveprop(js, mkval(T_PROP, elem_off));
  }
  
  jsval_t len_key = js_mkstr(js, "length", 6);
  jsval_t len_val = tov((double) len);
  setprop(js, arr, len_key, len_val);
  
  return result;
}

static jsval_t builtin_array_slice(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "slice called on non-array");
  }
  
  jsoff_t off = lkp(js, arr, "length", 6);
  jsoff_t len = 0;
  
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  jsoff_t start = 0, end = len;
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    double d = tod(args[0]);
    if (d < 0) {
      start = (jsoff_t) (d + len < 0 ? 0 : d + len);
    } else {
      start = (jsoff_t) (d > len ? len : d);
    }
  }
  
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    double d = tod(args[1]);
    if (d < 0) {
      end = (jsoff_t) (d + len < 0 ? 0 : d + len);
    } else {
      end = (jsoff_t) (d > len ? len : d);
    }
  }
  
  if (start > end) start = end;
  jsval_t result = mkarr(js);
  if (is_err(result)) return result;
  jsoff_t result_idx = 0;
  
  for (jsoff_t i = start; i < end; i++) {
    char idxstr[16];
    snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) i);
    jsoff_t elem_off = lkp(js, arr, idxstr, strlen(idxstr));
    if (elem_off != 0) {
      jsval_t elem = resolveprop(js, mkval(T_PROP, elem_off));
      snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) result_idx);
      jsval_t key = js_mkstr(js, idxstr, strlen(idxstr));
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
  
  jsoff_t off = lkp(js, arr, "length", 6);
  jsoff_t len = 0;
  
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  if (len == 0) return js_mkstr(js, "", 0);
  char result[1024] = "";
  size_t result_len = 0;
  
  for (jsoff_t i = 0; i < len; i++) {
    char idxstr[16];
    snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) i);
    jsoff_t elem_off = lkp(js, arr, idxstr, strlen(idxstr));
    
    if (i > 0 && result_len + sep_len < sizeof(result)) {
      memcpy(result + result_len, sep, sep_len);
      result_len += sep_len;
    }
    
    if (elem_off != 0) {
      jsval_t elem = resolveprop(js, mkval(T_PROP, elem_off));
      if (vtype(elem) == T_STR) {
        jsoff_t elem_len, elem_off_str = vstr(js, elem, &elem_len);
        if (result_len + elem_len < sizeof(result)) {
          memcpy(result + result_len, &js->mem[elem_off_str], elem_len);
          result_len += elem_len;
        }
      } else if (vtype(elem) == T_NUM) {
        char numstr[32];
        snprintf(numstr, sizeof(numstr), "%g", tod(elem));
        size_t num_len = strlen(numstr);
        if (result_len + num_len < sizeof(result)) {
          memcpy(result + result_len, numstr, num_len);
          result_len += num_len;
        }
      } else if (vtype(elem) == T_BOOL) {
        const char *boolstr = vdata(elem) ? "true" : "false";
        size_t bool_len = strlen(boolstr);
        if (result_len + bool_len < sizeof(result)) {
          memcpy(result + result_len, boolstr, bool_len);
          result_len += bool_len;
        }
      }
    }
  }
  return js_mkstr(js, result, result_len);
}

static jsval_t builtin_array_includes(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "includes called on non-array");
  }
  
  if (nargs == 0) return mkval(T_BOOL, 0);
  jsval_t search = args[0];
  jsoff_t off = lkp(js, arr, "length", 6);
  jsoff_t len = 0;
  
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  for (jsoff_t i = 0; i < len; i++) {
    char idxstr[16];
    snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) i);
    jsoff_t elem_off = lkp(js, arr, idxstr, strlen(idxstr));
    if (elem_off != 0) {
      jsval_t elem = resolveprop(js, mkval(T_PROP, elem_off));
      if (vtype(elem) == vtype(search)) {
        if (vtype(elem) == T_NUM && tod(elem) == tod(search)) {
          return mkval(T_BOOL, 1);
        } else if (vtype(elem) == T_BOOL && vdata(elem) == vdata(search)) {
          return mkval(T_BOOL, 1);
        } else if (vtype(elem) == T_STR) {
          jsoff_t elem_len, elem_off_str = vstr(js, elem, &elem_len);
          jsoff_t search_len, search_off = vstr(js, search, &search_len);
          if (elem_len == search_len && memcmp(&js->mem[elem_off_str], &js->mem[search_off], elem_len) == 0) {
            return mkval(T_BOOL, 1);
          }
        } else if ((vtype(elem) == T_OBJ || vtype(elem) == T_ARR || vtype(elem) == T_FUNC) && vdata(elem) == vdata(search)) {
          return mkval(T_BOOL, 1);
        }
      }
    }
  }
  return mkval(T_BOOL, 0);
}

static jsval_t builtin_array_every(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "every called on non-array");
  }
  
  if (nargs == 0 || vtype(args[0]) != T_FUNC) {
    return js_mkerr(js, "every requires a function argument");
  }
  
  jsval_t callback = args[0];
  jsoff_t off = lkp(js, arr, "length", 6);
  jsoff_t len = 0;
  
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  for (jsoff_t i = 0; i < len; i++) {
    char idxstr[16];
    snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) i);
    jsoff_t elem_off = lkp(js, arr, idxstr, strlen(idxstr));
    
    jsval_t elem = js_mkundef();
    if (elem_off != 0) {
      elem = resolveprop(js, mkval(T_PROP, elem_off));
    }
    
    jsval_t call_args[3] = { elem, tov((double)i), arr };
    jsval_t result = call_js_with_args(js, callback, call_args, 3);
    
    if (is_err(result)) return result;
    
    if (!js_truthy(js, result)) {
      return mkval(T_BOOL, 0);
    }
  }
  
  return mkval(T_BOOL, 1);
}

static jsval_t builtin_string_indexOf(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = js->this_val;
  if (vtype(str) != T_STR) return js_mkerr(js, "indexOf called on non-string");
  if (nargs == 0) return tov(-1);
  jsval_t search = args[0];
  
  if (vtype(search) != T_STR) return tov(-1);
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  jsoff_t search_len, search_off = vstr(js, search, &search_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  const char *search_ptr = (char *) &js->mem[search_off];
  
  for (jsoff_t i = 0; i <= str_len - search_len; i++) {
    if (memcmp(str_ptr + i, search_ptr, search_len) == 0) {
      return tov((double) i);
    }
  }
  
  return tov(-1);
}

static jsval_t builtin_string_substring(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = js->this_val;
  if (vtype(str) != T_STR) return js_mkerr(js, "substring called on non-string");
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  jsoff_t start = 0, end = str_len;
  
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    double d = tod(args[0]);
    start = (jsoff_t) (d < 0 ? 0 : (d > str_len ? str_len : d));
  }
  
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    double d = tod(args[1]);
    end = (jsoff_t) (d < 0 ? 0 : (d > str_len ? str_len : d));
  }
  
  if (start > end) {
    jsoff_t tmp = start;
    start = end;
    end = tmp;
  }
  
  jsoff_t sub_len = end - start;
  return js_mkstr(js, str_ptr + start, sub_len);
}

static jsval_t builtin_string_split(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = js->this_val;
  if (vtype(str) != T_STR) return js_mkerr(js, "split called on non-string");
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  jsval_t arr = mkarr(js);
  
  if (is_err(arr)) return arr;
  if (nargs == 0 || vtype(args[0]) != T_STR) {
    jsval_t key = js_mkstr(js, "0", 1);
    setprop(js, arr, key, str);
    jsval_t len_key = js_mkstr(js, "length", 6);
    setprop(js, arr, len_key, tov(1));
    return mkval(T_ARR, vdata(arr));
  }
  
  jsval_t sep = args[0];
  jsoff_t sep_len, sep_off = vstr(js, sep, &sep_len);
  const char *sep_ptr = (char *) &js->mem[sep_off];
  jsoff_t idx = 0;
  jsoff_t start = 0;
  
  if (sep_len == 0) {
    for (jsoff_t i = 0; i < str_len; i++) {
      char idxstr[16];
      snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) idx);
      jsval_t key = js_mkstr(js, idxstr, strlen(idxstr));
      jsval_t part = js_mkstr(js, str_ptr + i, 1);
      setprop(js, arr, key, part);
      idx++;
    }
  } else {
    for (jsoff_t i = 0; i <= str_len - sep_len; i++) {
      if (memcmp(str_ptr + i, sep_ptr, sep_len) == 0) {
        char idxstr[16];
        snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) idx);
        jsval_t key = js_mkstr(js, idxstr, strlen(idxstr));
        jsval_t part = js_mkstr(js, str_ptr + start, i - start);
        setprop(js, arr, key, part);
        idx++;
        start = i + sep_len;
        i += sep_len - 1;
      }
    }
    if (start <= str_len) {
      char idxstr[16];
      snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) idx);
      jsval_t key = js_mkstr(js, idxstr, strlen(idxstr));
      jsval_t part = js_mkstr(js, str_ptr + start, str_len - start);
      setprop(js, arr, key, part);
      idx++;
    }
  }
  
  jsval_t len_key = js_mkstr(js, "length", 6);
  setprop(js, arr, len_key, tov((double) idx));
  return mkval(T_ARR, vdata(arr));
}

static jsval_t builtin_string_slice(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = js->this_val;
  if (vtype(str) != T_STR) return js_mkerr(js, "slice called on non-string");
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  jsoff_t start = 0, end = str_len;
  
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    double d = tod(args[0]);
    if (d < 0) {
      start = (jsoff_t) (d + str_len < 0 ? 0 : d + str_len);
    } else {
      start = (jsoff_t) (d > str_len ? str_len : d);
    }
  }
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    double d = tod(args[1]);
    if (d < 0) {
      end = (jsoff_t) (d + str_len < 0 ? 0 : d + str_len);
    } else {
      end = (jsoff_t) (d > str_len ? str_len : d);
    }
  }
  
  if (start > end) start = end;
  jsoff_t sub_len = end - start;
  return js_mkstr(js, str_ptr + start, sub_len);
}

static jsval_t builtin_string_includes(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = js->this_val;
  if (vtype(str) != T_STR) return js_mkerr(js, "includes called on non-string");
  if (nargs == 0) return mkval(T_BOOL, 0);
  jsval_t search = args[0];
  
  if (vtype(search) != T_STR) return mkval(T_BOOL, 0);
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  jsoff_t search_len, search_off = vstr(js, search, &search_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  const char *search_ptr = (char *) &js->mem[search_off];
  
  if (search_len == 0) return mkval(T_BOOL, 1);
  for (jsoff_t i = 0; i <= str_len - search_len; i++) {
    if (memcmp(str_ptr + i, search_ptr, search_len) == 0) {
      return mkval(T_BOOL, 1);
    }
  }
  
  return mkval(T_BOOL, 0);
}

static jsval_t builtin_string_startsWith(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = js->this_val;
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
  jsval_t str = js->this_val;
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
  jsval_t str = js->this_val;
  if (vtype(str) != T_STR) return js_mkerr(js, "replace called on non-string");
  if (nargs < 2) return str;
  
  jsval_t search = args[0];
  jsval_t replacement = args[1];
  
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  
  bool is_regex = false;
  bool global_flag = false;
  bool is_func_replacement = (vtype(replacement) == T_FUNC);
  char pattern_buf[256];
  jsoff_t pattern_len = 0;
  
  if (vtype(search) == T_OBJ) {
    jsoff_t pattern_off = lkp(js, search, "source", 6);
    if (pattern_off == 0) goto not_regex;
    
    jsval_t pattern_val = resolveprop(js, mkval(T_PROP, pattern_off));
    if (vtype(pattern_val) != T_STR) goto not_regex;
    
    is_regex = true;
    jsoff_t plen, poff = vstr(js, pattern_val, &plen);
    pattern_len = plen < sizeof(pattern_buf) - 1 ? plen : sizeof(pattern_buf) - 1;
    memcpy(pattern_buf, &js->mem[poff], pattern_len);
    pattern_buf[pattern_len] = '\0';
    
    jsoff_t flags_off = lkp(js, search, "flags", 5);
    if (flags_off == 0) goto not_regex;
    
    jsval_t flags_val = resolveprop(js, mkval(T_PROP, flags_off));
    if (vtype(flags_val) != T_STR) goto not_regex;
    
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
    if (vtype(replacement) != T_STR) return str;
    jsoff_t repl_off;
    repl_off = vstr(js, replacement, &repl_len);
    repl_ptr = (char *) &js->mem[repl_off];
  }
  
  char result[4096];
  jsoff_t result_len = 0;
  
  if (is_regex) {
    regex_t regex;
    int cflags = REG_EXTENDED;
    
    if (regcomp(&regex, pattern_buf, cflags) != 0) {
      return js_mkerr(js, "invalid regex pattern");
    }
    
    char str_buf[4096];
    if (str_len >= sizeof(str_buf)) {
      regfree(&regex);
      return js_mkerr(js, "string too long");
    }
    memcpy(str_buf, str_ptr, str_len);
    str_buf[str_len] = '\0';
    
    jsoff_t pos = 0;
    bool replaced = false;
    
    while (pos < str_len) {
      regmatch_t match;
      int ret = regexec(&regex, str_buf + pos, 1, &match, 0);
      
      if (ret == 0 && match.rm_so >= 0) {
        jsoff_t before_len = (jsoff_t)match.rm_so;
        if (result_len + before_len < sizeof(result)) {
          memcpy(result + result_len, str_buf + pos, before_len);
          result_len += before_len;
        }
        
        jsoff_t match_len = (jsoff_t)(match.rm_eo - match.rm_so);
        
        if (is_func_replacement) {
          jsval_t match_str = js_mkstr(js, str_buf + pos + match.rm_so, match_len);
          jsval_t cb_args[1] = { match_str };
          jsval_t cb_result = js_call(js, replacement, cb_args, 1);
          
          if (vtype(cb_result) == T_ERR) {
            regfree(&regex);
            return cb_result;
          }
          
          if (vtype(cb_result) == T_STR) {
            jsoff_t cb_len, cb_off = vstr(js, cb_result, &cb_len);
            if (result_len + cb_len < sizeof(result)) {
              memcpy(result + result_len, &js->mem[cb_off], cb_len);
              result_len += cb_len;
            }
          } else {
            char numbuf[32];
            size_t n = tostr(js, cb_result, numbuf, sizeof(numbuf));
            if (result_len + n < sizeof(result)) {
              memcpy(result + result_len, numbuf, n);
              result_len += (jsoff_t)n;
            }
          }
        } else {
          if (result_len + repl_len < sizeof(result)) {
            memcpy(result + result_len, repl_ptr, repl_len);
            result_len += repl_len;
          }
        }
        
        if (match.rm_eo == 0) {
          if (pos < str_len && result_len < sizeof(result)) {
            result[result_len++] = str_buf[pos];
          }
          pos++;
        } else {
          pos += (jsoff_t)match.rm_eo;
        }
        
        replaced = true;
        
        if (!global_flag) break;
      } else {
        break;
      }
    }
    
    if (pos < str_len && result_len + (str_len - pos) < sizeof(result)) {
      memcpy(result + result_len, str_buf + pos, str_len - pos);
      result_len += str_len - pos;
    }
    
    regfree(&regex);
    return replaced ? js_mkstr(js, result, result_len) : str;
    
  } else {
    if (vtype(search) != T_STR) return str;
    jsoff_t search_len, search_off = vstr(js, search, &search_len);
    const char *search_ptr = (char *) &js->mem[search_off];
    
    if (search_len > str_len) return str;
    
    for (jsoff_t i = 0; i <= str_len - search_len; i++) {
      if (memcmp(str_ptr + i, search_ptr, search_len) == 0) {
        if (result_len + i < sizeof(result)) {
          memcpy(result + result_len, str_ptr, i);
          result_len += i;
        }
        
        if (is_func_replacement) {
          jsval_t match_str = js_mkstr(js, search_ptr, search_len);
          jsval_t cb_args[1] = { match_str };
          jsval_t cb_result = js_call(js, replacement, cb_args, 1);
          
          if (vtype(cb_result) == T_ERR) return cb_result;
          
          if (vtype(cb_result) == T_STR) {
            jsoff_t cb_len, cb_off = vstr(js, cb_result, &cb_len);
            if (result_len + cb_len < sizeof(result)) {
              memcpy(result + result_len, &js->mem[cb_off], cb_len);
              result_len += cb_len;
            }
          } else {
            char numbuf[32];
            size_t n = tostr(js, cb_result, numbuf, sizeof(numbuf));
            if (result_len + n < sizeof(result)) {
              memcpy(result + result_len, numbuf, n);
              result_len += (jsoff_t)n;
            }
          }
        } else {
          if (result_len + repl_len < sizeof(result)) {
            memcpy(result + result_len, repl_ptr, repl_len);
            result_len += repl_len;
          }
        }
        
        jsoff_t after_start = i + search_len;
        jsoff_t after_len = str_len - after_start;
        if (after_len > 0 && result_len + after_len < sizeof(result)) {
          memcpy(result + result_len, str_ptr + after_start, after_len);
          result_len += after_len;
        }
        return js_mkstr(js, result, result_len);
      }
    }
    return str;
  }
}

static jsval_t builtin_string_template(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = js->this_val;
  if (vtype(str) != T_STR) return js_mkerr(js, "template called on non-string");
  if (nargs < 1 || vtype(args[0]) != T_OBJ) return str;
  
  jsval_t data = args[0];
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  
  char result[2048];
  jsoff_t result_len = 0;
  jsoff_t i = 0;
  
  while (i < str_len && result_len < sizeof(result) - 1) {
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
            if (result_len + val_len < sizeof(result)) {
              memcpy(result + result_len, &js->mem[val_off], val_len);
              result_len += val_len;
            }
          } else if (vtype(value) == T_NUM) {
            char numstr[32];
            snprintf(numstr, sizeof(numstr), "%g", tod(value));
            size_t num_len = strlen(numstr);
            if (result_len + num_len < sizeof(result)) {
              memcpy(result + result_len, numstr, num_len);
              result_len += num_len;
            }
          } else if (vtype(value) == T_BOOL) {
            const char *boolstr = vdata(value) ? "true" : "false";
            size_t bool_len = strlen(boolstr);
            if (result_len + bool_len < sizeof(result)) {
              memcpy(result + result_len, boolstr, bool_len);
              result_len += bool_len;
            }
          }
        } else {
        }
        i = end + 2;
        continue;
      }
    }
    result[result_len++] = str_ptr[i++];
  }
  return js_mkstr(js, result, result_len);
}

static jsval_t builtin_string_charCodeAt(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = js->this_val;
  if (vtype(str) != T_STR) return js_mkerr(js, "charCodeAt called on non-string");
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(-NAN);
  
  double idx_d = tod(args[0]);
  if (idx_d < 0 || idx_d != (double)(long)idx_d) return tov(-NAN);
  
  jsoff_t idx = (jsoff_t) idx_d;
  jsoff_t str_len = offtolen(loadoff(js, (jsoff_t) vdata(str)));
  
  if (idx >= str_len) return tov(-NAN);
  
  jsoff_t str_off = (jsoff_t) vdata(str) + sizeof(jsoff_t);
  unsigned char ch = (unsigned char) js->mem[str_off + idx];
  
  return tov((double) ch);
}

static jsval_t builtin_string_toLowerCase(struct js *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  jsval_t str = js->this_val;
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
  jsval_t str = js->this_val;
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
  jsval_t str = js->this_val;
  if (vtype(str) != T_STR) return js_mkerr(js, "trim called on non-string");
  
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  
  jsoff_t start = 0, end = str_len;
  while (start < end && is_space(str_ptr[start])) start++;
  while (end > start && is_space(str_ptr[end - 1])) end--;
  
  return js_mkstr(js, str_ptr + start, end - start);
}

static jsval_t builtin_string_repeat(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = js->this_val;
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
  jsval_t str = js->this_val;
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
  jsval_t str = js->this_val;
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
  jsval_t str = js->this_val;
  if (vtype(str) != T_STR) return js_mkerr(js, "charAt called on non-string");
  if (nargs < 1 || vtype(args[0]) != T_NUM) return js_mkstr(js, "", 0);
  
  double idx_d = tod(args[0]);
  if (idx_d < 0 || idx_d != (double)(long)idx_d) return js_mkstr(js, "", 0);
  
  jsoff_t idx = (jsoff_t) idx_d;
  jsoff_t str_len = offtolen(loadoff(js, (jsoff_t) vdata(str)));
  
  if (idx >= str_len) return js_mkstr(js, "", 0);
  
  jsoff_t str_off = (jsoff_t) vdata(str) + sizeof(jsoff_t);
  char ch = js->mem[str_off + idx];
  
  return js_mkstr(js, &ch, 1);
}

static jsval_t builtin_number_toString(struct js *js, jsval_t *args, int nargs) {
  jsval_t num = js->this_val;
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
      *--p = digit < 10 ? '0' + digit : 'a' + (digit - 10);
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
      frac_buf[frac_pos++] = digit < 10 ? '0' + digit : 'a' + (digit - 10);
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
  jsval_t num = js->this_val;
  if (vtype(num) != T_NUM) return js_mkerr(js, "toFixed called on non-number");
  
  int digits = 0;
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    digits = (int) tod(args[0]);
    if (digits < 0) digits = 0;
    if (digits > 20) digits = 20;
  }
  
  char buf[64];
  snprintf(buf, sizeof(buf), "%.*f", digits, tod(num));
  return js_mkstr(js, buf, strlen(buf));
}

static jsval_t builtin_number_toPrecision(struct js *js, jsval_t *args, int nargs) {
  jsval_t num = js->this_val;
  if (vtype(num) != T_NUM) return js_mkerr(js, "toPrecision called on non-number");
  
  if (nargs < 1 || vtype(args[0]) != T_NUM) {
    char buf[64];
    size_t len = strnum(num, buf, sizeof(buf));
    return js_mkstr(js, buf, len);
  }
  
  int precision = (int) tod(args[0]);
  if (precision < 1) precision = 1;
  if (precision > 21) precision = 21;
  
  char buf[64];
  snprintf(buf, sizeof(buf), "%.*g", precision, tod(num));
  return js_mkstr(js, buf, strlen(buf));
}

static jsval_t builtin_number_toExponential(struct js *js, jsval_t *args, int nargs) {
  jsval_t num = js->this_val;
  if (vtype(num) != T_NUM) return js_mkerr(js, "toExponential called on non-number");
  
  int digits = 6;
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    digits = (int) tod(args[0]);
    if (digits < 0) digits = 0;
    if (digits > 20) digits = 20;
  }
  
  char buf[64];
  snprintf(buf, sizeof(buf), "%.*e", digits, tod(num));
  return js_mkstr(js, buf, strlen(buf));
}

static jsval_t builtin_parseInt(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return tov(-NAN);
  
  jsval_t str_val = args[0];
  if (vtype(str_val) != T_STR) {
    const char *str = js_str(js, str_val);
    str_val = js_mkstr(js, str, strlen(str));
  }
  
  jsoff_t str_len, str_off = vstr(js, str_val, &str_len);
  const char *str = (char *) &js->mem[str_off];
  
  int radix = 10;
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    radix = (int) tod(args[1]);
    if (radix < 2 || radix > 36) return tov(-NAN);
  }
  
  jsoff_t i = 0;
  while (i < str_len && is_space(str[i])) i++;
  
  if (i >= str_len) return tov(-NAN);
  
  int sign = 1;
  if (str[i] == '-') {
    sign = -1;
    i++;
  } else if (str[i] == '+') {
    i++;
  }
  
  if (radix == 16 && i + 1 < str_len && str[i] == '0' && (str[i + 1] == 'x' || str[i + 1] == 'X')) {
    i += 2;
  }
  
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
  
  if (!found_digit) return tov(-NAN);
  
  return tov(sign * result);
}

static jsval_t builtin_parseFloat(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return tov(-NAN);
  
  jsval_t str_val = args[0];
  if (vtype(str_val) != T_STR) {
    const char *str = js_str(js, str_val);
    str_val = js_mkstr(js, str, strlen(str));
  }
  
  jsoff_t str_len, str_off = vstr(js, str_val, &str_len);
  const char *str = (char *) &js->mem[str_off];
  
  jsoff_t i = 0;
  while (i < str_len && is_space(str[i])) i++;
  
  if (i >= str_len) return tov(-NAN);
  
  char *end;
  double result = strtod(&str[i], &end);
  
  if (end == &str[i]) return tov(-NAN);
  
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
  char *out = (char *)ANT_GC_MALLOC(out_len + 1);
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
  ANT_GC_FREE(out);
  return result;
}

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
  
  char *out = (char *)ANT_GC_MALLOC(out_len + 1);
  if (!out) return js_mkerr(js, "out of memory");
  
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
  
  size_t i = 0, j = 0;
  while (i < str_len) {
    int8_t a = decode_table[(unsigned char)str[i++]];
    int8_t b = decode_table[(unsigned char)str[i++]];
    int8_t c = (str[i] == '=') ? 0 : decode_table[(unsigned char)str[i]]; i++;
    int8_t d = (str[i] == '=') ? 0 : decode_table[(unsigned char)str[i]]; i++;
    
    if (a < 0 || b < 0 || (str[i-2] != '=' && c < 0) || (str[i-1] != '=' && d < 0)) {
      ANT_GC_FREE(out);
      return js_mkerr(js, "atob: invalid character in base64 string");
    }
    
    uint32_t triple = ((uint32_t)a << 18) | ((uint32_t)b << 12) | ((uint32_t)c << 6) | (uint32_t)d;
    if (j < out_len) out[j++] = (triple >> 16) & 0xFF;
    if (j < out_len) out[j++] = (triple >> 8) & 0xFF;
    if (j < out_len) out[j++] = triple & 0xFF;
  }
  
  jsval_t result = js_mkstr(js, out, out_len);
  ANT_GC_FREE(out);
  return result;
}

static jsval_t builtin_resolve_internal(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_reject_internal(struct js *js, jsval_t *args, int nargs);
static void resolve_promise(struct js *js, jsval_t p, jsval_t val);
static void reject_promise(struct js *js, jsval_t p, jsval_t val);

static size_t strpromise(struct js *js, jsval_t value, char *buf, size_t len) {
  jsval_t prom_obj = mkval(T_OBJ, vdata(value));
  jsoff_t state_off = lkp(js, prom_obj, "__state", 7);
  int state = 0;
  if (state_off != 0) {
    jsval_t val = resolveprop(js, mkval(T_PROP, state_off));
    if (vtype(val) == T_NUM) state = (int)tod(val);
  }
  
  if (state == 0) {
    return (size_t)snprintf(buf, len, "Promise { <pending> }");
  }
  
  jsoff_t value_off = lkp(js, prom_obj, "__value", 7);
  if (value_off == 0) {
    const char *s = (state == 1) ? "<fulfilled>" : "<rejected>";
    return (size_t)snprintf(buf, len, "Promise { %s }", s);
  }
  
  jsval_t prom_val = resolveprop(js, mkval(T_PROP, value_off));
  char value_buf[256];
  size_t value_len = tostr(js, prom_val, value_buf, sizeof(value_buf));
  if (value_len >= sizeof(value_buf)) value_len = sizeof(value_buf) - 1;
  value_buf[value_len] = '\0';
  
  if (state == 1) {
    return (size_t)snprintf(buf, len, "Promise { %s }", value_buf);
  } else {
    return (size_t)snprintf(buf, len, "Promise { <rejected> %s }", value_buf);
  }
}

static jsval_t mkpromise(struct js *js) {
  jsval_t obj = mkobj(js, 0);
  if (is_err(obj)) return obj;
  setprop(js, obj, js_mkstr(js, "__state", 7), tov(0.0));
  setprop(js, obj, js_mkstr(js, "__value", 7), js_mkundef());
  setprop(js, obj, js_mkstr(js, "__handlers", 10), mkarr(js));
  return mkval(T_PROMISE, vdata(obj));
}

static jsval_t builtin_trigger_handler_wrapper(struct js *js, jsval_t *args, int nargs);

static void trigger_handlers(struct js *js, jsval_t p) {
  jsval_t wrapper_obj = mkobj(js, 0);
  setprop(js, wrapper_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_trigger_handler_wrapper));
  setprop(js, wrapper_obj, js_mkstr(js, "promise", 7), p);
  jsval_t wrapper_fn = mkval(T_FUNC, vdata(wrapper_obj));
  queue_microtask(js, wrapper_fn);
}

static jsval_t builtin_trigger_handler_wrapper(struct js *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t p = js_get(js, me, "promise");
  if (vtype(p) != T_PROMISE) return js_mkundef();
  
  jsval_t p_obj = mkval(T_OBJ, vdata(p));
  jsoff_t state_off = lkp(js, p_obj, "__state", 7);
  int state = (int)tod(resolveprop(js, mkval(T_PROP, state_off)));
  
  jsoff_t val_off = lkp(js, p_obj, "__value", 7);
  jsval_t val = resolveprop(js, mkval(T_PROP, val_off));
  
  jsoff_t handlers_off = lkp(js, p_obj, "__handlers", 10);
  jsval_t handlers_arr = resolveprop(js, mkval(T_PROP, handlers_off));
  
  jsoff_t len_off = lkp(js, handlers_arr, "length", 6);
  int len = (int)tod(resolveprop(js, mkval(T_PROP, len_off)));
  
  for (int i = 0; i < len; i += 3) {
    char idx1[16], idx2[16], idx3[16];
    snprintf(idx1, sizeof(idx1), "%d", i);
    snprintf(idx2, sizeof(idx2), "%d", i+1);
    snprintf(idx3, sizeof(idx3), "%d", i+2);
    
    jsval_t onFulfilled = resolveprop(js, js_get(js, handlers_arr, idx1));
    jsval_t onRejected = resolveprop(js, js_get(js, handlers_arr, idx2));
    jsval_t nextPromise = resolveprop(js, js_get(js, handlers_arr, idx3));
    
    jsval_t handler = (state == 1) ? onFulfilled : onRejected;
    
    if (vtype(handler) == T_FUNC || vtype(handler) == T_CFUNC) {
       jsval_t res;
       if (vtype(handler) == T_CFUNC) {
          jsval_t (*fn)(struct js *, jsval_t *, int) = (jsval_t(*)(struct js *, jsval_t *, int)) vdata(handler);
          res = fn(js, &val, 1);
       } else {
          jsval_t args[] = { val };
          res = js_call(js, handler, args, 1);
       }
       
       if (is_err(res)) {
          jsval_t err_str = js_mkstr(js, js->errmsg, strlen(js->errmsg));
          reject_promise(js, nextPromise, err_str);
       } else {
          resolve_promise(js, nextPromise, res);
       }
    } else {
       if (state == 1) resolve_promise(js, nextPromise, val);
       else reject_promise(js, nextPromise, val);
    }
  }
  setprop(js, p_obj, js_mkstr(js, "__handlers", 10), mkarr(js));
  return js_mkundef();
}

static void resolve_promise(struct js *js, jsval_t p, jsval_t val) {
  jsval_t p_obj = mkval(T_OBJ, vdata(p));
  jsoff_t state_off = lkp(js, p_obj, "__state", 7);
  if (state_off == 0) {
    return;
  }
  jsval_t state_val = resolveprop(js, mkval(T_PROP, state_off));
  if ((int)tod(state_val) != 0) {
    return;
  }

  if (vtype(val) == T_PROMISE) {
     if (vdata(val) == vdata(p)) {
        jsval_t err = js_mkerr(js, "TypeError: Chaining cycle");
        reject_promise(js, p, err);
        return;
     }
     jsval_t res_obj = mkobj(js, 0);
     setprop(js, res_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_resolve_internal));
     setprop(js, res_obj, js_mkstr(js, "promise", 7), p);
     jsval_t res_fn = mkval(T_FUNC, vdata(res_obj));
     
     jsval_t rej_obj = mkobj(js, 0);
     setprop(js, rej_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_reject_internal));
     setprop(js, rej_obj, js_mkstr(js, "promise", 7), p);
     jsval_t rej_fn = mkval(T_FUNC, vdata(rej_obj));
     
     jsval_t args[] = { res_fn, rej_fn };
     jsval_t then_prop = js_get(js, val, "then");
     if (vtype(then_prop) == T_FUNC || vtype(then_prop) == T_CFUNC) {
         jsval_t saved_this = js->this_val;
         js->this_val = val;
         js_call(js, then_prop, args, 2);
         js->this_val = saved_this;
         return;
     }
  }

  setprop(js, p_obj, js_mkstr(js, "__state", 7), tov(1.0));
  setprop(js, p_obj, js_mkstr(js, "__value", 7), val);
  trigger_handlers(js, p);
}

static void reject_promise(struct js *js, jsval_t p, jsval_t val) {
  jsval_t p_obj = mkval(T_OBJ, vdata(p));
  jsoff_t state_off = lkp(js, p_obj, "__state", 7);
  if (state_off == 0) return;
  jsval_t state_val = resolveprop(js, mkval(T_PROP, state_off));
  if ((int)tod(state_val) != 0) return;

  setprop(js, p_obj, js_mkstr(js, "__state", 7), tov(2.0));
  setprop(js, p_obj, js_mkstr(js, "__value", 7), val);
  trigger_handlers(js, p);
}

static jsval_t builtin_resolve_internal(struct js *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t p = js_get(js, me, "promise");
  if (vtype(p) != T_PROMISE) return js_mkundef();
  resolve_promise(js, p, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static jsval_t builtin_reject_internal(struct js *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t p = js_get(js, me, "promise");
  if (vtype(p) != T_PROMISE) return js_mkundef();
  reject_promise(js, p, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static jsval_t builtin_promise_executor_wrapper(struct js *js, jsval_t *args, int nargs);

static jsval_t builtin_Promise(struct js *js, jsval_t *args, int nargs) {
  jsval_t p = mkpromise(js);
  jsval_t res_obj = mkobj(js, 0);
  setprop(js, res_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_resolve_internal));
  setprop(js, res_obj, js_mkstr(js, "promise", 7), p);
  jsval_t res_fn = mkval(T_FUNC, vdata(res_obj));
  jsval_t rej_obj = mkobj(js, 0);
  setprop(js, rej_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_reject_internal));
  setprop(js, rej_obj, js_mkstr(js, "promise", 7), p);
  jsval_t rej_fn = mkval(T_FUNC, vdata(rej_obj));
  if (nargs > 0) {
     jsval_t wrapper_obj = mkobj(js, 0);
     setprop(js, wrapper_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_promise_executor_wrapper));
     setprop(js, wrapper_obj, js_mkstr(js, "executor", 8), args[0]);
     setprop(js, wrapper_obj, js_mkstr(js, "resolve", 7), res_fn);
     setprop(js, wrapper_obj, js_mkstr(js, "reject", 6), rej_fn);
     jsval_t wrapper_fn = mkval(T_FUNC, vdata(wrapper_obj));
     queue_microtask(js, wrapper_fn);
  }
  return p;
}

static jsval_t builtin_promise_executor_wrapper(struct js *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t executor = js_get(js, me, "executor");
  jsval_t res_fn = js_get(js, me, "resolve");
  jsval_t rej_fn = js_get(js, me, "reject");
  jsval_t exec_args[] = { res_fn, rej_fn };
  js_call(js, executor, exec_args, 2);
  return js_mkundef();
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
  jsval_t onFulfilled = nargs > 0 ? args[0] : js_mkundef();
  jsval_t onRejected = nargs > 1 ? args[1] : js_mkundef();
  jsval_t p_obj = mkval(T_OBJ, vdata(p));
  jsval_t handlers = resolveprop(js, js_get(js, p_obj, "__handlers"));
  jsval_t push_args[] = { onFulfilled, onRejected, nextP };
  jsval_t saved_this = js->this_val;
  js->this_val = handlers;
  builtin_array_push(js, push_args, 3);
  js->this_val = saved_this;
  jsoff_t state_off = lkp(js, p_obj, "__state", 7);
  int state = (int)tod(resolveprop(js, mkval(T_PROP, state_off)));
  if (state != 0) trigger_handlers(js, p);
  return nextP;
}

static jsval_t builtin_promise_catch(struct js *js, jsval_t *args, int nargs) {
  jsval_t args_then[] = { js_mkundef(), nargs > 0 ? args[0] : js_mkundef() };
  return builtin_promise_then(js, args_then, 2);
}

static jsval_t builtin_promise_finally(struct js *js, jsval_t *args, int nargs) {
  jsval_t fn = nargs > 0 ? args[0] : js_mkundef();
  jsval_t args_then[] = { fn, fn };
  return builtin_promise_then(js, args_then, 2);
}

static jsval_t builtin_Promise_try(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return builtin_Promise_resolve(js, args, 0);
  jsval_t fn = args[0];
  jsval_t res = js_call(js, fn, NULL, 0);
  if (is_err(res)) {
     jsval_t err_str = js_mkstr(js, js->errmsg, strlen(js->errmsg));
     jsval_t rej_args[] = { err_str };
     return builtin_Promise_reject(js, rej_args, 1);
  }
  jsval_t res_args[] = { res };
  return builtin_Promise_resolve(js, res_args, 1);
}

static jsval_t resume_coroutine_wrapper(struct js *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t coro_val = js_get(js, me, "__coroutine");
  if (vtype(coro_val) != T_NUM) return js_mkundef();
  
  coroutine_t *coro = (coroutine_t *)(uintptr_t)tod(coro_val);
  if (!coro) return js_mkundef();
  
  
  coro->result = nargs > 0 ? args[0] : js_mkundef();
  coro->is_settled = true;
  coro->is_error = false;
  coro->is_ready = true;
  
  return js_mkundef();
}

static jsval_t reject_coroutine_wrapper(struct js *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t coro_val = js_get(js, me, "__coroutine");
  
  if (vtype(coro_val) != T_NUM) return js_mkundef();
  
  coroutine_t *coro = (coroutine_t *)(uintptr_t)tod(coro_val);
  if (!coro) return js_mkundef();
  
  coro->result = nargs > 0 ? args[0] : js_mkundef();
  coro->is_settled = true;
  coro->is_error = true;
  coro->is_ready = true;
  
  return js_mkundef();
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
    jsval_t result_promise = js_get(js, tracker, "promise");
    resolve_promise(js, result_promise, mkval(T_ARR, vdata(results)));
  }
  
  return js_mkundef();
}

static jsval_t builtin_Promise_all_reject_handler(struct js *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t tracker = js_get(js, me, "tracker");
  jsval_t result_promise = js_get(js, tracker, "promise");
  
  jsval_t reason = nargs > 0 ? args[0] : js_mkundef();
  reject_promise(js, result_promise, reason);
  
  return js_mkundef();
}

static jsval_t builtin_Promise_all(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Promise.all requires an array");
  
  jsval_t arr = args[0];
  if (vtype(arr) != T_ARR) return js_mkerr(js, "Promise.all requires an array");
  
  jsoff_t len_off = lkp(js, arr, "length", 6);
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
  setprop(js, tracker, js_mkstr(js, "promise", 7), result_promise);
  
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
    setprop(js, resolve_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_Promise_all_resolve_handler));
    setprop(js, resolve_obj, js_mkstr(js, "index", 5), tov((double)i));
    setprop(js, resolve_obj, js_mkstr(js, "tracker", 7), tracker);
    jsval_t resolve_fn = mkval(T_FUNC, vdata(resolve_obj));
    
    jsval_t reject_obj = mkobj(js, 0);
    setprop(js, reject_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_Promise_all_reject_handler));
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
  
  jsoff_t len_off = lkp(js, arr, "length", 6);
  if (len_off == 0) return mkpromise(js);
  jsval_t len_val = resolveprop(js, mkval(T_PROP, len_off));
  int len = (int)tod(len_val);
  
  if (len == 0) return mkpromise(js);
  jsval_t result_promise = mkpromise(js);
  
  jsval_t resolve_obj = mkobj(js, 0);
  setprop(js, resolve_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_resolve_internal));
  setprop(js, resolve_obj, js_mkstr(js, "promise", 7), result_promise);
  jsval_t resolve_fn = mkval(T_FUNC, vdata(resolve_obj));
  
  jsval_t reject_obj = mkobj(js, 0);
  setprop(js, reject_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_reject_internal));
  setprop(js, reject_obj, js_mkstr(js, "promise", 7), result_promise);
  jsval_t reject_fn = mkval(T_FUNC, vdata(reject_obj));
  
  for (int i = 0; i < len; i++) {
    char idx[16];
    snprintf(idx, sizeof(idx), "%d", i);
    jsval_t item = resolveprop(js, js_get(js, arr, idx));
    
    if (vtype(item) != T_PROMISE) {
      resolve_promise(js, result_promise, item);
      return result_promise;
    }
    
    jsval_t item_obj = mkval(T_OBJ, vdata(item));
    jsoff_t state_off = lkp(js, item_obj, "__state", 7);
    int state = (int)tod(resolveprop(js, mkval(T_PROP, state_off)));
    
    if (state == 1) {
      jsoff_t val_off = lkp(js, item_obj, "__value", 7);
      jsval_t val = resolveprop(js, mkval(T_PROP, val_off));
      resolve_promise(js, result_promise, val);
      return result_promise;
    } else if (state == 2) {
      jsoff_t val_off = lkp(js, item_obj, "__value", 7);
      jsval_t val = resolveprop(js, mkval(T_PROP, val_off));
      reject_promise(js, result_promise, val);
      return result_promise;
    }
    
    jsval_t then_args[] = { resolve_fn, reject_fn };
    jsval_t saved_this = js->this_val;
    js->this_val = item;
    builtin_promise_then(js, then_args, 2);
    js->this_val = saved_this;
  }
  
  return result_promise;
}

static jsval_t do_instanceof(struct js *js, jsval_t l, jsval_t r) {
  uint8_t ltype = vtype(l);
  
  if (vtype(r) != T_FUNC) {
    // handle legacy T_CFUNC
    if (vtype(r) == T_CFUNC) {
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
    }
    return mkval(T_BOOL, 0);
  }
  
  jsval_t func_obj = mkval(T_OBJ, vdata(r));
  jsoff_t proto_off = lkp(js, func_obj, "prototype", 9);
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
  if (vtype(l) != T_STR) {
    return js_mkerr(js, "left operand of 'in' must be string");
  }
  
  if (vtype(r) != T_OBJ && vtype(r) != T_ARR && vtype(r) != T_FUNC) {
    return js_mkerr(js, "right operand of 'in' must be object");
  }
  
  jsoff_t prop_len;
  jsoff_t prop_off = vstr(js, l, &prop_len);
  const char *prop_name = (char *) &js->mem[prop_off];
  
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
  return realpath(path, NULL);
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
  if ((result = esm_try_resolve(dir, spec, ".json"))) goto cleanup_ext;
  
  char idx[PATH_MAX];
  snprintf(idx, PATH_MAX, "%s/index%s", spec, base_ext);
  if ((result = esm_try_resolve(dir, idx, ""))) goto cleanup_ext;
  
  if (strcmp(base_ext, ".js") != 0) {
    snprintf(idx, PATH_MAX, "%s/index.js", spec);
    if ((result = esm_try_resolve(dir, idx, ""))) goto cleanup_ext;
  }
  
cleanup_ext:
  free(base_ext);
cleanup:
  free(base_copy);
  return result;
}

static bool esm_is_json(const char *path) {
  size_t len = strlen(path);
  return len > 5 && strcmp(path + len - 5, ".json") == 0;
}

static bool esm_is_text(const char *path) {
  size_t len = strlen(path);
  return (len > 4 && strcmp(path + len - 4, ".txt") == 0) ||
         (len > 3 && strcmp(path + len - 3, ".md") == 0) ||
         (len > 5 && strcmp(path + len - 5, ".html") == 0) ||
         (len > 4 && strcmp(path + len - 4, ".css") == 0);
}

static bool esm_is_image(const char *path) {
  size_t len = strlen(path);
  return (len > 4 && strcmp(path + len - 4, ".png") == 0) ||
         (len > 4 && strcmp(path + len - 4, ".jpg") == 0) ||
         (len > 5 && strcmp(path + len - 5, ".jpeg") == 0) ||
         (len > 4 && strcmp(path + len - 4, ".gif") == 0) ||
         (len > 4 && strcmp(path + len - 4, ".svg") == 0) ||
         (len > 5 && strcmp(path + len - 5, ".webp") == 0);
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
  char *canonical_path = esm_canonicalize_path(resolved_path);
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
    free(current);
  }
  global_module_cache.count = 0;
}

static jsval_t esm_load_json(struct js *js, const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return js_mkerr(js, "Cannot open JSON file: %s", path);
  
  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  
  char *content = (char *)malloc(size + 1);
  if (!content) {
    fclose(fp);
    return js_mkerr(js, "OOM loading JSON");
  }
  
  fread(content, 1, size, fp);
  fclose(fp);
  content[size] = '\0';
  
  jsval_t result = js_eval(js, content, size);
  free(content);
  
  return result;
}

static jsval_t esm_load_text(struct js *js, const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return js_mkerr(js, "Cannot open text file: %s", path);
  
  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  
  char *content = (char *)malloc(size + 1);
  if (!content) {
    fclose(fp);
    return js_mkerr(js, "OOM loading text");
  }
  
  fread(content, 1, size, fp);
  fclose(fp);
  content[size] = '\0';
  
  jsval_t result = js_mkstr(js, content, size);
  free(content);
  
  return result;
}

static jsval_t esm_load_image(struct js *js, const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return js_mkerr(js, "Cannot open image file: %s", path);
  
  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  
  unsigned char *content = (unsigned char *)malloc(size);
  if (!content) {
    fclose(fp);
    return js_mkerr(js, "OOM loading image");
  }
  
  fread(content, 1, size, fp);
  fclose(fp);
  
  jsval_t obj = mkobj(js, 0);
  jsval_t data_arr = mkarr(js);
  
  for (long i = 0; i < size; i++) {
    char idx[16];
    snprintf(idx, sizeof(idx), "%ld", i);
    setprop(js, data_arr, js_mkstr(js, idx, strlen(idx)), tov((double)content[i]));
  }
  setprop(js, data_arr, js_mkstr(js, "length", 6), tov((double)size));
  
  setprop(js, obj, js_mkstr(js, "data", 4), mkval(T_ARR, vdata(data_arr)));
  setprop(js, obj, js_mkstr(js, "path", 4), js_mkstr(js, path, strlen(path)));
  setprop(js, obj, js_mkstr(js, "size", 4), tov((double)size));
  
  free(content);
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
    
    jsval_t ns = mkobj(js, 0);
    setprop(js, ns, js_mkstr(js, "default", 7), json_val);
    mod->namespace_obj = ns;
    mod->default_export = json_val;
    mod->is_loaded = true;
    mod->is_loading = false;
    return ns;
  }
  
  if (mod->is_text) {
    jsval_t text_val = esm_load_text(js, mod->resolved_path);
    if (is_err(text_val)) {
      mod->is_loading = false;
      return text_val;
    }
    
    jsval_t ns = mkobj(js, 0);
    setprop(js, ns, js_mkstr(js, "default", 7), text_val);
    mod->namespace_obj = ns;
    mod->default_export = text_val;
    mod->is_loaded = true;
    mod->is_loading = false;
    return ns;
  }
  
  if (mod->is_image) {
    jsval_t img_val = esm_load_image(js, mod->resolved_path);
    if (is_err(img_val)) {
      mod->is_loading = false;
      return img_val;
    }
    
    jsval_t ns = mkobj(js, 0);
    setprop(js, ns, js_mkstr(js, "default", 7), img_val);
    mod->namespace_obj = ns;
    mod->default_export = img_val;
    mod->is_loaded = true;
    mod->is_loading = false;
    return ns;
  }
  
  FILE *fp = fopen(mod->resolved_path, "rb");
  if (!fp) {
    mod->is_loading = false;
    return js_mkerr(js, "Cannot open module: %s", mod->resolved_path);
  }
  
  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  
  char *content = (char *)malloc(size + 1);
  if (!content) {
    fclose(fp);
    mod->is_loading = false;
    return js_mkerr(js, "OOM loading module");
  }
  
  fread(content, 1, size, fp);
  fclose(fp);
  content[size] = '\0';
  
  jsval_t ns = mkobj(js, 0);
  mod->namespace_obj = ns;
  
  jsval_t glob = js_glob(js);
  jsval_t module_scope = js_get(js, glob, "__esm_module_scope");
  jsval_t prev_module = module_scope;
  js_set(js, glob, "__esm_module_scope", ns);
  
  const char *prev_filename = js->filename;
  jsval_t saved_scope = js->scope;
  
  js_set_filename(js, mod->resolved_path);
  mkscope(js);
  
  jsval_t result = js_eval(js, content, size);
  
  free(content);
  
  js->scope = saved_scope;
  js_set_filename(js, prev_filename);
  js_set(js, glob, "__esm_module_scope", prev_module);
  
  if (is_err(result)) {
    mod->is_loading = false;
    return result;
  }
  
  jsoff_t default_off = lkp(js, ns, "default", 7);
  if (default_off != 0) {
    mod->default_export = resolveprop(js, mkval(T_PROP, default_off));
  }
  
  mod->is_loaded = true;
  mod->is_loading = false;
  
  return ns;
}

static jsval_t builtin_import(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_STR) {
    return js_mkerr(js, "import() requires a string specifier");
  }
  
  jsoff_t spec_len;
  jsoff_t spec_off = vstr(js, args[0], &spec_len);
  const char *specifier = (char *)&js->mem[spec_off];
  
  ant_library_t *lib = find_library(specifier, spec_len);
  if (lib) {
    jsval_t lib_obj = lib->init_fn(js);
    if (is_err(lib_obj)) return builtin_Promise_reject(js, &lib_obj, 1);
    jsval_t promise_args[] = { lib_obj };
    return builtin_Promise_resolve(js, promise_args, 1);
  }
  
  const char *base_path = js->filename ? js->filename : ".";
  char *resolved_path = esm_resolve_path(specifier, base_path);
  if (!resolved_path) {
    return js_mkerr(js, "Cannot resolve module: %.*s", (int)spec_len, specifier);
  }
  
  esm_module_t *mod = esm_find_module(resolved_path);
  if (!mod) {
    mod = esm_create_module(specifier, resolved_path);
    if (!mod) {
      free(resolved_path);
      return js_mkerr(js, "Cannot create module");
    }
  }
  
  jsval_t ns = esm_load_module(js, mod);
  free(resolved_path);
  
  if (is_err(ns)) return builtin_Promise_reject(js, &ns, 1);
  
  jsval_t promise_args[] = { ns };
  return builtin_Promise_resolve(js, promise_args, 1);
}

static jsval_t builtin_import_meta_resolve(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_STR) {
    return js_mkerr(js, "import.meta.resolve() requires a string specifier");
  }
  
  jsoff_t spec_len;
  jsoff_t spec_off = vstr(js, args[0], &spec_len);
  const char *specifier = (char *)&js->mem[spec_off];
  
  const char *base_path = js->filename ? js->filename : ".";
  char *resolved_path = esm_resolve_path(specifier, base_path);
  if (!resolved_path) {
    return js_mkerr(js, "Cannot resolve module: %.*s", (int)spec_len, specifier);
  }
  
  size_t url_len = strlen(resolved_path) + 8;
  char *url = malloc(url_len);
  if (!url) {
    free(resolved_path);
    return js_mkerr(js, "oom");
  }
  
  snprintf(url, url_len, "file://%s", resolved_path);
  free(resolved_path);
  
  jsval_t result = js_mkstr(js, url, strlen(url));
  free(url);
  
  return result;
}

void js_setup_import_meta(struct js *js, const char *filename) {
  if (!filename) return;
  
  jsval_t import_meta = mkobj(js, 0);
  if (is_err(import_meta)) return;
  
  size_t url_len = strlen(filename) + 8;
  char *url = malloc(url_len);
  if (url) {
    snprintf(url, url_len, "file://%s", filename);
    jsval_t url_val = js_mkstr(js, url, strlen(url));
    if (!is_err(url_val)) setprop(js, import_meta, js_mkstr(js, "url", 3), url_val);
    free(url);
  }
  
  jsval_t filename_val = js_mkstr(js, filename, strlen(filename));
  if (!is_err(filename_val)) setprop(js, import_meta, js_mkstr(js, "filename", 8), filename_val);
  
  char *filename_copy = strdup(filename);
  if (filename_copy) {
    char *dir = dirname(filename_copy);
    if (dir) {
      jsval_t dirname_val = js_mkstr(js, dir, strlen(dir));
      if (!is_err(dirname_val)) setprop(js, import_meta, js_mkstr(js, "dirname", 7), dirname_val);
    }
    free(filename_copy);
  }
  
  setprop(js, import_meta, js_mkstr(js, "main", 4), js_mktrue());
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
    EXPECT(TOK_RPAREN, );
    
    if (vtype(spec) != T_STR) {
      return js_mkerr(js, "import() requires string");
    }
    
    jsval_t args[] = { spec };
    return builtin_import(js, args, 1);
  }
  
  if (next(js) == TOK_MUL) {
    js->consumed = 1;
    EXPECT(TOK_AS, );
    EXPECT(TOK_IDENTIFIER, );
    
    const char *namespace_name = &js->code[js->toff];
    size_t namespace_len = js->tlen;
    js->consumed = 1;
    
    EXPECT(TOK_FROM, );
    EXPECT(TOK_STRING, );
    
    jsval_t spec = js_str_literal(js);
    
    jsoff_t spec_len;
    jsoff_t spec_off = vstr(js, spec, &spec_len);
    const char *specifier = (char *)&js->mem[spec_off];
    
    jsval_t ns = js_mkundef();
    ant_library_t *lib = find_library(specifier, spec_len);
    if (lib) {
     ns = lib->init_fn(js);
    } else {
      const char *base_path = js->filename ? js->filename : ".";
      char *resolved_path = esm_resolve_path(specifier, base_path);
      if (!resolved_path) {
        return js_mkerr(js, "Cannot resolve module: %.*s", (int)spec_len, specifier);
      }
      
      esm_module_t *mod = esm_find_module(resolved_path);
      if (!mod) {
        mod = esm_create_module(specifier, resolved_path);
        if (!mod) {
          free(resolved_path);
          return js_mkerr(js, "Cannot create module");
        }
      }
      
      const char *saved_code = js->code;
      jsoff_t saved_clen = js->clen;
      jsoff_t saved_pos = js->pos;
      
      ns = esm_load_module(js, mod);
      
      js->code = saved_code;
      js->clen = saved_clen;
      js->pos = saved_pos;
      
      free(resolved_path);
    }
    
    js->consumed = 1; next(js); js->consumed = 0;
    if (is_err(ns)) return ns;
    
    setprop(js, js->scope, js_mkstr(js, namespace_name, namespace_len), ns);
    return js_mkundef();
  }
  
  if (next(js) == TOK_IDENTIFIER) {
    const char *default_name = &js->code[js->toff];
    size_t default_len = js->tlen;
    js->consumed = 1;
    
    EXPECT(TOK_FROM, );
    EXPECT(TOK_STRING, );
    
    jsval_t spec = js_str_literal(js);
    
    jsoff_t spec_len;
    jsoff_t spec_off = vstr(js, spec, &spec_len);
    const char *specifier = (char *)&js->mem[spec_off];
    
    jsval_t ns = js_mkundef();
    ant_library_t *lib = find_library(specifier, spec_len);
    if (lib) {
      ns = lib->init_fn(js);
    } else {
      const char *base_path = js->filename ? js->filename : ".";
      char *resolved_path = esm_resolve_path(specifier, base_path);
      if (!resolved_path) {
        return js_mkerr(js, "Cannot resolve module: %.*s", (int)spec_len, specifier);
      }
      
      esm_module_t *mod = esm_find_module(resolved_path);
      if (!mod) {
        mod = esm_create_module(specifier, resolved_path);
        if (!mod) {
          free(resolved_path);
          return js_mkerr(js, "Cannot create module");
        }
      }
      
      const char *saved_code = js->code;
      jsoff_t saved_clen = js->clen;
      jsoff_t saved_pos = js->pos;
      
      ns = esm_load_module(js, mod);
      
      js->code = saved_code;
      js->clen = saved_clen;
      js->pos = saved_pos;
      
      free(resolved_path);
    }
    
    js->consumed = 1; next(js); js->consumed = 0;
    if (is_err(ns)) return ns;
    
    jsoff_t default_off = lkp(js, ns, "default", 7);
    jsval_t default_val = default_off != 0 ? resolveprop(js, mkval(T_PROP, default_off)) : js_mkundef();
    
    setprop(js, js->scope, js_mkstr(js, default_name, default_len), default_val);
    return js_mkundef();
  }
  
  if (next(js) == TOK_LBRACE) {
    js->consumed = 1;
    
    typedef struct {
      const char *import_name;
      size_t import_len;
      const char *local_name;
      size_t local_len;
    } import_binding_t;
    
    import_binding_t bindings[64];
    int binding_count = 0;
    
    while (next(js) != TOK_RBRACE && binding_count < 64) {
      EXPECT(TOK_IDENTIFIER, );
      const char *import_name = &js->code[js->toff];
      size_t import_len = js->tlen;
      js->consumed = 1;
      
      const char *local_name = import_name;
      size_t local_len = import_len;
      
      if (next(js) == TOK_AS) {
        js->consumed = 1;
        EXPECT(TOK_IDENTIFIER, );
        local_name = &js->code[js->toff];
        local_len = js->tlen;
        js->consumed = 1;
      }
      
      bindings[binding_count].import_name = import_name;
      bindings[binding_count].import_len = import_len;
      bindings[binding_count].local_name = local_name;
      bindings[binding_count].local_len = local_len;
      binding_count++;
      
      if (next(js) == TOK_COMMA) js->consumed = 1;
    }
    
    EXPECT(TOK_RBRACE, );
    EXPECT(TOK_FROM, );
    EXPECT(TOK_STRING, );
    
    jsval_t spec = js_str_literal(js);
    jsoff_t spec_len;
    
    jsoff_t spec_off = vstr(js, spec, &spec_len);
    const char *specifier = (char *)&js->mem[spec_off];
    
    jsval_t ns = js_mkundef();
    ant_library_t *lib = find_library(specifier, spec_len);
    if (lib) {
      ns = lib->init_fn(js);
    } else {
      const char *base_path = js->filename ? js->filename : ".";
      char *resolved_path = esm_resolve_path(specifier, base_path);
      if (!resolved_path) return js_mkerr(js, "Cannot resolve module: %.*s", (int)spec_len, specifier);
      
      esm_module_t *mod = esm_find_module(resolved_path);
      if (!mod) {
        mod = esm_create_module(specifier, resolved_path);
        if (!mod) {
          free(resolved_path);
          return js_mkerr(js, "Cannot create module");
        }
      }
      
      const char *saved_code = js->code;
      jsoff_t saved_clen = js->clen;
      jsoff_t saved_pos = js->pos;
      uint8_t saved_tok = js->tok;
      uint8_t saved_consumed = js->consumed;
      jsoff_t saved_toff = js->toff;
      jsoff_t saved_tlen = js->tlen;
      jsval_t saved_scope = js->scope;
      
      ns = esm_load_module(js, mod);
      
      js->code = saved_code;
      js->clen = saved_clen;
      js->pos = saved_pos;
      js->tok = saved_tok;
      js->consumed = saved_consumed;
      js->toff = saved_toff;
      js->tlen = saved_tlen;
      js->scope = saved_scope;
      
      free(resolved_path);
    }
    
    js->consumed = 1;
    next(js);
    js->consumed = 0;
    
    if (is_err(ns)) return ns;
    
    for (int i = 0; i < binding_count; i++) {
      jsoff_t prop_off = lkp(js, ns, bindings[i].import_name, bindings[i].import_len);
      jsval_t imported_val = prop_off != 0 ? resolveprop(js, mkval(T_PROP, prop_off)) : js_mkundef();
      
      setprop(js, js->scope, js_mkstr(js, bindings[i].local_name, bindings[i].local_len), imported_val);
    }
    
    return js_mkundef();
  }
  
  return js_mkerr(js, "Invalid import statement");
}

static jsval_t js_export_stmt(struct js *js) {
  js->consumed = 1;
  
  jsval_t glob = js_glob(js);
  jsval_t module_ns = js_get(js, glob, "__esm_module_scope");
  
  if (vtype(module_ns) != T_OBJ) {
    module_ns = mkobj(js, 0);
    js_set(js, glob, "__esm_module_scope", module_ns);
  }
  
  if (next(js) == TOK_DEFAULT) {
    js->consumed = 1;
    jsval_t value = js_assignment(js);
    if (is_err(value)) return value;
    
    setprop(js, module_ns, js_mkstr(js, "default", 7), resolveprop(js, value));
    return value;
  }
  
  if (next(js) == TOK_CONST || next(js) == TOK_LET || next(js) == TOK_VAR) {
    bool is_const = (next(js) == TOK_CONST);
    js->consumed = 1;
    
    EXPECT(TOK_IDENTIFIER, );
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
    mkprop(js, js->scope, key, resolveprop(js, value), is_const);
    setprop(js, module_ns, key, resolveprop(js, value));
    
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
        setprop(js, module_ns, name_val, func);
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
        setprop(js, module_ns, name_val, cls);
      }
    }
    
    return cls;
  }
  
  if (next(js) == TOK_LBRACE) {
    js->consumed = 1;
    
    while (next(js) != TOK_RBRACE) {
      EXPECT(TOK_IDENTIFIER, );
      const char *local_name = &js->code[js->toff];
      size_t local_len = js->tlen;
      js->consumed = 1;
      
      const char *export_name = local_name;
      size_t export_len = local_len;
      
      if (next(js) == TOK_AS) {
        js->consumed = 1;
        EXPECT(TOK_IDENTIFIER, );
        export_name = &js->code[js->toff];
        export_len = js->tlen;
        js->consumed = 1;
      }
      
      jsval_t local_val = lookup(js, local_name, local_len);
      if (is_err(local_val)) return local_val;
      
      setprop(js, module_ns, js_mkstr(js, export_name, export_len), resolveprop(js, local_val));
      if (next(js) == TOK_COMMA) js->consumed = 1;
    }
    
    EXPECT(TOK_RBRACE, );
    if (next(js) == TOK_SEMICOLON) js->consumed = 1;
    return js_mkundef();
  }
  
  return js_mkerr(js, "Invalid export statement");
}

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
  if (vtype(map_proto) == T_OBJ) {
    set_proto(js, map_obj, map_proto);
  }
  
  map_entry_t **map_head = (map_entry_t **)ANT_GC_MALLOC(sizeof(map_entry_t *));
  if (!map_head) return js_mkerr(js, "out of memory");
  *map_head = NULL;
  
  jsval_t map_ptr = mkval(T_NUM, (size_t)map_head);
  jsval_t map_key = js_mkstr(js, "__map", 5);
  setprop(js, map_obj, map_key, map_ptr);
  
  return map_obj;
}

static jsval_t builtin_Set(struct js *js, jsval_t *args, int nargs) {
  jsval_t set_obj = mkobj(js, 0);
  
  jsval_t set_proto_val = get_ctor_proto(js, "Set", 3);
  if (vtype(set_proto_val) == T_OBJ) {
    set_proto(js, set_obj, set_proto_val);
  }
  
  set_entry_t **set_head = (set_entry_t **)ANT_GC_MALLOC(sizeof(set_entry_t *));
  if (!set_head) return js_mkerr(js, "out of memory");
  *set_head = NULL;
  
  jsval_t set_ptr = mkval(T_NUM, (size_t)set_head);
  jsval_t set_key = js_mkstr(js, "__set", 5);
  setprop(js, set_obj, set_key, set_ptr);
  
  return set_obj;
}

static map_entry_t** get_map_from_obj(struct js *js, jsval_t obj) {
  jsoff_t map_off = lkp(js, obj, "__map", 5);
  if (map_off == 0) return NULL;
  jsval_t map_val = resolveprop(js, mkval(T_PROP, map_off));
  if (vtype(map_val) != T_NUM) return NULL;
  return (map_entry_t**)(size_t)vdata(map_val);
}

static set_entry_t** get_set_from_obj(struct js *js, jsval_t obj) {
  jsoff_t set_off = lkp(js, obj, "__set", 5);
  if (set_off == 0) return NULL;
  jsval_t set_val = resolveprop(js, mkval(T_PROP, set_off));
  if (vtype(set_val) != T_NUM) return NULL;
  return (set_entry_t**)(size_t)vdata(set_val);
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
    entry = (map_entry_t *)ANT_GC_MALLOC(sizeof(map_entry_t));
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
    ANT_GC_FREE(entry);
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
    ANT_GC_FREE(entry);
  }
  *map_ptr = NULL;
  
  return js_mkundef();
}

static jsval_t map_size(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(js, this_val);
  if (!map_ptr) return tov(0);
  
  size_t count = HASH_COUNT(*map_ptr);
  return tov((double)count);
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
    entry = (set_entry_t *)ANT_GC_MALLOC(sizeof(set_entry_t));
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
    ANT_GC_FREE(entry);
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
    ANT_GC_FREE(entry);
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
  if (vtype(wm_proto) == T_OBJ) {
    set_proto(js, wm_obj, wm_proto);
  }
  
  weakmap_entry_t **wm_head = (weakmap_entry_t **)ANT_GC_MALLOC(sizeof(weakmap_entry_t *));
  if (!wm_head) return js_mkerr(js, "out of memory");
  *wm_head = NULL;
  
  jsval_t wm_ptr = mkval(T_NUM, (size_t)wm_head);
  jsval_t wm_key = js_mkstr(js, "__weakmap", 9);
  setprop(js, wm_obj, wm_key, wm_ptr);
  
  return wm_obj;
}

static jsval_t builtin_WeakSet(struct js *js, jsval_t *args, int nargs) {
  jsval_t ws_obj = mkobj(js, 0);
  
  jsval_t ws_proto = get_ctor_proto(js, "WeakSet", 7);
  if (vtype(ws_proto) == T_OBJ) {
    set_proto(js, ws_obj, ws_proto);
  }
  
  weakset_entry_t **ws_head = (weakset_entry_t **)ANT_GC_MALLOC(sizeof(weakset_entry_t *));
  if (!ws_head) return js_mkerr(js, "out of memory");
  *ws_head = NULL;
  
  jsval_t ws_ptr = mkval(T_NUM, (size_t)ws_head);
  jsval_t ws_key = js_mkstr(js, "__weakset", 9);
  setprop(js, ws_obj, ws_key, ws_ptr);
  
  return ws_obj;
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
    entry = (weakmap_entry_t *)ANT_GC_MALLOC(sizeof(weakmap_entry_t));
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
    ANT_GC_FREE(entry);
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
    entry = (weakset_entry_t *)ANT_GC_MALLOC(sizeof(weakset_entry_t));
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
    ANT_GC_FREE(entry);
    return mkval(T_BOOL, 1);
  }
  return mkval(T_BOOL, 0);
}

struct js *js_create(void *buf, size_t len) {
  ANT_GC_INIT();
  init_free_list();
  
  struct js *js = NULL;
  if (len < sizeof(*js) + esize(T_OBJ)) return js;
  memset(buf, 0, len);
  
  js = (struct js *) buf;
  js->mem = (uint8_t *) (js + 1);
  js->size = (jsoff_t) (len - sizeof(*js));
  js->scope = mkobj(js, 0);
  js->size = js->size / 8U * 8U;
  js->lwm = js->size;
  js->gct = js->size / 2;
  js->this_val = js->scope;
  
  jsval_t glob = js->scope;
  jsval_t object_proto = js_mkobj(js);
  setprop(js, object_proto, js_mkstr(js, "toString", 8), js_mkfun(builtin_object_toString));
  
  jsval_t function_proto = js_mkobj(js);
  set_proto(js, function_proto, object_proto);
  setprop(js, function_proto, js_mkstr(js, "call", 4), js_mkfun(builtin_function_call));
  setprop(js, function_proto, js_mkstr(js, "apply", 5), js_mkfun(builtin_function_apply));
  setprop(js, function_proto, js_mkstr(js, "bind", 4), js_mkfun(builtin_function_bind));
  
  jsval_t array_proto = js_mkobj(js);
  set_proto(js, array_proto, object_proto);
  setprop(js, array_proto, js_mkstr(js, "push", 4), js_mkfun(builtin_array_push));
  setprop(js, array_proto, js_mkstr(js, "pop", 3), js_mkfun(builtin_array_pop));
  setprop(js, array_proto, js_mkstr(js, "slice", 5), js_mkfun(builtin_array_slice));
  setprop(js, array_proto, js_mkstr(js, "join", 4), js_mkfun(builtin_array_join));
  setprop(js, array_proto, js_mkstr(js, "includes", 8), js_mkfun(builtin_array_includes));
  setprop(js, array_proto, js_mkstr(js, "every", 5), js_mkfun(builtin_array_every));
  
  jsval_t string_proto = js_mkobj(js);
  set_proto(js, string_proto, object_proto);
  setprop(js, string_proto, js_mkstr(js, "indexOf", 7), js_mkfun(builtin_string_indexOf));
  setprop(js, string_proto, js_mkstr(js, "substring", 9), js_mkfun(builtin_string_substring));
  setprop(js, string_proto, js_mkstr(js, "split", 5), js_mkfun(builtin_string_split));
  setprop(js, string_proto, js_mkstr(js, "slice", 5), js_mkfun(builtin_string_slice));
  setprop(js, string_proto, js_mkstr(js, "includes", 8), js_mkfun(builtin_string_includes));
  setprop(js, string_proto, js_mkstr(js, "startsWith", 10), js_mkfun(builtin_string_startsWith));
  setprop(js, string_proto, js_mkstr(js, "endsWith", 8), js_mkfun(builtin_string_endsWith));
  setprop(js, string_proto, js_mkstr(js, "replace", 7), js_mkfun(builtin_string_replace));
  setprop(js, string_proto, js_mkstr(js, "template", 8), js_mkfun(builtin_string_template));
  setprop(js, string_proto, js_mkstr(js, "charCodeAt", 10), js_mkfun(builtin_string_charCodeAt));
  setprop(js, string_proto, js_mkstr(js, "toLowerCase", 11), js_mkfun(builtin_string_toLowerCase));
  setprop(js, string_proto, js_mkstr(js, "toUpperCase", 11), js_mkfun(builtin_string_toUpperCase));
  setprop(js, string_proto, js_mkstr(js, "trim", 4), js_mkfun(builtin_string_trim));
  setprop(js, string_proto, js_mkstr(js, "repeat", 6), js_mkfun(builtin_string_repeat));
  setprop(js, string_proto, js_mkstr(js, "padStart", 8), js_mkfun(builtin_string_padStart));
  setprop(js, string_proto, js_mkstr(js, "padEnd", 6), js_mkfun(builtin_string_padEnd));
  setprop(js, string_proto, js_mkstr(js, "charAt", 6), js_mkfun(builtin_string_charAt));
  
  jsval_t number_proto = js_mkobj(js);
  set_proto(js, number_proto, object_proto);
  setprop(js, number_proto, js_mkstr(js, "toString", 8), js_mkfun(builtin_number_toString));
  setprop(js, number_proto, js_mkstr(js, "toFixed", 7), js_mkfun(builtin_number_toFixed));
  setprop(js, number_proto, js_mkstr(js, "toPrecision", 11), js_mkfun(builtin_number_toPrecision));
  setprop(js, number_proto, js_mkstr(js, "toExponential", 13), js_mkfun(builtin_number_toExponential));
  
  jsval_t boolean_proto = js_mkobj(js);
  set_proto(js, boolean_proto, object_proto);
  
  jsval_t error_proto = js_mkobj(js);
  set_proto(js, error_proto, object_proto);
  setprop(js, error_proto, js_mkstr(js, "name", 4), js_mkstr(js, "Error", 5));
  setprop(js, error_proto, js_mkstr(js, "message", 7), js_mkstr(js, "", 0));
  
  jsval_t date_proto = js_mkobj(js);
  set_proto(js, date_proto, object_proto);
  
  jsval_t regexp_proto = js_mkobj(js);
  set_proto(js, regexp_proto, object_proto);
  
  jsval_t map_proto = js_mkobj(js);
  set_proto(js, map_proto, object_proto);
  setprop(js, map_proto, js_mkstr(js, "set", 3), js_mkfun(map_set));
  setprop(js, map_proto, js_mkstr(js, "get", 3), js_mkfun(map_get));
  setprop(js, map_proto, js_mkstr(js, "has", 3), js_mkfun(map_has));
  setprop(js, map_proto, js_mkstr(js, "delete", 6), js_mkfun(map_delete));
  setprop(js, map_proto, js_mkstr(js, "clear", 5), js_mkfun(map_clear));
  setprop(js, map_proto, js_mkstr(js, "size", 4), js_mkfun(map_size));
  
  jsval_t set_proto_obj = js_mkobj(js);
  set_proto(js, set_proto_obj, object_proto);
  setprop(js, set_proto_obj, js_mkstr(js, "add", 3), js_mkfun(set_add));
  setprop(js, set_proto_obj, js_mkstr(js, "has", 3), js_mkfun(set_has));
  setprop(js, set_proto_obj, js_mkstr(js, "delete", 6), js_mkfun(set_delete));
  setprop(js, set_proto_obj, js_mkstr(js, "clear", 5), js_mkfun(set_clear));
  setprop(js, set_proto_obj, js_mkstr(js, "size", 4), js_mkfun(set_size));
  
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
  
  jsval_t promise_proto = js_mkobj(js);
  set_proto(js, promise_proto, object_proto);
  setprop(js, promise_proto, js_mkstr(js, "then", 4), js_mkfun(builtin_promise_then));
  setprop(js, promise_proto, js_mkstr(js, "catch", 5), js_mkfun(builtin_promise_catch));
  setprop(js, promise_proto, js_mkstr(js, "finally", 7), js_mkfun(builtin_promise_finally));
  
  jsval_t obj_func_obj = mkobj(js, 0);
  set_proto(js, obj_func_obj, function_proto);
  setprop(js, obj_func_obj, js_mkstr(js, "__code", 6), js_mkstr(js, "__builtin_Object", 16));
  setprop(js, obj_func_obj, js_mkstr(js, "keys", 4), js_mkfun(builtin_object_keys));
  setprop(js, obj_func_obj, js_mkstr(js, "values", 6), js_mkfun(builtin_object_values));
  setprop(js, obj_func_obj, js_mkstr(js, "entries", 7), js_mkfun(builtin_object_entries));
  setprop(js, obj_func_obj, js_mkstr(js, "getPrototypeOf", 14), js_mkfun(builtin_object_getPrototypeOf));
  setprop(js, obj_func_obj, js_mkstr(js, "setPrototypeOf", 14), js_mkfun(builtin_object_setPrototypeOf));
  setprop(js, obj_func_obj, js_mkstr(js, "create", 6), js_mkfun(builtin_object_create));
  setprop(js, obj_func_obj, js_mkstr(js, "hasOwn", 6), js_mkfun(builtin_object_hasOwn));
  setprop(js, obj_func_obj, js_mkstr(js, "defineProperty", 14), js_mkfun(builtin_object_defineProperty));
  setprop(js, obj_func_obj, js_mkstr(js, "prototype", 9), object_proto);
  setprop(js, glob, js_mkstr(js, "Object", 6), mkval(T_FUNC, vdata(obj_func_obj)));
  
  jsval_t func_ctor_obj = mkobj(js, 0);
  set_proto(js, func_ctor_obj, function_proto);
  setprop(js, func_ctor_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_Function));
  setprop(js, func_ctor_obj, js_mkstr(js, "prototype", 9), function_proto);
  setprop(js, glob, js_mkstr(js, "Function", 8), mkval(T_FUNC, vdata(func_ctor_obj)));
  
  jsval_t str_ctor_obj = mkobj(js, 0);
  set_proto(js, str_ctor_obj, function_proto);
  setprop(js, str_ctor_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_String));
  setprop(js, str_ctor_obj, js_mkstr(js, "prototype", 9), string_proto);
  setprop(js, glob, js_mkstr(js, "String", 6), mkval(T_FUNC, vdata(str_ctor_obj)));
  
  jsval_t number_ctor_obj = mkobj(js, 0);
  set_proto(js, number_ctor_obj, function_proto);
  setprop(js, number_ctor_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_Number));
  setprop(js, number_ctor_obj, js_mkstr(js, "isNaN", 5), js_mkfun(builtin_Number_isNaN));
  setprop(js, number_ctor_obj, js_mkstr(js, "isFinite", 8), js_mkfun(builtin_Number_isFinite));
  setprop(js, number_ctor_obj, js_mkstr(js, "prototype", 9), number_proto);
  setprop(js, glob, js_mkstr(js, "Number", 6), mkval(T_FUNC, vdata(number_ctor_obj)));
  
  jsval_t bool_ctor_obj = mkobj(js, 0);
  set_proto(js, bool_ctor_obj, function_proto);
  setprop(js, bool_ctor_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_Boolean));
  setprop(js, bool_ctor_obj, js_mkstr(js, "prototype", 9), boolean_proto);
  setprop(js, glob, js_mkstr(js, "Boolean", 7), mkval(T_FUNC, vdata(bool_ctor_obj)));
  
  jsval_t arr_ctor_obj = mkobj(js, 0);
  set_proto(js, arr_ctor_obj, function_proto);
  setprop(js, arr_ctor_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_Array));
  setprop(js, arr_ctor_obj, js_mkstr(js, "prototype", 9), array_proto);
  setprop(js, glob, js_mkstr(js, "Array", 5), mkval(T_FUNC, vdata(arr_ctor_obj)));
  
  jsval_t map_ctor_obj = mkobj(js, 0);
  set_proto(js, map_ctor_obj, function_proto);
  setprop(js, map_ctor_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_Map));
  setprop(js, map_ctor_obj, js_mkstr(js, "prototype", 9), map_proto);
  setprop(js, glob, js_mkstr(js, "Map", 3), mkval(T_FUNC, vdata(map_ctor_obj)));
  
  jsval_t set_ctor_obj = mkobj(js, 0);
  set_proto(js, set_ctor_obj, function_proto);
  setprop(js, set_ctor_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_Set));
  setprop(js, set_ctor_obj, js_mkstr(js, "prototype", 9), set_proto_obj);
  setprop(js, glob, js_mkstr(js, "Set", 3), mkval(T_FUNC, vdata(set_ctor_obj)));
  
  jsval_t weakmap_ctor_obj = mkobj(js, 0);
  set_proto(js, weakmap_ctor_obj, function_proto);
  setprop(js, weakmap_ctor_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_WeakMap));
  setprop(js, weakmap_ctor_obj, js_mkstr(js, "prototype", 9), weakmap_proto);
  setprop(js, glob, js_mkstr(js, "WeakMap", 7), mkval(T_FUNC, vdata(weakmap_ctor_obj)));
  
  jsval_t weakset_ctor_obj = mkobj(js, 0);
  set_proto(js, weakset_ctor_obj, function_proto);
  setprop(js, weakset_ctor_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_WeakSet));
  setprop(js, weakset_ctor_obj, js_mkstr(js, "prototype", 9), weakset_proto);
  setprop(js, glob, js_mkstr(js, "WeakSet", 7), mkval(T_FUNC, vdata(weakset_ctor_obj)));
  
  jsval_t err_ctor_obj = mkobj(js, 0);
  set_proto(js, err_ctor_obj, function_proto);
  setprop(js, err_ctor_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_Error));
  setprop(js, err_ctor_obj, js_mkstr(js, "prototype", 9), error_proto);
  setprop(js, glob, js_mkstr(js, "Error", 5), mkval(T_FUNC, vdata(err_ctor_obj)));
  
  jsval_t regex_ctor_obj = mkobj(js, 0);
  set_proto(js, regex_ctor_obj, function_proto);
  setprop(js, regex_ctor_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_RegExp));
  setprop(js, regex_ctor_obj, js_mkstr(js, "prototype", 9), regexp_proto);
  setprop(js, glob, js_mkstr(js, "RegExp", 6), mkval(T_FUNC, vdata(regex_ctor_obj)));
  
  jsval_t date_ctor_obj = mkobj(js, 0);
  set_proto(js, date_ctor_obj, function_proto);
  setprop(js, date_ctor_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_Date));
  setprop(js, date_ctor_obj, js_mkstr(js, "now", 3), js_mkfun(builtin_Date_now));
  setprop(js, date_ctor_obj, js_mkstr(js, "prototype", 9), date_proto);
  setprop(js, glob, js_mkstr(js, "Date", 4), mkval(T_FUNC, vdata(date_ctor_obj)));
  
  jsval_t p_ctor_obj = mkobj(js, 0);
  set_proto(js, p_ctor_obj, function_proto);
  setprop(js, p_ctor_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_Promise));
  setprop(js, p_ctor_obj, js_mkstr(js, "resolve", 7), js_mkfun(builtin_Promise_resolve));
  setprop(js, p_ctor_obj, js_mkstr(js, "reject", 6), js_mkfun(builtin_Promise_reject));
  setprop(js, p_ctor_obj, js_mkstr(js, "try", 3), js_mkfun(builtin_Promise_try));
  setprop(js, p_ctor_obj, js_mkstr(js, "all", 3), js_mkfun(builtin_Promise_all));
  setprop(js, p_ctor_obj, js_mkstr(js, "race", 4), js_mkfun(builtin_Promise_race));
  setprop(js, p_ctor_obj, js_mkstr(js, "prototype", 9), promise_proto);
  setprop(js, glob, js_mkstr(js, "Promise", 7), mkval(T_FUNC, vdata(p_ctor_obj)));
  
  jsval_t bigint_ctor_obj = mkobj(js, 0);
  set_proto(js, bigint_ctor_obj, function_proto);
  setprop(js, bigint_ctor_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_BigInt));
  setprop(js, bigint_ctor_obj, js_mkstr(js, "asIntN", 6), js_mkfun(builtin_BigInt_asIntN));
  setprop(js, bigint_ctor_obj, js_mkstr(js, "asUintN", 7), js_mkfun(builtin_BigInt_asUintN));
  setprop(js, glob, js_mkstr(js, "BigInt", 6), mkval(T_FUNC, vdata(bigint_ctor_obj)));
  
  setprop(js, glob, js_mkstr(js, "eval", 4), js_mkfun(builtin_eval));
  setprop(js, glob, js_mkstr(js, "parseInt", 8), js_mkfun(builtin_parseInt));
  setprop(js, glob, js_mkstr(js, "parseFloat", 10), js_mkfun(builtin_parseFloat));
  setprop(js, glob, js_mkstr(js, "btoa", 4), js_mkfun(builtin_btoa));
  setprop(js, glob, js_mkstr(js, "atob", 4), js_mkfun(builtin_atob));
  setprop(js, glob, js_mkstr(js, "NaN", 3), tov(NAN));
  setprop(js, glob, js_mkstr(js, "Infinity", 8), tov(INFINITY));
  
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
  
  setprop(js, import_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_import));
  setprop(js, glob, js_mkstr(js, "import", 6), mkval(T_FUNC, vdata(import_obj)));
  setprop(js, glob, js_mkstr(js, "__esm_module_scope", 18), js_mkundef());
  
  set_proto(js, glob, object_proto);
  
  js->owns_mem = false;
  js->max_size = 0;
  
  return js;
}

struct js *js_create_dynamic(size_t initial_size, size_t max_size) {
  if (initial_size < sizeof(struct js) + esize(T_OBJ)) initial_size = 1024 * 1024;
  if (max_size == 0 || max_size < initial_size) max_size = 512 * 1024 * 1024;
  
  void *buf = ANT_GC_MALLOC(initial_size);
  if (buf == NULL) return NULL;
  
  struct js *js = js_create(buf, initial_size);
  if (js == NULL) {
    ANT_GC_FREE(buf);
    return NULL;
  }
  
  js->owns_mem = true;
  js->max_size = (jsoff_t) max_size;
  
  return js;
}

void js_destroy(struct js *js) {
  if (js == NULL) return;
  esm_cleanup_module_cache();
  
  if (js->owns_mem) {
    ANT_GC_FREE((void *)((uint8_t *)js - 0));
  }
}

double js_getnum(jsval_t value) { return tod(value); }
int js_getbool(jsval_t value) { return vdata(value) & 1 ? 1 : 0; }

void js_setgct(struct js *js, size_t gct) { js->gct = (jsoff_t) gct; }
void js_setmaxcss(struct js *js, size_t max) { js->maxcss = (jsoff_t) max; }
void js_set_filename(struct js *js, const char *filename) { js->filename = filename; }

jsval_t js_mktrue(void) { return mkval(T_BOOL, 1); }
jsval_t js_mkfalse(void) { return mkval(T_BOOL, 0); }
jsval_t js_mkundef(void) { return mkval(T_UNDEF, 0); }
jsval_t js_mknull(void) { return mkval(T_NULL, 0); }
jsval_t js_mknum(double value) { return tov(value); }
jsval_t js_mkobj(struct js *js) { return mkobj(js, 0); }
jsval_t js_glob(struct js *js) { (void) js; return mkval(T_OBJ, 0); }
jsval_t js_mkfun(jsval_t (*fn)(struct js *, jsval_t *, int)) { return mkval(T_CFUNC, (size_t) (void *) fn); }
jsval_t js_getthis(struct js *js) { return js->this_val; }
jsval_t js_getcurrentfunc(struct js *js) { return js->current_func; }

void js_set(struct js *js, jsval_t obj, const char *key, jsval_t val) {
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
      mkprop(js, obj, key_str, val, false);
    }
  } else if (vtype(obj) == T_FUNC) {
    jsval_t func_obj = mkval(T_OBJ, vdata(obj));
    jsoff_t existing = lkp(js, func_obj, key, key_len);
    if (existing > 0) {
      if (is_const_prop(js, existing)) {
        js_mkerr(js, "assignment to constant");
        return;
      }
      saveval(js, existing + sizeof(jsoff_t) * 2, val);
    } else {
      jsval_t key_str = js_mkstr(js, key, key_len);
      mkprop(js, func_obj, key_str, val, false);
    }
  }
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
  
  if (vtype(obj) != T_OBJ) return js_mkundef();
  jsoff_t off = lkp(js, obj, key, key_len);
  
  if (off == 0) {
    jsval_t result = try_dynamic_getter(js, obj, key, key_len);
    if (vtype(result) != T_UNDEF) return result;
  }
  
  return off == 0 ? js_mkundef() : resolveprop(js, mkval(T_PROP, off));
}

char *js_getstr(struct js *js, jsval_t value, size_t *len) {
  if (vtype(value) != T_STR) return NULL;
  jsoff_t n, off = vstr(js, value, &n);
  if (len != NULL) *len = n;
  return (char *) &js->mem[off];
}

void js_merge_obj(struct js *js, jsval_t dst, jsval_t src) {
  if (vtype(dst) != T_OBJ || vtype(src) != T_OBJ) return;
  jsoff_t next = loadoff(js, (jsoff_t) vdata(src)) & ~(3U | CONSTMASK);
  while (next < js->brk && next != 0) {
    jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
    jsval_t val = loadval(js, next + (jsoff_t) (sizeof(next) + sizeof(koff)));
    jsoff_t klen = offtolen(loadoff(js, koff));
    
    char *key = (char *) &js->mem[koff + sizeof(koff)];
    setprop(js, dst, js_mkstr(js, key, klen), val);
    next = loadoff(js, next) & ~(3U | CONSTMASK);
  }
}

int js_type(jsval_t val) {
  switch (vtype(val)) {
    case T_UNDEF:   return JS_UNDEF;
    case T_NULL:    return JS_NULL;
    case T_BOOL:    return vdata(val) == 0 ? JS_FALSE: JS_TRUE;
    case T_STR:     return JS_STR;
    case T_NUM:     return JS_NUM;
    case T_ERR:     return JS_ERR;
    case T_PROMISE: return JS_PROMISE;
    default:        return JS_PRIV;
  }
}

void js_stats(struct js *js, size_t *total, size_t *lwm, size_t *css) {
  if (total) *total = js->size;
  if (lwm) *lwm = js->lwm;
  if (css) *css = js->css;
}

size_t js_getbrk(struct js *js) { return (size_t) js->brk; }

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

jsval_t js_eval(struct js *js, const char *buf, size_t len) {
  jsval_t res = js_mkundef();
  if (len == (size_t) ~0U) len = strlen(buf);
  js->consumed = 1;
  js->tok = TOK_ERR;
  js->code = buf;
  js->clen = (jsoff_t) len;
  js->pos = 0;
  js->cstk = &res;
  
  uint8_t saved_tok = js->tok;
  jsoff_t saved_pos = js->pos;
  uint8_t saved_consumed = js->consumed;
  js->consumed = 1;
  
  if (next(js) == TOK_STRING) {
    const char *str = &js->code[js->toff + 1];
    size_t str_len = js->tlen - 2;
    if (str_len == 10 && memcmp(str, "use strict", 10) == 0) {
      js->flags |= F_STRICT;
    }
  }
  
  js->tok = saved_tok;
  js->pos = saved_pos;
  js->consumed = saved_consumed;
  
  while (next(js) != TOK_EOF && !is_err(res)) {
    res = js_stmt(js);
    if (js->flags & F_RETURN) break;
  }
  return res;
}

jsval_t js_call(struct js *js, jsval_t func, jsval_t *args, int nargs) {
  if (vtype(func) == T_CFUNC) {
    jsval_t (*fn)(struct js *, jsval_t *, int) = (jsval_t(*)(struct js *, jsval_t *, int)) vdata(func);
    return fn(js, args, nargs);
  } else if (vtype(func) == T_FUNC) {
    jsval_t func_obj = mkval(T_OBJ, vdata(func));
    jsoff_t native_off = lkp(js, func_obj, "__native_func", 13);
    if (native_off != 0) {
      jsval_t native_val = resolveprop(js, mkval(T_PROP, native_off));
      if (vtype(native_val) == T_CFUNC) {
        jsval_t saved_func = js->current_func;
        js->current_func = func;
        jsval_t (*fn)(struct js *, jsval_t *, int) = (jsval_t(*)(struct js *, jsval_t *, int)) vdata(native_val);
        jsval_t res = fn(js, args, nargs);
        js->current_func = saved_func;
        return res;
      }
    }
    jsoff_t code_off = lkp(js, func_obj, "__code", 6);
    
    if (code_off == 0) return js_mkerr(js, "function has no code");
    jsval_t code_val = resolveprop(js, mkval(T_PROP, code_off));
    if (vtype(code_val) != T_STR) return js_mkerr(js, "function code not string");
    jsoff_t fnlen, fnoff = vstr(js, code_val, &fnlen);
    const char *fn = (const char *) (&js->mem[fnoff]);
    
    jsoff_t async_off = lkp(js, func_obj, "__async", 7);
    bool is_async = false;
    if (async_off != 0) {
      jsval_t async_val = resolveprop(js, mkval(T_PROP, async_off));
      is_async = vtype(async_val) == T_BOOL && vdata(async_val) == 1;
    }
    
    if (is_async) {
      jsval_t closure_scope = js_mkundef();
      jsoff_t scope_off = lkp(js, func_obj, "__scope", 7);
      if (scope_off != 0) {
        closure_scope = resolveprop(js, mkval(T_PROP, scope_off));
      }
      return start_async_in_coroutine(js, fn, fnlen, closure_scope, args, nargs);
    }
    
    jsoff_t fnpos = 1;
    
    jsval_t saved_scope = js->scope;
    jsoff_t scope_off = lkp(js, func_obj, "__scope", 7);
    if (scope_off != 0) {
      jsval_t closure_scope = resolveprop(js, mkval(T_PROP, scope_off));
      if (vtype(closure_scope) == T_OBJ) {
        js->scope = closure_scope;
      }
    }
    
    uint8_t saved_flags = js->flags;
    js->flags = 0;
    mkscope(js);
    js->flags = saved_flags;
    int arg_idx = 0;
    
    bool has_rest = false;
    jsoff_t rest_param_start = 0, rest_param_len = 0;
    
    while (fnpos < fnlen) {
      fnpos = skiptonext(fn, fnlen, fnpos);
      if (fnpos < fnlen && fn[fnpos] == ')') break;
      
      bool is_rest = false;
      if (fnpos + 3 < fnlen && fn[fnpos] == '.' && fn[fnpos + 1] == '.' && fn[fnpos + 2] == '.') {
        is_rest = true;
        has_rest = true;
        fnpos += 3;
        fnpos = skiptonext(fn, fnlen, fnpos);
      }
      
      jsoff_t identlen = 0;
      uint8_t tok = parseident(&fn[fnpos], fnlen - fnpos, &identlen);
      if (tok != TOK_IDENTIFIER) break;
      
      if (is_rest) {
        rest_param_start = fnpos;
        rest_param_len = identlen;
        fnpos = skiptonext(fn, fnlen, fnpos + identlen);
        break;
      }
      
      jsval_t v = arg_idx < nargs ? args[arg_idx] : js_mkundef();
      setprop(js, js->scope, js_mkstr(js, &fn[fnpos], identlen), v);
      arg_idx++;
      fnpos = skiptonext(fn, fnlen, fnpos + identlen);
      if (fnpos < fnlen && fn[fnpos] == ',') fnpos++;
    }
    
    if (has_rest && rest_param_len > 0) {
      jsval_t rest_array = mkarr(js);
      if (!is_err(rest_array)) {
        jsoff_t idx = 0;
        while (arg_idx < nargs) {
          char idxstr[16];
          snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) idx);
          jsval_t key = js_mkstr(js, idxstr, strlen(idxstr));
          setprop(js, rest_array, key, args[arg_idx]);
          idx++;
          arg_idx++;
        }
        jsval_t len_key = js_mkstr(js, "length", 6);
        setprop(js, rest_array, len_key, tov((double) idx));
        rest_array = mkval(T_ARR, vdata(rest_array));
        setprop(js, js->scope, js_mkstr(js, &fn[rest_param_start], rest_param_len), rest_array);
      }
    }
    
    if (fnpos < fnlen && fn[fnpos] == ')') fnpos++;
    fnpos = skiptonext(fn, fnlen, fnpos);
    
    if (fnpos < fnlen && fn[fnpos] == '{') fnpos++;
    size_t body_len = fnlen - fnpos - 1;
    
    jsval_t saved_this = js->this_val;
    js->this_val = js_glob(js);
    
    js->flags = F_CALL;
    jsval_t res = js_eval(js, &fn[fnpos], body_len);
    if (!is_err(res) && !(js->flags & F_RETURN)) res = js_mkundef();
    
    js->this_val = saved_this;
    delscope(js);
    js->scope = saved_scope;
    
    return res;
  }
  return js_mkerr(js, "not a function");
}

js_prop_iter_t js_prop_iter_begin(struct js *js, jsval_t obj) {
  js_prop_iter_t iter = {0};
  iter.obj = obj;
  iter.js_internal = (void *)js;
  
  if (vtype(obj) == T_OBJ || vtype(obj) == T_ARR || vtype(obj) == T_FUNC) {
    jsval_t check_obj = (vtype(obj) == T_FUNC) ? mkval(T_OBJ, vdata(obj)) : obj;
    jsoff_t next = loadoff(js, (jsoff_t) vdata(check_obj)) & ~(3U | CONSTMASK);
    iter.current = (void *)(uintptr_t)next;
  }
  
  return iter;
}

bool js_prop_iter_next(js_prop_iter_t *iter, const char **key, size_t *key_len, jsval_t *value) {
  if (!iter || !iter->js_internal) return false;
  
  struct js *js = (struct js *)iter->js_internal;
  jsoff_t next = (jsoff_t)(uintptr_t)iter->current;
  
  if (next >= js->brk || next == 0) return false;
  
  jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
  jsval_t val = loadval(js, next + (jsoff_t) (sizeof(next) + sizeof(koff)));
  
  if (key) {
    jsoff_t klen = offtolen(loadoff(js, koff));
    *key = (const char *) &js->mem[koff + sizeof(koff)];
    if (key_len) *key_len = klen;
  }
  
  if (value) *value = val;
  
  iter->current = (void *)(uintptr_t)(loadoff(js, next) & ~(3U | CONSTMASK));
  return true;
}

void js_prop_iter_end(js_prop_iter_t *iter) {
  if (iter) {
    iter->current = NULL;
    iter->js_internal = NULL;
  }
}

jsval_t js_mkpromise(struct js *js) { return mkpromise(js); }
void js_resolve_promise(struct js *js, jsval_t promise, jsval_t value) { resolve_promise(js, promise, value); }
void js_reject_promise(struct js *js, jsval_t promise, jsval_t value) { reject_promise(js, promise, value); }

#ifdef JS_DUMP
void js_dump(struct js *js) {
  jsoff_t off = 0, v;
  printf("JS size %u, brk %u, lwm %u, css %u, nogc %u\n", js->size, js->brk, js->lwm, (unsigned) js->css, js->nogc);
  while (off < js->brk) {
    memcpy(&v, &js->mem[off], sizeof(v));
    printf(" %5u: ", off);
    jsoff_t cleaned = v & ~(GCMASK | CONSTMASK);
    if ((cleaned & 3U) == T_OBJ) {
      printf("OBJ %u %u%s\n", cleaned & ~3U, loadoff(js, (jsoff_t) (off + sizeof(off))), (v & CONSTMASK) ? " [CONST]" : "");
    } else if ((cleaned & 3U) == T_PROP) {
      jsoff_t koff = loadoff(js, (jsoff_t) (off + sizeof(v)));
      jsval_t val = loadval(js, (jsoff_t) (off + sizeof(v) + sizeof(v)));
      printf("PROP next %u, koff %u vtype %d vdata %lu%s\n", cleaned & ~3U, koff, vtype(val), (unsigned long) vdata(val), (v & CONSTMASK) ? " [CONST]" : "");
    } else if ((cleaned & 3) == T_STR) {
      jsoff_t len = offtolen(cleaned);
      printf("STR %u [%.*s]\n", len, (int) len, js->mem + off + sizeof(v));
    } else {
      printf("???\n");
      break;
    }
    off += esize(v & ~(GCMASK | CONSTMASK));
  }
}
#endif

void js_set_getter(struct js *js, jsval_t obj, js_getter_fn getter) {
  if (vtype(obj) != T_OBJ) return;
  jsval_t getter_val = mkval(T_CFUNC, (size_t)(void *)getter);
  js_set(js, obj, "__getter", getter_val);
}

void js_print_stack_trace(FILE *stream) {
  if (global_call_stack.depth > 0) {
    for (int i = global_call_stack.depth - 1; i >= 0; i--) {
      call_frame_t *frame = &global_call_stack.frames[i];
      fprintf(stream, "  at ");
      
      if (frame->function_name) {
        fprintf(stream, "%s", frame->function_name);
      } else fprintf(stream, "<anonymous>");
      
      fprintf(stream, " (\x1b[90m");
      
      if (frame->filename) {
        fprintf(stream, "%s:%d:%d", frame->filename, frame->line, frame->col);
      } else fprintf(stream, "<unknown>");
      
      fprintf(stream, "\x1b[0m)\n");
    }
  }
}
