#include <string.h>

#include "ant.h"
#include "errors.h"
#include "internal.h"
#include "gc/roots.h"

#include "modules/assert.h"
#include "silver/engine.h"

static ant_value_t assertion_error(ant_t *js, const char *msg, ant_value_t msg_val) {
  if (vtype(msg_val) == kTypeString) {
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
  if (nargs >= 1 && vtype(args[0]) == kTypeString) {
    char *msg = js_getstr(js, args[0], NULL);
    if (msg) return js_mkerr(js, "%s", msg);
  }
  return js_mkerr(js, "Assertion failed");
}

// assert.ifError(value)
static ant_value_t assert_if_error(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  uint8_t t = vtype(args[0]);
  if (t == kTypeNull || t == kTypeUndefined) return js_mkundef();
  if (is_err(args[0])) return args[0];
  char *msg = js_getstr(js, args[0], NULL);
  return js_mkerr(js, "ifError got unwanted exception: %s", msg ? msg : "(unknown)");
}

// TODO: make into global helper
static bool values_loose_equal(ant_t *js, ant_value_t a, ant_value_t b) {
  uint8_t ta = vtype(a), tb = vtype(b);
  if (ta == tb) return strict_eq_values(js, a, b);
  if ((ta == kTypeNull && tb == kTypeUndefined) || (ta == kTypeUndefined && tb == kTypeNull)) return true;
  if (ta == kTypeNumber && tb == kTypeString) {
    char *sb = js_getstr(js, b, NULL);
    return sb && js_getnum(a) == strtod(sb, NULL);
  }
  if (ta == kTypeString && tb == kTypeNumber) {
    char *sa = js_getstr(js, a, NULL);
    return sa && strtod(sa, NULL) == js_getnum(b);
  }
  return false;
}

static bool deep_equal_impl(ant_t *js, ant_value_t a, ant_value_t b, bool strict, int depth) {
  if (depth > 64) return false;
  uint8_t ta = vtype(a), tb = vtype(b);

  if (ta == kTypeArray && tb == kTypeArray) {
    ant_offset_t la = js_arr_len(js, a), lb = js_arr_len(js, b);
    if (la != lb) return false;
    for (ant_offset_t i = 0; i < la; i++) {
      if (!deep_equal_impl(js, js_arr_get(js, a, i), js_arr_get(js, b, i), strict, depth + 1))
        return false;
    }
    return true;
  }

  if (ta == kTypeObject && tb == kTypeObject) {
    if (vdata(a) == vdata(b)) return true;
    ant_iter_t iter = js_prop_iter_begin(js, a);
    const char *key; size_t key_len; ant_value_t va;
    while (js_prop_iter_next(&iter, &key, &key_len, &va)) {
      ant_value_t vb = js_get(js, b, key);
      if (!deep_equal_impl(js, va, vb, strict, depth + 1)) {
        js_prop_iter_end(&iter);
        return false;
      }
    }
    js_prop_iter_end(&iter);
    ant_iter_t iter2 = js_prop_iter_begin(js, b);
    while (js_prop_iter_next(&iter2, &key, &key_len, &va)) {
      ant_value_t va2 = js_get(js, a, key);
      if (vtype(va2) == kTypeUndefined && vtype(va) != kTypeUndefined) {
        js_prop_iter_end(&iter2);
        return false;
      }
    }
    js_prop_iter_end(&iter2);
    return true;
  }

  return strict ? same_value_values(js, a, b) : values_loose_equal(js, a, b);
}

bool js_deep_equal(ant_t *js, ant_value_t a, ant_value_t b, bool strict) {
  return deep_equal_impl(js, a, b, strict, 0);
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
  if (!same_value_values(js, args[0], args[1]))
    return assertion_error(js, "Expected values to be strictly equal", nargs >= 3 ? args[2] : js_mkundef());
  return js_mkundef();
}

static ant_value_t assert_not_strict_equal(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();
  if (same_value_values(js, args[0], args[1]))
    return assertion_error(js, "Expected values to not be strictly equal", nargs >= 3 ? args[2] : js_mkundef());
  return js_mkundef();
}

static ant_value_t assert_deep_equal(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();
  if (!js_deep_equal(js, args[0], args[1], false))
    return assertion_error(js, "Expected values to be deeply equal", nargs >= 3 ? args[2] : js_mkundef());
  return js_mkundef();
}

static ant_value_t assert_not_deep_equal(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();
  if (js_deep_equal(js, args[0], args[1], false))
    return assertion_error(js, "Expected values to not be deeply equal", nargs >= 3 ? args[2] : js_mkundef());
  return js_mkundef();
}

static ant_value_t assert_deep_strict_equal(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();
  if (!js_deep_equal(js, args[0], args[1], true))
    return assertion_error(js, "Expected values to be deeply strictly equal", nargs >= 3 ? args[2] : js_mkundef());
  return js_mkundef();
}

static ant_value_t assert_not_deep_strict_equal(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();
  if (js_deep_equal(js, args[0], args[1], true))
    return assertion_error(js, "Expected values to not be deeply strictly equal", nargs >= 3 ? args[2] : js_mkundef());
  return js_mkundef();
}

static ant_value_t assert_exception_matches(
  ant_t *js, ant_value_t thrown, ant_value_t expected, ant_value_t message
) {
  if (!is_callable(expected)) return js_mkundef();
  ant_value_t prototype = js_get(js, expected, "prototype");
  if (is_err(prototype)) return prototype;
  
  if (!is_undefined(prototype)) {
    ant_value_t matches = do_instanceof(js, thrown, expected);
    if (is_err(matches)) return matches;
    if (js_truthy(js, matches)) return js_mkundef();
  }

  ant_value_t error_ctor = js_get(js, js_glob(js), "Error");
  if (is_err(error_ctor)) return error_ctor;
  if (js_is_prototype_of(js, error_ctor, expected))
    return assertion_error(js, "The error is expected to be an instance of the specified constructor", message);

  ant_value_t receiver = js_mkobj(js);
  if (is_err(receiver)) return receiver;
  ant_value_t valid = sv_vm_call(js->vm, js, expected, receiver, &thrown, 1, NULL, false);
  
  if (is_err(valid)) return valid;
  if (valid != js_true)
    return assertion_error(js, "The validation function is expected to return true", message);
  
  return js_mkundef();
}

static ant_value_t assert_throws(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr(js, "assert.throws: first argument must be a function");

  GC_ROOT_SAVE(mark, js);
  ant_value_t fn = args[0];
  ant_value_t expected = nargs >= 2 ? args[1] : js_mkundef();
  ant_value_t message = nargs >= 3 ? args[2] : js_mkundef();
  ant_value_t thrown = js_mkundef();
  
  GC_ROOT_PIN(js, fn);
  GC_ROOT_PIN(js, expected);
  GC_ROOT_PIN(js, message);
  GC_ROOT_PIN(js, thrown);

  ant_value_t result = sv_vm_call(js->vm, js, fn, js_mkundef(), NULL, 0, NULL, false);
  if (!is_err(result))
    result = assertion_error(js, "Missing expected exception", message);
  else {
    thrown = js_take_thrown(js, result);
    result = assert_exception_matches(js, thrown, expected, message);
  } GC_ROOT_RESTORE(js, mark);
  
  return result;
}

static ant_value_t assert_does_not_throw(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != kTypeFunction)
    return js_mkerr(js, "assert.doesNotThrow: first argument must be a function");
  ant_value_t result = sv_vm_call(js->vm, js, args[0], js_mkundef(), NULL, 0, NULL, false);
  if (is_err(result))
    return js_mkerr(js, "Got unwanted exception: %s", js_str(js, result));
  return js_mkundef();
}

