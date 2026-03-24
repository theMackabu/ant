#include <math.h>
#include <string.h>

#include "ant.h"
#include "errors.h"
#include "internal.h"
#include "silver/engine.h"

static ant_value_t assertion_error(ant_t *js, const char *msg, ant_value_t msg_val) {
  if (vtype(msg_val) == T_STR) {
    char *s = js_getstr(js, msg_val, NULL);
    if (s) return js_mkerr(js, "%s", s);
  }
  return js_mkerr(js, "%s", msg);
}

// assert(value, message) / assert.ok
static ant_value_t assert_ok(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !js_truthy(js, args[0]))
    return assertion_error(js, "The expression evaluated to a falsy value", nargs >= 2 ? args[1] : js_mkundef());
  return js_mkundef();
}

// assert.fail(message)
static ant_value_t assert_fail(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs >= 1 && vtype(args[0]) == T_STR) {
    char *msg = js_getstr(js, args[0], NULL);
    if (msg) return js_mkerr(js, "%s", msg);
  }
  return js_mkerr(js, "Assertion failed");
}

// assert.ifError(value)
static ant_value_t assert_if_error(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  uint8_t t = vtype(args[0]);
  if (t == T_NULL || t == T_UNDEF) return js_mkundef();
  if (is_err(args[0])) return args[0];
  char *msg = js_getstr(js, args[0], NULL);
  return js_mkerr(js, "ifError got unwanted exception: %s", msg ? msg : "(unknown)");
}

// TODO: make into global helper
static bool values_strict_equal(ant_t *js, ant_value_t a, ant_value_t b) {
  uint8_t ta = vtype(a), tb = vtype(b);
  if (ta != tb) return false;
  if (ta == T_NULL || ta == T_UNDEF) return true;
  if (ta == T_BOOL) return a == b;
  if (ta == T_NUM) {
    double na = js_getnum(a), nb = js_getnum(b);
    return (na == nb) || (isnan(na) && isnan(nb));
  }
  if (ta == T_STR) {
    char *sa = js_getstr(js, a, NULL);
    char *sb = js_getstr(js, b, NULL);
    return sa && sb && strcmp(sa, sb) == 0;
  }
  return vdata(a) == vdata(b);
}

// TODO: make into global helper
static bool values_loose_equal(ant_t *js, ant_value_t a, ant_value_t b) {
  uint8_t ta = vtype(a), tb = vtype(b);
  if (ta == tb) return values_strict_equal(js, a, b);
  if ((ta == T_NULL && tb == T_UNDEF) || (ta == T_UNDEF && tb == T_NULL)) return true;
  if (ta == T_NUM && tb == T_STR) {
    char *sb = js_getstr(js, b, NULL);
    return sb && js_getnum(a) == strtod(sb, NULL);
  }
  if (ta == T_STR && tb == T_NUM) {
    char *sa = js_getstr(js, a, NULL);
    return sa && strtod(sa, NULL) == js_getnum(b);
  }
  return false;
}

// TODO: make into global helper
static bool deep_equal(ant_t *js, ant_value_t a, ant_value_t b, bool strict, int depth) {
  if (depth > 64) return false;
  uint8_t ta = vtype(a), tb = vtype(b);

  if (ta == T_ARR && tb == T_ARR) {
    ant_offset_t la = js_arr_len(js, a), lb = js_arr_len(js, b);
    if (la != lb) return false;
    for (ant_offset_t i = 0; i < la; i++) {
      if (!deep_equal(js, js_arr_get(js, a, i), js_arr_get(js, b, i), strict, depth + 1))
        return false;
    }
    return true;
  }

  if (ta == T_OBJ && tb == T_OBJ) {
    if (vdata(a) == vdata(b)) return true;
    ant_iter_t iter = js_prop_iter_begin(js, a);
    const char *key; size_t key_len; ant_value_t va;
    while (js_prop_iter_next(&iter, &key, &key_len, &va)) {
      ant_value_t vb = js_get(js, b, key);
      if (!deep_equal(js, va, vb, strict, depth + 1)) {
        js_prop_iter_end(&iter);
        return false;
      }
    }
    js_prop_iter_end(&iter);
    ant_iter_t iter2 = js_prop_iter_begin(js, b);
    while (js_prop_iter_next(&iter2, &key, &key_len, &va)) {
      ant_value_t va2 = js_get(js, a, key);
      if (vtype(va2) == T_UNDEF && vtype(va) != T_UNDEF) {
        js_prop_iter_end(&iter2);
        return false;
      }
    }
    js_prop_iter_end(&iter2);
    return true;
  }

  return strict ? values_strict_equal(js, a, b) : values_loose_equal(js, a, b);
}

