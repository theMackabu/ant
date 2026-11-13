#pragma once

#include "ant/base.h"

typedef struct ant_isolate_options {
  size_t object_arena_bytes;
} ant_isolate_options_t;

// create a new ant isolate
ant_t *Ant_Create(const ant_isolate_options_t *options);

// destroy current ant isolate
void Ant_Destroy(ant_t *);

// clean up after a thread
void ant_thread_cleanup(void);

void init_intrinsic_symbols(ant_t *js);
void js_init_function_has_instance(ant_t *js);
bool js_realm_init_intrinsics(ant_t *js);
bool js_realm_finish_intrinsics(ant_t *js);

typedef enum: uint8_t {
  JS_EVAL_COMPLETE,
  JS_EVAL_ASYNC_ENTRY,
} js_eval_completion_kind_t;

typedef struct {
  ant_value_t value;
  coroutine_t *async_coro;
  js_eval_completion_kind_t kind;
} js_eval_result_t;

ant_value_t js_eval_bytecode(ant_t *, const char *, size_t);
ant_value_t js_eval_bytecode_module(ant_t *, const char *, size_t);
ant_value_t js_eval_bytecode_eval(ant_t *, const char *, size_t, bool strict);
js_eval_result_t js_eval_bytecode_repl(ant_t *, const char *, size_t);
bool Ant_Eval_WithoutSideEffects(ant_t *js, const char *expr, size_t len, ant_value_t *out);

bool js_chkargs(ant_value_t *, int, const char *);
void js_stats(ant_t *, size_t *total, size_t *min, size_t *cstacksize);

void js_check_unhandled_rejections(ant_t *js);
void js_setup_import_meta(ant_t *js, const char *filename);
