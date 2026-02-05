#ifndef ANT_INTERNAL_H
#define ANT_INTERNAL_H

#include "ant.h"
#include "gc.h"
#include <utarray.h>

extern const UT_icd jsoff_icd;
extern const UT_icd jsval_icd;

extern UT_array *global_scope_stack;
extern UT_array *saved_scope_stack;

struct for_let_ctx {
  const char *var_name;   // interned variable name
  jsoff_t var_len;        // length of var name
  jsoff_t prop_off;       // offset of var property in loop scope
  jsval_t body_scope;     // loop body scope for capturing block-scoped vars
};

struct js {
  const char *code;       // currently parsed code snippet
  char *errmsg;           // dynamic error message buffer
  size_t errmsg_size;     // size of error message buffer
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
  jsval_t tval;           // holds last parsed numeric or string literal value
  jsval_t scope;          // current scope
  jsval_t global;         // global root object
  jsval_t object;         // global object prototype
  jsval_t this_val;       // 'this' value for currently executing function
  jsval_t super_val;      // 'super' value for class methods
  jsval_t new_target;     // constructor called with 'new', undefined otherwise
  jsval_t module_ns;      // current ESM module namespace
  uint8_t *mem;           // available JS memory
  jsoff_t size;           // memory size
  jsoff_t brk;            // current mem usage boundary
  void *cstk;             // C stack pointer at the beginning of js_eval()
  size_t stack_limit;     // max stack bytes allowed (0 = no limit)
  jsval_t current_func;   // currently executing function (for native closures)
  bool var_warning_shown; // flag to show var deprecation warning only once
  bool owns_mem;          // true if js owns the memory buffer (dynamic allocation)
  jsoff_t max_size;       // maximum allowed memory size (for dynamic growth)
  bool had_newline;       // true if newline was crossed before current token
  jsval_t thrown_value;   // stores the actual thrown value for catch blocks
  bool is_hoisting;       // true during function declaration hoisting pass
  uint64_t sym_counter;   // counter for generating unique symbol IDs
  bool needs_gc;          // deferred GC flag, checked at statement boundaries
  bool gc_safe;           // true when GC is allowed to run (at safe points only)
  jsoff_t gc_alloc_since; // bytes allocated since last GC
  int eval_depth;         // recursion depth of js_eval calls
  int parse_depth;        // recursion depth of parser (for stack overflow protection)
  bool skip_func_hoist;   // skip function declaration hoisting (pre-computed)
  bool fatal_error;       // fatal error that should bypass promise rejection handling
  
  struct for_let_ctx *for_let_stack;
  int for_let_stack_len;
  int for_let_stack_cap;
  
  jsval_t *gc_roots;
  jshdl_t gc_roots_len;
  jshdl_t gc_roots_cap;
  
  jsval_t ascii_char_cache[128];
  bool ascii_cache_init;
  
  void *token_stream;
  int token_stream_pos;
  const char *token_stream_code;
};

typedef struct {
  const char *ptr;
  size_t len;
  bool needs_free;
} js_cstr_t;

enum {
  T_OBJ, T_PROP, T_STR, T_UNDEF, T_NULL, T_NUM, T_BOOL, T_FUNC,
  T_CODEREF, T_CFUNC, T_ERR, T_ARR, T_PROMISE, T_TYPEDARRAY, 
  T_BIGINT, T_PROPREF, T_SYMBOL, T_GENERATOR, T_FFI
};

#define JS_HASH_SIZE       512
#define JS_MAX_PARSE_DEPTH (1024 * 2)
#define JS_ERR_NO_STACK    (1 << 8)

#define NANBOX_PREFIX     0x7FC0000000000000ULL
#define NANBOX_PREFIX_CHK 0x3FEULL
#define NANBOX_TYPE_SHIFT 48
#define NANBOX_TYPE_MASK  0x1F
#define NANBOX_DATA_MASK  0x0000FFFFFFFFFFFFULL

#define TYPE_FLAG(t) (1u << (t))

#define T_SPECIAL_OBJECT_MASK  (TYPE_FLAG(T_OBJ) | TYPE_FLAG(T_ARR))
#define T_NEEDS_PROTO_FALLBACK (TYPE_FLAG(T_FUNC) | TYPE_FLAG(T_ARR) | TYPE_FLAG(T_PROMISE))
#define T_OBJECT_MASK          (TYPE_FLAG(T_OBJ) | TYPE_FLAG(T_ARR) | TYPE_FLAG(T_FUNC) | TYPE_FLAG(T_PROMISE))
#define T_NON_NUMERIC_MASK     (TYPE_FLAG(T_STR) | TYPE_FLAG(T_ARR) | TYPE_FLAG(T_FUNC) | TYPE_FLAG(T_CFUNC) | TYPE_FLAG(T_OBJ))

bool is_internal_prop(const char *key, jsoff_t klen);
size_t uint_to_str(char *buf, size_t bufsize, uint64_t val);

void js_gc_reserve_roots(GC_RESERVE_ARGS);
void js_gc_update_roots(GC_UPDATE_ARGS);

jsoff_t esize(jsoff_t w);
jsval_t tov(double d);
double tod(jsval_t v);

jsval_t resolveprop(struct js *js, jsval_t v);
jsval_t setprop_cstr(struct js *js, jsval_t obj, const char *key, size_t len, jsval_t v);
jsval_t setprop_interned(struct js *js, jsval_t obj, const char *key, size_t len, jsval_t v);

jsval_t coerce_to_str(struct js *js, jsval_t v);
jsval_t coerce_to_str_concat(struct js *js, jsval_t v);
js_cstr_t js_to_cstr(struct js *js, jsval_t value, char *stack_buf, size_t stack_size);

jsoff_t lkp(struct js *js, jsval_t obj, const char *buf, size_t len);
jsoff_t lkp_proto(struct js *js, jsval_t obj, const char *buf, size_t len);
jsoff_t vstr(struct js *js, jsval_t value, jsoff_t *len);

jsval_t mkarr(struct js *js);
jsval_t mkval(uint8_t type, uint64_t data);

jsval_t call_js(ant_t *js, const char *fn, jsoff_t fnlen, jsval_t closure_scope);
jsval_t call_js_internal(ant_t *js, const char *fn, jsoff_t fnlen, jsval_t closure_scope, jsval_t *bound_args, int bound_argc, jsval_t func_val);

jsval_t call_js_with_args(ant_t *js, jsval_t fn, jsval_t *args, int nargs);
jsval_t call_js_code_with_args(ant_t *js, const char *fn, jsoff_t fnlen, jsval_t closure_scope, jsval_t *args, int nargs, jsval_t func_val);

#define is_non_numeric(v)    ((1u << vtype(v)) & T_NON_NUMERIC_MASK)
#define is_object_type(v)    ((1u << vtype(v)) & T_OBJECT_MASK)
#define is_special_object(v) ((1u << vtype(v)) & T_SPECIAL_OBJECT_MASK)

static inline bool is_err(jsval_t v) { 
  return vtype(v) == T_ERR; 
}

static inline bool is_null(jsval_t v) { 
  return vtype(v) == T_NULL; 
}

static inline bool is_undefined(jsval_t v) { 
  return vtype(v) == T_UNDEF; 
}

#endif