static ant_value_t assert_equal(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();
  if (!values_loose_equal(js, args[0], args[1]))
    return assertion_error(js, "Expected values to be equal", nargs >= 3 ? args[2] : js_mkundef());
  return js_mkundef();
}

static ant_value_t assert_not_equal(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();
  if (values_loose_equal(js, args[0], args[1]))
    return assertion_error(js, "Expected values to not be equal", nargs >= 3 ? args[2] : js_mkundef());
  return js_mkundef();
}

static ant_value_t assert_strict_equal(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();
  if (!values_strict_equal(js, args[0], args[1]))
    return assertion_error(js, "Expected values to be strictly equal", nargs >= 3 ? args[2] : js_mkundef());
  return js_mkundef();
}

static ant_value_t assert_not_strict_equal(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();
  if (values_strict_equal(js, args[0], args[1]))
    return assertion_error(js, "Expected values to not be strictly equal", nargs >= 3 ? args[2] : js_mkundef());
  return js_mkundef();
}

static ant_value_t assert_deep_equal(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();
  if (!deep_equal(js, args[0], args[1], false, 0))
    return assertion_error(js, "Expected values to be deeply equal", nargs >= 3 ? args[2] : js_mkundef());
  return js_mkundef();
}

static ant_value_t assert_not_deep_equal(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();
  if (deep_equal(js, args[0], args[1], false, 0))
    return assertion_error(js, "Expected values to not be deeply equal", nargs >= 3 ? args[2] : js_mkundef());
  return js_mkundef();
}

static ant_value_t assert_deep_strict_equal(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();
  if (!deep_equal(js, args[0], args[1], true, 0))
    return assertion_error(js, "Expected values to be deeply strictly equal", nargs >= 3 ? args[2] : js_mkundef());
  return js_mkundef();
}

static ant_value_t assert_not_deep_strict_equal(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();
  if (deep_equal(js, args[0], args[1], true, 0))
    return assertion_error(js, "Expected values to not be deeply strictly equal", nargs >= 3 ? args[2] : js_mkundef());
  return js_mkundef();
}

static ant_value_t assert_throws(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_FUNC)
    return js_mkerr(js, "assert.throws: first argument must be a function");
  ant_value_t result = sv_vm_call(js->vm, js, args[0], js_mkundef(), NULL, 0, NULL, false);
  if (!is_err(result))
    return js_mkerr(js, "Missing expected exception");
  return js_mkundef();
}

static ant_value_t assert_does_not_throw(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_FUNC)
    return js_mkerr(js, "assert.doesNotThrow: first argument must be a function");
  ant_value_t result = sv_vm_call(js->vm, js, args[0], js_mkundef(), NULL, 0, NULL, false);
  if (is_err(result))
    return js_mkerr(js, "Got unwanted exception: %s", js_str(js, result));
  return js_mkundef();
}

static bool promise_was_rejected(ant_value_t result) {
  if (vtype(result) != T_PROMISE) return false;
  ant_object_t *obj = js_obj_ptr(js_as_obj(result));
  return obj && obj->promise_state && obj->promise_state->state == 2;
}

static void promise_mark_handled(ant_value_t result) {
  if (vtype(result) != T_PROMISE) return;
  ant_object_t *obj = js_obj_ptr(js_as_obj(result));
  if (obj && obj->promise_state) obj->promise_state->has_rejection_handler = true;
}

static bool promise_was_fulfilled(ant_value_t result) {
  if (vtype(result) != T_PROMISE) return false;
  ant_object_t *obj = js_obj_ptr(js_as_obj(result));
  return obj && obj->promise_state && obj->promise_state->state == 1;
}

static ant_value_t assert_rejects(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "assert.rejects: first argument required");
  ant_value_t promise = js_mkpromise(js);
  ant_value_t result = vtype(args[0]) == T_FUNC
    ? sv_vm_call(js->vm, js, args[0], js_mkundef(), NULL, 0, NULL, false)
    : args[0];
  if (is_err(result) || promise_was_rejected(result)) {
    promise_mark_handled(result);
    js_resolve_promise(js, promise, js_mkundef());
  } else js_reject_promise(js, promise, js_mkerr(js, "Missing expected rejection"));
  return promise;
}

static ant_value_t assert_does_not_reject(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "assert.doesNotReject: first argument required");
  ant_value_t promise = js_mkpromise(js);
  ant_value_t result = vtype(args[0]) == T_FUNC
    ? sv_vm_call(js->vm, js, args[0], js_mkundef(), NULL, 0, NULL, false)
    : args[0];
  if (is_err(result) || promise_was_rejected(result)) {
    promise_mark_handled(result);
    js_reject_promise(js, promise, js_mkerr(js, "Got unwanted rejection"));
  } else js_resolve_promise(js, promise, js_mkundef());
  return promise;
}

