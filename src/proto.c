#include <compat.h> // IWYU pragma: keep

#include "proto.h"
#include "modules/object.h"
#include "silver/engine.h"

bool same_object_identity(ant_value_t a, ant_value_t b) {
  if (!is_object_type(a) || !is_object_type(b)) return false;
  return vdata(js_as_obj(a)) == vdata(js_as_obj(b));
}

static inline ant_value_t proto_next_obj_or_null(ant_t *js, ant_value_t cur) {
  if (!is_object_type(cur)) return Ant_Value_Null;
  ant_value_t next = get_proto(js, cur);
  return is_object_type(next) ? next : Ant_Value_Null;
}

void proto_overflow_guard_init(proto_overflow_guard_t *g) {
  g->depth = 0;
  g->overflow = false;
  g->fast = Ant_Value_Null;
}

bool proto_overflow_guard_hit_cycle(ant_t *js, proto_overflow_guard_t *g, ant_value_t cur) {
  if (!g->overflow) {
    if (++g->depth < MAX_PROTO_CHAIN_DEPTH) return false;
    g->overflow = true;
    g->fast = cur;
  }

  g->fast = proto_next_obj_or_null(js, g->fast);
  g->fast = proto_next_obj_or_null(js, g->fast);
  return same_object_identity(cur, g->fast);
}

bool proto_walk_next(ant_t *js, ant_value_t *cur, uint8_t *t, proto_walk_flags_t flags) {
  uint8_t ct = *t;

  if (flags & PROTO_WALK_F_OBJECT_ONLY) {
    if (!is_object_type(*cur)) return false;
    ant_value_t next = get_proto(js, *cur);
    uint8_t nt = vtype(next);
    if (nt == kTypeNull || nt == kTypeUndefined || !is_object_type(next)) return false;
    *cur = next; *t = nt;
    return true;
  }

  if (T_FLAG_FIND(ct) & T_OBJECT_MASK) {
    ant_value_t as_obj = js_as_obj(*cur);
    ant_value_t proto = js_get_slot(as_obj, SLOT_PROTO);
    
    uint8_t pt = vtype(proto);
    if (pt == kTypeObject || pt == kTypeArray || pt == kTypeFunction) {
      *cur = proto;
      *t = pt;
      return true;
    }
    
    if (T_FLAG_FIND(ct) & T_NEEDS_PROTO_FALLBACK) {
      ant_value_t fallback = js_primitive_prototype(js, ct);
      uint8_t ft = vtype(fallback);
      if (ft == kTypeNull || ft == kTypeUndefined) return false;
      *cur = fallback;
      *t = ft;
      return true;
    }
    
    return false;
  }

  if (is_boxable_primitive_type(ct)) {
    ant_value_t proto = js_primitive_prototype(js, ct);
    uint8_t pt = vtype(proto);
    if (pt == kTypeNull || pt == kTypeUndefined) return false;
    *cur = proto; *t = pt;
    return true;
  }

  return false;
}

void proto_walk_overflow_guard_init(proto_walk_overflow_guard_t *g) {
  g->depth = 0;
  g->overflow = false;
  g->fast_active = false;
  g->fast_cur = Ant_Value_Null;
  g->fast_t = kTypeNull;
}

bool proto_walk_overflow_guard_hit_cycle(
  ant_t *js,
  proto_walk_overflow_guard_t *g,
  ant_value_t cur,
  uint8_t cur_t,
  proto_walk_flags_t flags
) {
  if (!g->overflow) {
    if (++g->depth < MAX_PROTO_CHAIN_DEPTH) return false;
    g->overflow = true;
    g->fast_active = true;
    g->fast_cur = cur;
    g->fast_t = cur_t;
  }

  if (g->fast_active && !proto_walk_next(js, &g->fast_cur, &g->fast_t, flags))
    g->fast_active = false;
  if (g->fast_active && !proto_walk_next(js, &g->fast_cur, &g->fast_t, flags))
    g->fast_active = false;

  return g->fast_active && same_object_identity(cur, g->fast_cur);
}

ant_value_t js_instance_proto_from_new_target(ant_t *js, ant_value_t fallback_proto, ant_value_t new_target) {
  ant_value_t instance_proto = Ant_Value_Undefined;

  if (vtype(new_target) == kTypeFunction || vtype(new_target) == kTypeBuiltin) {
    ant_value_t nt_obj = js_as_obj(new_target);
    ant_value_t nt_proto = lkp_interned_val(js, nt_obj, js->intern.prototype);
    if (is_object_type(nt_proto)) instance_proto = nt_proto;
  }

  if (!is_object_type(instance_proto) && is_object_type(fallback_proto)) instance_proto = fallback_proto;
  return instance_proto;
}

bool proto_chain_contains(ant_t *js, ant_value_t obj, ant_value_t proto_target) {
  if (!is_object_type(obj) || !is_object_type(proto_target)) return false;
  
  ant_value_t cur = obj;
  uint8_t t = vtype(cur);
  
  proto_overflow_guard_t guard;
  proto_overflow_guard_init(&guard);

  while (true) {
    if (!proto_walk_next(js, &cur, &t, PROTO_WALK_F_OBJECT_ONLY)) break;
    if (same_object_identity(cur, proto_target)) return true;
    if (proto_overflow_guard_hit_cycle(js, &guard, cur)) break;
  }
  
  return false;
}

