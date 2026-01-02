#ifndef ANT_INTERNAL_H
#define ANT_INTERNAL_H

#include "ant.h"

struct js {
  jsoff_t css;            // max observed C stack size
  jsoff_t lwm;            // JS ram low watermark: min free ram observed
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
  jsoff_t nogc;           // entity offset to exclude from GC
  jsval_t tval;           // holds last parsed numeric or string literal value
  jsval_t scope;          // current scope
  jsval_t this_val;       // 'this' value for currently executing function
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
};

#define JS_T_OBJ   0
#define JS_T_PROP  1
#define JS_T_STR   2

#define JS_V_OBJ       0
#define JS_V_PROP      1
#define JS_V_STR       2
#define JS_V_FUNC      7
#define JS_V_ARR       11
#define JS_V_PROMISE   12
#define JS_V_BIGINT    14
#define JS_V_GENERATOR 17
#define JS_HASH_SIZE   512

#define NANBOX_PREFIX     0x7FC0000000000000ULL
#define NANBOX_PREFIX_CHK 0x3FEULL
#define NANBOX_TYPE_SHIFT 48
#define NANBOX_TYPE_MASK  0x1F
#define NANBOX_DATA_MASK  0x0000FFFFFFFFFFFFULL

void js_gc_update_roots(GC_UPDATE_ARGS);
bool js_has_pending_coroutines(void);

#endif
