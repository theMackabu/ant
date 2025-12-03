#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct js;
typedef uint64_t jsval_t;

enum { 
  JS_UNDEF, JS_NULL, JS_TRUE, JS_FALSE, 
  JS_STR, JS_NUM, JS_ERR, JS_PRIV 
};

struct js *js_create(void *buf, size_t len);
struct js *js_create_dynamic(size_t initial_size, size_t max_size);

void js_gc(struct js *);
void js_destroy(struct js *);

jsval_t js_glob(struct js *);
jsval_t js_eval(struct js *, const char *, size_t);

void js_dump(struct js *);
void js_mkscope(struct js *);
void js_delscope(struct js *);
void js_setgct(struct js *, size_t);
bool js_truthy(struct js *, jsval_t);
void js_setmaxcss(struct js *, size_t);

bool js_chkargs(jsval_t *, int, const char *);
void js_set_filename(struct js *, const char *);
void js_stats(struct js *, size_t *total, size_t *min, size_t *cstacksize);
size_t js_getbrk(struct js *);

jsval_t js_mkundef(void);
jsval_t js_mknull(void);
jsval_t js_mktrue(void);
jsval_t js_mkfalse(void);
jsval_t js_mknum(double);

jsval_t js_getthis(struct js *);
jsval_t js_getcurrentfunc(struct js *);
jsval_t js_get(struct js *, jsval_t, const char *);

jsval_t js_mkobj(struct js *);
jsval_t js_mkstr(struct js *, const void *, size_t);
jsval_t js_mkerr(struct js *js, const char *fmt, ...);
jsval_t js_mkfun(jsval_t (*fn)(struct js *, jsval_t *, int));
jsval_t js_call(struct js *js, jsval_t func, jsval_t *args, int nargs);

void js_set(struct js *, jsval_t, const char *, jsval_t);
void js_merge_obj(struct js *, jsval_t dst, jsval_t src);

int js_type(jsval_t val);
int js_getbool(jsval_t val);

double js_getnum(jsval_t val);
char *js_getstr(struct js *js, jsval_t val, size_t *len);

const char *js_str(struct js *, jsval_t val);

typedef struct {
  jsval_t obj;
  void *current;
  void *js_internal;
} js_prop_iter_t;

js_prop_iter_t js_prop_iter_begin(struct js *js, jsval_t obj);
bool js_prop_iter_next(js_prop_iter_t *iter, const char **key, size_t *key_len, jsval_t *value);
void js_prop_iter_end(js_prop_iter_t *iter);

jsval_t js_mkpromise(struct js *js);
void js_resolve_promise(struct js *js, jsval_t promise, jsval_t value);
void js_reject_promise(struct js *js, jsval_t promise, jsval_t value);