static ant_value_t assert_rejects(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "assert.rejects: first argument required");
  ant_value_t promise = js_mkpromise(js);
  ant_value_t result = vtype(args[0]) == kTypeFunction
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
  ant_value_t result = vtype(args[0]) == kTypeFunction
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
  if (vtype(test_fn) != kTypeFunction && vtype(test_fn) != kTypeBuiltin) return js_mkerr(js, "assert.match: second argument must be a RegExp");
  ant_value_t test_args[1] = {args[0]};
  ant_value_t result = sv_vm_call(js->vm, js, test_fn, args[1], test_args, 1, NULL, false);
  if (!js_truthy(js, result))
    return assertion_error(js, "Value does not match the regular expression", nargs >= 3 ? args[2] : js_mkundef());
  return js_mkundef();
}

static ant_value_t assert_does_not_match(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();
  ant_value_t test_fn = js_getprop_fallback(js, args[1], "test");
  if (vtype(test_fn) != kTypeFunction && vtype(test_fn) != kTypeBuiltin) return js_mkerr(js, "assert.doesNotMatch: second argument must be a RegExp");
  ant_value_t test_args[1] = {args[0]};
  ant_value_t result = sv_vm_call(js->vm, js, test_fn, args[1], test_args, 1, NULL, false);
  if (js_truthy(js, result))
    return assertion_error(js, "Value matches the regular expression", nargs >= 3 ? args[2] : js_mkundef());
  return js_mkundef();
}

static ant_value_t assert_assertion_error_ctor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t self = js_getthis(js);
  js_set(js, self, "name", js_mkstr(js, "AssertionError", 14));
  if (nargs >= 1 && vtype(args[0]) == kTypeObject) {
    const char *fields[] = {"message", "actual", "expected", "operator"};
    for (int i = 0; i < 4; i++) {
      ant_value_t v = js_get(js, args[0], fields[i]);
      if (vtype(v) != kTypeUndefined) js_set(js, self, fields[i], v);
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
  
  ant_value_t ae_fn = js_obj_to_func(js, ae_ctor);
  js_set(js, ae_proto, "constructor", ae_fn);
  js_set(js, assert_obj, "AssertionError", ae_fn);

  ant_value_t lib = js_obj_to_func(js, assert_obj);
  js_set(js, lib, "default", lib);
  
  return lib;
}
