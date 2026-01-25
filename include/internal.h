#ifndef ANT_INTERNAL_H
#define ANT_INTERNAL_H

#include "ant.h"

struct for_let_ctx {
  const char *var_name;   // interned variable name
  jsoff_t var_len;        // length of var name
  jsoff_t prop_off;       // offset of var property in loop scope
  jsval_t body_scope;     // loop body scope for capturing block-scoped vars
};

struct js {
  jsoff_t css;            // max observed C stack size
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
  jsval_t this_val;       // 'this' value for currently executing function
  jsval_t super_val;      // 'super' value for class methods
  jsval_t new_target;     // constructor called with 'new', undefined otherwise
  jsval_t module_ns;      // current ESM module namespace
  uint8_t *mem;           // available JS memory
  jsoff_t size;           // memory size
  jsoff_t brk;            // current mem usage boundary
  jsoff_t maxcss;         // maximum allowed C stack size usage
  void *cstk;             // C stack pointer at the beginning of js_eval()
  jsval_t current_func;   // currently executing function (for native closures)
  bool var_warning_shown; // flag to show var deprecation warning only once
  bool owns_mem;          // true if js owns the memory buffer (dynamic allocation)
  jsoff_t max_size;       // maximum allowed memory size (for dynamic growth)
  bool had_newline;       // true if newline was crossed before current token
  jsval_t thrown_value;   // stores the actual thrown value for catch blocks
  bool is_hoisting;       // true during function declaration hoisting pass
  uint64_t sym_counter;   // counter for generating unique symbol IDs
  bool needs_gc;          // deferred GC flag, checked at statement boundaries
  bool gc_suppress;       // suppress GC during microtask batch processing
  int eval_depth;         // recursion depth of js_eval calls
  int parse_depth;        // recursion depth of parser (for stack overflow protection)
  bool skip_func_hoist;   // skip function declaration hoisting (pre-computed)
  
  // for-let loop context stack
  struct for_let_ctx *for_let_stack;
  int for_let_stack_len;
  int for_let_stack_cap;
};

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
#define T_NEEDS_PROTO_FALLBACK (TYPE_FLAG(T_FUNC) | TYPE_FLAG(T_ARR) | TYPE_FLAG(T_PROMISE))
#define T_NON_NUMERIC_MASK (TYPE_FLAG(T_STR) | TYPE_FLAG(T_ARR) | TYPE_FLAG(T_FUNC) | TYPE_FLAG(T_CFUNC) | TYPE_FLAG(T_OBJ))

jsoff_t esize(jsoff_t w);

void js_gc_update_roots(GC_UPDATE_ARGS);
bool js_has_pending_coroutines(void);
bool is_internal_prop(const char *key, jsoff_t klen);

#define is_non_numeric(v) ((1u << vtype(v)) & T_NON_NUMERIC_MASK)

#endif
