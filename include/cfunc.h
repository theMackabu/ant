#ifndef ANT_CFUNC_H
#define ANT_CFUNC_H

#include "types.h"
#include "ptr.h"
#include "realm.h"
#include "handles.h"

typedef struct {
  ant_cfunc_meta_t meta;
  const ant_cfunc_meta_t *stored;
} ant_cfunc_site_t;

void js_define_methods(
  ant_t *js, ant_value_t obj, 
  const ant_cfunc_meta_t *methods, 
  size_t count, uint8_t attrs
);

ant_value_t js_mkfun_dyn(ant_cfunc_t fn);
ant_value_t js_mkfun_meta(const ant_cfunc_meta_t *meta);
ant_value_t js_mkfun_site(ant_cfunc_site_t *site);

ant_value_t js_heavy_mkfun(ant_t *js, ant_value_t (*fn)(ant_params_t), ant_value_t data);
ant_value_t js_heavy_mkfun_cfunc(ant_t *js, ant_value_t cfunc, ant_value_t data);
ant_value_t js_heavy_mkfun_native(ant_t *js, ant_value_t (*fn)(ant_params_t), void *ptr, uint32_t tag);
ant_value_t js_make_ctor_cfunc(ant_t *js, ant_value_t cfunc, ant_value_t proto, uint8_t proto_attrs);

#define Ant_Function_CreateWithMeta(fn, name, arity, flags) ({ \
  static ant_cfunc_site_t _ant_cfunc_site = {                  \
    { (fn), (name), (arity), (flags) }, NULL                   \
  };                                                           \
  js_mkfun_site(&_ant_cfunc_site);                             \
})

#define Ant_Function_CreateEx(fn, arity, flags) \
  Ant_Function_CreateWithMeta(fn, NULL, arity, flags)
  
#define Ant_Function_Create(fn) \
    Ant_Function_CreateEx(fn, 0, 0)
  
#define Ant_Function_CreateWithArity(fn, arity) \
  Ant_Function_CreateEx(fn, arity, 0)

#define Ant_Function_CreateWithFlags(fn, flags) \
  Ant_Function_CreateEx(fn, 0, flags)

#define Ant_Function_CreateWithData(js, fn, name, arity, data) \
  js_heavy_mkfun_cfunc((js), Ant_Function_CreateWithMeta(fn, name, arity, 0), (data))

#define Ant_Function_CreateConstructorEx(js, fn, proto, name, length, attrs) \
  js_make_ctor_cfunc((js), Ant_Function_CreateWithMeta(fn, name, length, 0), (proto), (attrs))

#define Ant_Function_CreateConstructor(js, fn, proto, name, length) \
  Ant_Function_CreateConstructorEx(js, fn, proto, name, length, ANT_PROP_ATTR_WRITABLE)

#define Ant_Function_CreateIntrinsicConstructor(js, fn, proto, name, length) \
  Ant_Function_CreateConstructorEx(js, fn, proto, name, length, 0)

static inline ant_ptr_pair_map_t *js_cfunc_promoted_map(ant_t *js, const ant_cfunc_meta_t *meta) {
  ant_realm_t *realm = ant_cfunc_meta_realm(meta);
  return realm ? &realm->cfunc_promoted : &js->cfunc_promoted;
}

static inline ant_ptr_pair_map_t *js_cfunc_named_map(ant_t *js, const ant_cfunc_meta_t *meta) {
  ant_realm_t *realm = ant_cfunc_meta_realm(meta);
  return realm ? &realm->cfunc_named : &js->cfunc_named;
}

static inline ant_value_t js_cfunc_lookup_promoted(ant_t *js, ant_value_t cfunc) {
  ant_value_t promoted;
  ant_ptr_pair_map_t *map = js_cfunc_promoted_map(js, js_as_cfunc_meta(cfunc));
  return ant_ptr_pair_get(map, vdata(cfunc), 0, &promoted) ? promoted : cfunc;
}

#endif