static ant_value_t assert_match(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();
  ant_value_t test_fn = js_getprop_fallback(js, args[1], "test");
  if (vtype(test_fn) != T_FUNC && vtype(test_fn) != T_CFUNC) return js_mkerr(js, "assert.match: second argument must be a RegExp");
  ant_value_t test_args[1] = {args[0]};
  ant_value_t result = sv_vm_call(js->vm, js, test_fn, args[1], test_args, 1, NULL, false);
  if (!js_truthy(js, result))
    return assertion_error(js, "Value does not match the regular expression", nargs >= 3 ? args[2] : js_mkundef());
  return js_mkundef();
}

static ant_value_t assert_does_not_match(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();
  ant_value_t test_fn = js_getprop_fallback(js, args[1], "test");
  if (vtype(test_fn) != T_FUNC && vtype(test_fn) != T_CFUNC) return js_mkerr(js, "assert.doesNotMatch: second argument must be a RegExp");
  ant_value_t test_args[1] = {args[0]};
  ant_value_t result = sv_vm_call(js->vm, js, test_fn, args[1], test_args, 1, NULL, false);
  if (js_truthy(js, result))
    return assertion_error(js, "Value matches the regular expression", nargs >= 3 ? args[2] : js_mkundef());
  return js_mkundef();
}

static ant_value_t assert_assertion_error_ctor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t self = js_getthis(js);
  js_set(js, self, "name", js_mkstr(js, "AssertionError", 14));
  if (nargs >= 1 && vtype(args[0]) == T_OBJ) {
    const char *fields[] = {"message", "actual", "expected", "operator"};
    for (int i = 0; i < 4; i++) {
      ant_value_t v = js_get(js, args[0], fields[i]);
      if (vtype(v) != T_UNDEF) js_set(js, self, fields[i], v);
    }
  }
  return js_mkundef();
}

ant_value_t assert_library(ant_t *js) {
  ant_value_t assert_obj = js_mkobj(js);
  js_set_slot(assert_obj, SLOT_CFUNC, js_mkfun(assert_ok));

  js_set(js, assert_obj, "ok", js_mkfun(assert_ok));
  js_set(js, assert_obj, "fail", js_mkfun(assert_fail));
  js_set(js, assert_obj, "ifError", js_mkfun(assert_if_error));
  js_set(js, assert_obj, "equal", js_mkfun(assert_equal));
  js_set(js, assert_obj, "notEqual", js_mkfun(assert_not_equal));
  js_set(js, assert_obj, "strictEqual", js_mkfun(assert_strict_equal));
  js_set(js, assert_obj, "notStrictEqual", js_mkfun(assert_not_strict_equal));
  js_set(js, assert_obj, "deepEqual", js_mkfun(assert_deep_equal));
  js_set(js, assert_obj, "notDeepEqual", js_mkfun(assert_not_deep_equal));
  js_set(js, assert_obj, "deepStrictEqual", js_mkfun(assert_deep_strict_equal));
  js_set(js, assert_obj, "notDeepStrictEqual", js_mkfun(assert_not_deep_strict_equal));
  js_set(js, assert_obj, "throws", js_mkfun(assert_throws));
  js_set(js, assert_obj, "doesNotThrow", js_mkfun(assert_does_not_throw));
  js_set(js, assert_obj, "rejects", js_mkfun(assert_rejects));
  js_set(js, assert_obj, "doesNotReject", js_mkfun(assert_does_not_reject));
  js_set(js, assert_obj, "match", js_mkfun(assert_match));
  js_set(js, assert_obj, "doesNotMatch", js_mkfun(assert_does_not_match));

  ant_value_t ae_ctor = js_mkobj(js);
  ant_value_t ae_proto = js_mkobj(js);
  
  js_set(js, ae_proto, "name", js_mkstr(js, "AssertionError", 14));
  js_set_slot(ae_ctor, SLOT_CFUNC, js_mkfun(assert_assertion_error_ctor));
  js_mkprop_fast(js, ae_ctor, "prototype", 9, ae_proto);
  js_mkprop_fast(js, ae_ctor, "name", 4, js_mkstr(js, "AssertionError", 14));
  
  ant_value_t ae_fn = js_obj_to_func(ae_ctor);
  js_set(js, ae_proto, "constructor", ae_fn);
  js_set(js, assert_obj, "AssertionError", ae_fn);

  ant_value_t lib = js_obj_to_func(assert_obj);
  js_set(js, lib, "default", lib);
  
  return lib;
}