bool is_wrapper_ctor_target(ant_t *js, ant_value_t this_val, ant_value_t new_target) {
  if (vtype(new_target) == kTypeUndefined) return false;
  if (vtype(this_val) != kTypeObject) return false;
  if (vtype(js_get_slot(this_val, SLOT_PRIMITIVE)) != kTypeUndefined) return false;
  return true;
}

ant_value_t get_ctor_species_value(ant_t *js, ant_value_t ctor) {
  if (!is_object_type(ctor) && vtype(ctor) != kTypeBuiltin) return Ant_Value_Undefined;
  return js_get_sym(js, ctor, js->sym.species_sym);
}

bool same_ctor_identity(ant_t *js, ant_value_t a, ant_value_t b) {
  if (vtype(a) == vtype(b) && vdata(a) == vdata(b)) return true;
  
  if (vtype(a) == kTypeFunction && vtype(b) == kTypeBuiltin) {
    ant_value_t c = js_get_slot(a, SLOT_CFUNC);
    return vtype(c) == kTypeBuiltin && vdata(c) == vdata(b);
  }
  
  if (vtype(a) == kTypeBuiltin && vtype(b) == kTypeFunction) {
    ant_value_t c = js_get_slot(b, SLOT_CFUNC);
    return vtype(c) == kTypeBuiltin && vdata(c) == vdata(a);
  }
  
  if (vtype(a) == kTypeFunction && vtype(b) == kTypeFunction) {
    ant_value_t ca = js_get_slot(a, SLOT_CFUNC);
    ant_value_t cb = js_get_slot(b, SLOT_CFUNC);
    if (vtype(ca) == kTypeBuiltin && vtype(cb) == kTypeBuiltin && vdata(ca) == vdata(cb)) return true;
  }
  
  return false;
}

static bool proto_chain_contains_cycle_safe(ant_t *js, ant_value_t start, ant_value_t target);

bool js_is_prototype_of(ant_t *js, ant_value_t proto_obj, ant_value_t obj) {
  uint8_t obj_type = vtype(obj);
  if (obj_type != kTypeObject && obj_type != kTypeArray && obj_type != kTypeFunction) return false;
  uint8_t proto_type = vtype(proto_obj);
  if (proto_type != kTypeObject && proto_type != kTypeArray && proto_type != kTypeFunction) return false;
  ant_value_t current = get_proto(js, obj);
  return proto_chain_contains_cycle_safe(js, current, proto_obj);
}

static ant_value_t walk_prototype_chain_ex(
  ant_t *js, ant_value_t l, ant_value_t ctor_proto, bool *saw_proxy
);

ant_value_t js_object_is_prototype_of(ant_t *js, ant_value_t self, ant_value_t value, bool *cacheable) {
  if (cacheable) *cacheable = false;
  if (!is_object_type(value) && vtype(value) != kTypeBuiltin) return Ant_Value_False;

  if (vtype(self) == kTypeUndefined || vtype(self) == kTypeNull)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert undefined or null to object");
  if (!is_object_type(self) && vtype(self) != kTypeBuiltin) return Ant_Value_False;

  bool saw_proxy = false;
  ant_value_t result = walk_prototype_chain_ex(js, value, self, &saw_proxy);
  if (cacheable) *cacheable = !saw_proxy && !is_err(result);
  
  return result;
}

static bool proto_chain_contains_cycle_safe(ant_t *js, ant_value_t start, ant_value_t target) {
  if (!is_object_type(start) || !is_object_type(target)) return false;

  bool found = false;
  ant_value_t slow = start;
  ant_value_t fast = start;

  while (is_object_type(slow)) {
    if (same_object_identity(slow, target)) {
      found = true;
      break;
    }
    slow = get_proto(js, slow);

    if (is_object_type(fast)) fast = get_proto(js, fast);
    if (is_object_type(fast)) fast = get_proto(js, fast);

    if (
      is_object_type(slow) &&
      is_object_type(fast) &&
      same_object_identity(slow, fast)
    ) break;
  }

  return found;
}

bool js_instance_of_proto(ant_t *js, ant_value_t obj, ant_value_t ctor_proto) {
  return proto_chain_contains_cycle_safe(js, get_proto(js, obj), ctor_proto);
}

static ant_value_t walk_prototype_chain_ex(
  ant_t *js, ant_value_t l, ant_value_t ctor_proto, bool *saw_proxy
) {
  ant_value_t current = l;
  proto_overflow_guard_t guard;
  proto_overflow_guard_init(&guard);
  uint32_t proxy_hops = 0;
  for (;;) {
    if (vtype(current) == kTypeBuiltin) current = js->sym.function_proto;
    else if (is_proxy(current)) {
      if (saw_proxy) *saw_proxy = true;
      current = proxy_proto_hop(js, current, &proxy_hops);
      if (is_err(current)) return current;
    } else current = get_proto(js, current);
    if (!is_object_type(current)) return Ant_Value_False;
    if (same_object_identity(current, ctor_proto)) return Ant_Value_True;
    if (proto_overflow_guard_hit_cycle(js, &guard, current)) return Ant_Value_False;
  }
}

ant_value_t walk_prototype_chain(ant_t *js, ant_value_t l, ant_value_t ctor_proto) {
  return walk_prototype_chain_ex(js, l, ctor_proto, NULL);
}
