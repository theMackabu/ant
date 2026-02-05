#ifndef ROOTS_H
#define ROOTS_H

#include "types.h"

jshdl_t js_root(struct js *js, jsval_t val);
jsval_t js_deref(struct js *js, jshdl_t h);

void js_unroot(struct js *js, jshdl_t h);
void js_root_update(struct js *js, jshdl_t h, jsval_t val);

typedef struct {
  struct js *js;
  jshdl_t h;
} rooted_handle_t;

static inline void _rooted_cleanup(rooted_handle_t *r) {
  if (r->h) js_unroot(r->js, r->h);
}

#define ROOTED(name, val) \
  jsval_t name = (val); \
  rooted_handle_t name##_r __attribute__((cleanup(_rooted_cleanup))) = {js, js_root(js, name)}

#define ROOT_UPDATE(name, val) do { \
  name = (val); \
  js_root_update(js, name##_r.h, name); \
} while(0)

#define ROOT_SYNC(name) (name = js_deref(js, name##_r.h))

#endif