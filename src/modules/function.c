#include <compat.h> // IWYU pragma: keep
#include <stdlib.h>
#include <string.h>

#include "cfunc.h"
#include "stringify.h"
#include "silver/compiler.h"
#include "silver/call.h"
#include "silver/jit.h"
#include "modules/function.h"
#include "modules/proxy.h"
#include "operators.h"

ant_value_t js_normalize_sloppy_this(ant_t *js, ant_value_t value) {
  if (vtype(value) == kTypeUndefined || vtype(value) == kTypeNull) return js->global_this;
  if (is_object_type(value) || vtype(value) == kTypeBuiltin) return value;

  uint8_t type = vtype(value);
  if (!is_boxable_primitive_type(type)) return value;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, value);
  ant_value_t wrapper = Ant_Object_CreateBare(js);
  if (!is_err(wrapper)) {
    GC_ROOT_PIN(js, wrapper);
    js_set_slot_wb(js, wrapper, SLOT_PRIMITIVE, value);
    ant_value_t proto = js_primitive_prototype(js, type);
    if (is_object_type(proto)) js_set_proto_wb(js, wrapper, proto);
  }
  GC_ROOT_RESTORE(js, root_mark);
  return wrapper;
}

static const char *const anonymous_prefixes[2][2] = {
  { "function anonymous(", "function* anonymous(" },
  { "async function anonymous(", "async function* anonymous(" },
};

ant_value_t build_dynamic_function(ant_native_params_t, ant_value_t new_target, bool is_async, bool is_generator) {
  size_t params_len = 0;
  
  for (int i = 0; i < nargs - 1; i++) {
    args[i] = coerce_to_str(js, args[i]);
    if (is_err(args[i])) return args[i];
    params_len += vstrlen(js, args[i]);
    if (i < nargs - 2) params_len += 1;
  }
  
  ant_value_t body = nargs > 0 ? coerce_to_str(js, args[nargs - 1]) : js_mkstr(js, "", 0);
  if (is_err(body)) return body;
  size_t body_len = vstrlen(js, body);
  
  const char *prefix = anonymous_prefixes[!!is_async][!!is_generator];
  size_t prefix_len = strlen(prefix);
  
  sv_function_parts_t parts = { .is_async = is_async, .is_generator = is_generator };
  parts.params_off = (ant_offset_t)prefix_len;
  parts.params_end = parts.params_off + (ant_offset_t)params_len;
  parts.body_off = parts.params_end + 5;
  parts.body_end = parts.body_off + (ant_offset_t)body_len;
  parts.len = parts.body_end + 2;
  
  char *text = (char *)malloc((size_t)parts.len + 1);
  if (!text) return js_mkerr(js, "oom");
  
  size_t pos = prefix_len;
  memcpy(text, prefix, prefix_len);
  for (int i = 0; i < nargs - 1; i++) {
    ant_offset_t param_len, param_off = vstr(js, args[i], &param_len);
    memcpy(text + pos, (const void *)(uintptr_t)param_off, param_len);
    pos += param_len;
    if (i < nargs - 2) text[pos++] = ',';
  }
  
  memcpy(text + pos, "\n) {\n", 5);
  pos += 5;
  
  ant_offset_t body_str = vstr(js, body, NULL);
  memcpy(text + pos, (const void *)(uintptr_t)body_str, body_len);
  pos += body_len;
  
  memcpy(text + pos, "\n}", 2);
  pos += 2;
  text[pos] = '\0';
  parts.text = text;

  ant_value_t func_obj = mkobj(js, 0);
  if (is_err(func_obj)) { free(text); return func_obj; }

  sv_func_t *compiled = sv_compile_function(js, &parts);
  if (!compiled) {
    free(text);
    if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "invalid function body");
  }

  sv_closure_t *closure = js_closure_alloc(js);
  if (!closure) {
    sv_code_unit_unpin(compiled->unit);
    free(text);
    return js_mkerr(js, "oom");
  }
  
  closure->func = compiled;
  closure->bound_this = Ant_Value_Undefined;
  closure->call_flags = 0;
  closure->func_obj = func_obj;
  sv_code_unit_unpin(compiled->unit);

  js_set_func_source(js, func_obj, text, (size_t)parts.len,
    compiled->unit ? JS_FUNC_SOURCE_UNIT : JS_FUNC_SOURCE_COPY);
  free(text);
  
  ant_value_t length_result = mkprop_interned_exact(
    js, func_obj, js->intern.length,
    Ant_Value_FromNumber((double)compiled->function_length), ANT_PROP_ATTR_CONFIGURABLE
  );
  
  ant_value_t name_result = is_err(length_result)
    ? length_result
    : js_set_function_name(js, func_obj, "anonymous", 9);
  if (is_err(name_result)) return name_result;
  
  if (is_async && is_generator) {
    js_set_slot(func_obj, SLOT_ASYNC, Ant_Value_True);
    ant_value_t async_generator_proto = js_get_slot(js_glob(js), SLOT_ASYNC_GENERATOR_PROTO);
    if (is_object_type(async_generator_proto)) js_set_proto_init(func_obj, async_generator_proto);
  }
  
  else if (is_async) {
    js_set_slot(func_obj, SLOT_ASYNC, Ant_Value_True);
    ant_value_t async_proto = js_get_slot(js_glob(js), SLOT_ASYNC_PROTO);
    if (is_object_type(async_proto)) js_set_proto_init(func_obj, async_proto);
  }
  
  else if (is_generator) {
    ant_value_t generator_proto = js_get_slot(js_glob(js), SLOT_GENERATOR_PROTO);
    if (is_object_type(generator_proto)) js_set_proto_init(func_obj, generator_proto);
  }
  
  else {
    ant_value_t func_proto = js_get_slot(js_glob(js), SLOT_FUNC_PROTO);
    ant_value_t instance_proto = js_instance_proto_from_new_target(js, func_proto, new_target);
    if (is_object_type(instance_proto)) js_set_proto_init(func_obj, instance_proto);
  }

  ant_value_t func = mkref(kTypeFunction, closure);
  if (!is_async || is_generator) {
    ant_value_t proto_setup = is_generator
      ? setup_func_prototype_property(js, func, false)
      : setup_func_prototype(js, func);
    if (is_err(proto_setup)) return proto_setup;
  }
  
  if (is_generator) {
    ant_value_t prototype = js_get(js, func, "prototype");
    ant_value_t parent_proto = is_async ? js->sym.async_generator_proto : js->sym.generator_proto;
    if (is_object_type(prototype) && is_object_type(parent_proto)) js_set_proto_wb(js, prototype, parent_proto);
  }
  
  return func;
}

ant_value_t builtin_Function(ant_params_t) {
  if (__builtin_expect(js->codegen_blocked & ANT_CODEGEN_NO_STRINGS, 0)) return js_codegen_refused(js);
  return build_dynamic_function(js, args, nargs, call_new_target, false, false);
}

static ant_value_t builtin_AsyncFunction(ant_params_t) {
  if (__builtin_expect(js->codegen_blocked & ANT_CODEGEN_NO_STRINGS, 0)) return js_codegen_refused(js);
  return build_dynamic_function(js, args, nargs, call_new_target, true, false);
}

static ant_value_t builtin_AsyncGeneratorFunction(ant_params_t) {
  if (__builtin_expect(js->codegen_blocked & ANT_CODEGEN_NO_STRINGS, 0)) return js_codegen_refused(js);
  return build_dynamic_function(js, args, nargs, call_new_target, true, true);
}

static ant_value_t builtin_GeneratorFunction(ant_params_t) {
  if (__builtin_expect(js->codegen_blocked & ANT_CODEGEN_NO_STRINGS, 0)) return js_codegen_refused(js);
  return build_dynamic_function(js, args, nargs, call_new_target, false, true);
}

static ant_value_t builtin_function_empty(ant_params_t) {
  return Ant_Value_Undefined;
}

ant_value_t builtin_function_call(ant_params_t) {
  if (vtype(call_this) != kTypeFunction && vtype(call_this) != kTypeBuiltin) {
    return js_mkerr(js, "call requires a function");
  }
  
  ant_value_t this_arg = (nargs > 0) ? args[0] : Ant_Value_Undefined;
  ant_value_t *call_args = NULL;
  
  int call_nargs = (nargs > 1) ? nargs - 1 : 0;
  if (call_nargs > 0) call_args = &args[1];

  return sv_vm_call_explicit_this(js->vm, js, call_this, this_arg, call_args, call_nargs);
}

static ant_value_t builtin_function_toString(ant_params_t) {
  uint8_t t = vtype(call_this);
  
  if (t != kTypeFunction && t != kTypeBuiltin) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Function.prototype.toString requires that 'this' be a Function");
  }
  
  // TODO: make dry
  if (t == kTypeBuiltin) {
    ant_offset_t name_len = 0;
    const char *name = get_func_name(js, call_this, &name_len);
    if (name && name_len > 0) {
      size_t total = 9 + name_len + 21 + 1;
      char *buf = calloc(1, total);
      size_t n = 0;
      n += cpy(buf + n, total - n, "function ", 9);
      n += cpy(buf + n, total - n, name, name_len);
      n += cpy(buf + n, total - n, "() { [native code] }", 20);
      ant_value_t result = js_mkstr(js, buf, n);
      free(buf);
      return result;
    }
    return Ant_String_FromLiteral(js, "function() { [native code] }");
  }
  
  sv_closure_t *bound = js_func_closure(call_this);
  if (bound && (bound->call_flags & SV_CALL_HAS_BOUND_THIS))
    return Ant_String_FromLiteral(js, "function () { [native code] }");

  ant_value_t func_obj = js_func_obj(call_this);
  ant_value_t cfunc_slot = js_get_slot(func_obj, SLOT_CFUNC);
  
  // TODO: make dry
  // native, unless it carries source text (a body-less dynamic function)
  if (vtype(cfunc_slot) == kTypeBuiltin && !func_has_code(js_get_slot(func_obj, SLOT_CODE))) {
    ant_offset_t name_len = 0;
    const char *name = get_func_name(js, call_this, &name_len);
    if (name && name_len > 0) {
      size_t total = 9 + name_len + 21 + 1;
      char *buf = calloc(1, total);
      size_t n = 0;
      n += cpy(buf + n, total - n, "function ", 9);
      n += cpy(buf + n, total - n, name, name_len);
      n += cpy(buf + n, total - n, "() { [native code] }", 20);
      ant_value_t result = js_mkstr(js, buf, n);
      free(buf);
      return result;
    }
    return Ant_String_FromLiteral(js, "function() { [native code] }");
  }
  
  ant_value_t code_val = js_get_slot(func_obj, SLOT_CODE);
  ant_value_t len_val = js_get_slot(func_obj, SLOT_CODE_LEN);
  
  const char *code = func_code_text(js, code_val);
  if (code && vtype(len_val) == kTypeNumber) {
    size_t code_len = (size_t)Ant_Value_AsNumberUnchecked(len_val);
    
    if (code && code_len > 0) {
      ant_value_t async_slot = js_get_slot(func_obj, SLOT_ASYNC);
      sv_closure_t *closure = js_func_closure(call_this);
      
      bool is_async = (async_slot == Ant_Value_True);
      bool is_arrow = closure && (closure->call_flags & SV_CALL_IS_ARROW) != 0;
      
      if (is_arrow) {
        const char *paren_end = memchr(code, ')', code_len);
        if (!paren_end) goto fallback_arrow;
        
        size_t params_len = paren_end - code + 1;
        const char *body = paren_end + 1;
        size_t body_len = code_len - params_len;
        
        size_t len = (is_async ? 6 : 0) + params_len + 4 + body_len + 1;
        char *buf = calloc(1, len);
        size_t n = 0;
        
        if (is_async) n += cpy(buf + n, Ant_Remaining(n, len), "async ", 6);
        n += cpy(buf + n, Ant_Remaining(n, len), code, params_len);
        n += cpy(buf + n, Ant_Remaining(n, len), " => ", 4);
        n += cpy(buf + n, Ant_Remaining(n, len), body, body_len);
        
        ant_value_t result = js_mkstr(js, buf, n);
        free(buf);
        return result;
        fallback_arrow:;
      }
      
      return js_mkstr(js, code, code_len);
    }
  }
  
  sv_closure_t *cl = js_func_closure(call_this);
  if (cl->func != NULL) {
    sv_func_t *fn = cl->func;
    if (fn && fn->debug->source && fn->debug->source_end > fn->debug->source_start) {
      int start = fn->debug->source_start;
      int end   = fn->debug->source_end;
      if (start >= 0 && end <= fn->debug->source_len && end > start)
        return js_mkstr(js, fn->debug->source + start, (size_t)(end - start));
    }
  }

  char stack_buf[256];
  js_cstr_t text = js_to_cstr(js, call_this, stack_buf, sizeof(stack_buf));
  
  ant_value_t result = js_mkstr(js, text.ptr, text.len);
  if (text.needs_free) free((void *)text.ptr);
  
  return result;
}

ant_value_t Ant_Function_ToString(ant_t *js, ant_value_t fn) {
  return builtin_function_toString(js, NULL, 0, Ant_Value_Undefined, fn, Ant_Value_Undefined);
}

ant_value_t builtin_function_apply(ant_params_t) {
  if (vtype(call_this) != kTypeFunction && vtype(call_this) != kTypeBuiltin)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Function.prototype.apply requires that 'this' be a Function");
  
  ant_value_t this_arg = (nargs > 0) ? args[0] : Ant_Value_Undefined;
  ant_value_t arg_array = (nargs > 1) ? args[1] : Ant_Value_Undefined;
  
  if (vtype(arg_array) == kTypeUndefined || vtype(arg_array) == kTypeNull)
    return sv_vm_call_explicit_this(js->vm, js, call_this, this_arg, NULL, 0);
  if (!is_object_type(arg_array) && vtype(arg_array) != kTypeBuiltin)
    return js_mkerr_typed(js, JS_ERR_TYPE, "CreateListFromArrayLike called on non-object");

  ant_arg_list_t list;
  ant_value_t err = Ant_ArgList_FromArrayLike(js, arg_array, &list);
  if (is_err(err)) return err;

  ant_value_t result = sv_vm_call_explicit_this(js->vm, js, call_this, this_arg, list.args, list.argc);
  Ant_ArgList_Release(&list);
  
  return result;
}

bool js_is_function_call_builtin(ant_value_t func) {
  return 
    vtype(func) == kTypeBuiltin && 
    js_as_cfunc(func) == builtin_function_call;
}

bool js_is_function_apply_builtin(ant_value_t func) {
  return 
    vtype(func) == kTypeBuiltin && 
    js_as_cfunc(func) == builtin_function_apply;
}

static ant_value_t builtin_bound_proxy_call(ant_params_t) {
  if (vtype(call_func) != kTypeFunction) return js_mkerr_typed(js, JS_ERR_TYPE, "invalid bound proxy call");

  ant_value_t target = js_get_slot(js_func_obj(call_func), SLOT_TARGET_FUNC);
  if (vtype(target) == kTypeUndefined) return js_mkerr_typed(js, JS_ERR_TYPE, "invalid bound proxy target");

  if (vtype(call_new_target) != kTypeUndefined) {
    return js_proxy_construct(js, target, args, nargs, call_new_target);
  }

  return sv_vm_call(js->vm, js, target, call_this, args, nargs, NULL, Ant_Value_Undefined);
}

static __attribute__((noinline, cold))
bool js_bound_target_next(ant_value_t value, ant_value_t *next) {
  if (vtype(value) != kTypeFunction) return false;
  sv_closure_t *closure = js_func_closure(value);
  if (
    !closure ||
    !(closure->call_flags & (SV_CALL_HAS_BOUND_THIS | SV_CALL_HAS_BOUND_ARGS))
  ) return false;

  ant_value_t target = js_get_slot(js_func_obj(value), SLOT_TARGET_FUNC);
  if (vtype(target) == kTypeUndefined) return false;
  *next = target;
  return true;
}

static __attribute__((noinline, cold))
ant_value_t js_resolve_bound_target_slow(ant_value_t value) {
  ant_value_t slow = value;
  ant_value_t fast = value;
  ant_value_t next;

  for (;;) {
    if (!js_bound_target_next(slow, &next)) break;
    slow = next;
    if (!js_bound_target_next(fast, &next)) break;
    fast = next;
    if (!js_bound_target_next(fast, &next)) break;
    fast = next;
    Ant_Assert(slow != fast, "bound target chain contains a cycle");
  }

  while (js_bound_target_next(value, &next)) value = next;
  return value;
}

ant_value_t js_resolve_bound_target_known_bound(ant_value_t value) {
  ant_value_t target = js_get_slot(js_func_obj(value), SLOT_TARGET_FUNC);
  if (vtype(target) == kTypeUndefined) return value;

  sv_closure_t *closure =
    vtype(target) == kTypeFunction ? js_func_closure(target) : NULL;
  if (
    closure &&
    (closure->call_flags & (SV_CALL_HAS_BOUND_THIS | SV_CALL_HAS_BOUND_ARGS))
  ) return js_resolve_bound_target_slow(value);
  return target;
}

ant_value_t js_resolve_bound_target(ant_value_t value) {
  sv_closure_t *closure =
    vtype(value) == kTypeFunction ? js_func_closure(value) : NULL;
  if (
    !closure ||
    !(closure->call_flags & (SV_CALL_HAS_BOUND_THIS | SV_CALL_HAS_BOUND_ARGS))
  ) return value;
  return js_resolve_bound_target_known_bound(value);
}

static inline bool bound_argv_copy(
  sv_closure_t *closure,
  const ant_value_t *prefix, int prefix_count,
  const ant_value_t *suffix, int suffix_count
) {
  int total = prefix_count + suffix_count;
  
  if (total == 0) {
    closure->u.bound_argv = NULL;
    closure->bound_argc = 0;
    return true;
  }

  ant_value_t *argv = malloc(sizeof(*argv) * (size_t)total);
  if (!argv) return false;
  
  if (prefix_count > 0)
    memcpy(argv, prefix, sizeof(*argv) * (size_t)prefix_count);
  
  if (suffix_count > 0)
    memcpy(argv + prefix_count, suffix, sizeof(*argv) * (size_t)suffix_count);
  
  closure->u.bound_argv = argv;
  closure->bound_argc = total;
  
  return true;
}

ant_value_t js_define_function_length(ant_t *js, ant_value_t fn, int length) {
  return mkprop_interned_exact(js, js_as_obj(fn), js->intern.length, Ant_Value_FromNumber((double)length), ANT_PROP_ATTR_CONFIGURABLE);
}

static ant_value_t builtin_function_bind(ant_params_t) {
  uint8_t func_type = vtype(call_this);
  bool func_is_proxy = func_type == kTypeObject && is_proxy(call_this);
  
  if (func_type != kTypeFunction && func_type != kTypeBuiltin && !func_is_proxy) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "bind requires a function");
  }
  if (func_is_proxy && !is_callable(call_this)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "bind requires a function");
  }

  ant_value_t this_arg = (nargs > 0) ? args[0] : Ant_Value_Undefined;
  int bound_argc = (nargs > 1) ? nargs - 1 : 0;
  ant_value_t *bound_args = (bound_argc > 0) ? &args[1] : NULL;
  
  int orig_length = 0;
  
  if (func_is_proxy) {
    ant_value_t has_length = proxy_has_own(js, call_this, js->length_str);
    if (is_err(has_length)) return has_length;
    ant_value_t len_val = proxy_get(js, call_this, "length", 6);
    if (is_err(len_val)) return len_val;
    if (vtype(len_val) == kTypeNumber) orig_length = (int) Ant_Value_AsNumberUnchecked(len_val);
  } else {
    ant_value_t target = func_type == kTypeBuiltin ? js_cfunc_lookup_promoted(js, call_this) : call_this;
    ant_value_t len_val = vtype(target) == kTypeBuiltin
      ? js_cfunc_length_value(target)
      : lkp_interned_val(js, js_func_obj(target), js->intern.length);
    if (vtype(len_val) == kTypeNumber) orig_length = (int) Ant_Value_AsNumberUnchecked(len_val);
  }
  
  int bound_length = orig_length - bound_argc;
  if (bound_length < 0) bound_length = 0;

  const char *target_name = "";
  size_t target_name_len = 0;
  ant_value_t target_name_val = Ant_Value_Undefined;
  if (func_is_proxy) {
    target_name_val = proxy_get(js, call_this, "name", 4);
    if (is_err(target_name_val)) return target_name_val;
  } else if (func_type == kTypeBuiltin) {
    const ant_cfunc_meta_t *meta = js_as_cfunc_meta(call_this);
    if (meta && meta->name) {
      target_name = meta->name;
      target_name_len = strlen(meta->name);
    }
  } else {
    target_name_val = js_getprop_fallback(js, call_this, "name");
  }
  if (vtype(target_name_val) == kTypeString) {
    ant_offset_t nlen = 0;
    ant_offset_t noff = vstr(js, target_name_val, &nlen);
    target_name = (const char *)(uintptr_t)noff;
    target_name_len = (size_t)nlen;
  }

  if (func_is_proxy) {
    ant_value_t bound_func = mkobj(js, 0);
    if (is_err(bound_func)) return bound_func;

    js_set_slot(bound_func, SLOT_CFUNC, Ant_Function_Create(builtin_bound_proxy_call));
    js_set_slot(bound_func, SLOT_TARGET_FUNC, call_this);

    ant_value_t func_proto = js_get_slot(js_glob(js), SLOT_FUNC_PROTO);
    if (vtype(func_proto) == kTypeFunction) js_set_proto_init(bound_func, func_proto);

    uint8_t bind_flags = SV_CALL_HAS_BOUND_THIS;
    if (bound_argc > 0) bind_flags |= SV_CALL_HAS_BOUND_ARGS;
    
    ant_value_t bound = js_obj_to_func_ex(js, bound_func, bind_flags);
    if (is_err(bound)) return bound;
    
    sv_closure_t *bc = js_func_closure(bound);
    bc->bound_this = this_arg;
    
    if (bound_argc > 0 && !bound_argv_copy(bc, bound_args, bound_argc, NULL, 0))
      return js_mkerr(js, "oom");

    ant_value_t length_result = js_define_function_length(js, bound_func, bound_length);
    if (is_err(length_result)) return length_result;
    
    ant_value_t name_result = js_set_function_name_prefixed(
      js, bound_func, "bound ", 6, 
      target_name, target_name_len
    );
    
    if (is_err(name_result)) return name_result;
    js_mark_constructor(bound_func, js_is_constructor(call_this));

    return bound;
  }

  if (func_type == kTypeBuiltin) {
    ant_value_t bound_func = mkobj(js, 0);
    if (is_err(bound_func)) return bound_func;
    
    js_set_slot(bound_func, SLOT_CFUNC, call_this);
    js_set_slot(bound_func, SLOT_TARGET_FUNC, call_this);
    
    ant_value_t func_proto = js_get_slot(js_glob(js), SLOT_FUNC_PROTO);
    if (vtype(func_proto) == kTypeFunction) js_set_proto_init(bound_func, func_proto);

    uint8_t bind_flags = SV_CALL_HAS_BOUND_THIS;
    if (js_as_cfunc(call_this) == builtin_function_call) bind_flags |= SV_CALL_IS_UNCURRY;
    if (bound_argc > 0) bind_flags |= SV_CALL_HAS_BOUND_ARGS;
    
    ant_value_t bound = js_obj_to_func_ex(js, bound_func, bind_flags);
    if (is_err(bound)) return bound;
    
    sv_closure_t *bc = js_func_closure(bound);
    bc->bound_this = this_arg;
    
    if (
      bound_argc > 0 && 
      !bound_argv_copy(bc, bound_args, bound_argc, NULL, 0)
    ) return js_mkerr(js, "oom");
    
    ant_value_t length_result = js_define_function_length(js, bound_func, bound_length);
    if (is_err(length_result)) return length_result;
    
    ant_value_t name_result = js_set_function_name_prefixed(
      js, bound_func, "bound ", 6, 
      target_name, target_name_len
    );
    
    if (is_err(name_result)) return name_result;
    js_mark_constructor(bound_func, js_is_constructor(call_this));
    
    return bound;
  }

  ant_value_t func_obj = js_func_obj(call_this);
  ant_value_t bound_func = mkobj(js, 0);
  if (is_err(bound_func)) return bound_func;

  sv_closure_t *orig = js_func_closure(call_this);
  sv_closure_t *bound_closure = js_closure_alloc(js);
  if (!bound_closure) return js_mkerr(js, "oom");
  
  bound_closure->func = orig->func;
  if (orig->func) sv_func_note_bound_closure(orig->func);
  bound_closure->call_flags = orig->call_flags;
  bound_closure->upvalues = NULL;
  
  bound_closure->bound_this = 
    (orig->call_flags & (SV_CALL_IS_ARROW | SV_CALL_HAS_BOUND_THIS)) 
    ? orig->bound_this : this_arg;
  
  bound_closure->call_flags |= SV_CALL_HAS_BOUND_THIS;
  bound_closure->super_val = orig->super_val;
  bound_closure->func_obj = bound_func;
  
  if (bound_closure->call_flags & SV_CALL_HAS_BOUND_ARGS)
    bound_closure->u.bound_argv = NULL;
  else {
    bound_closure->u.pending.name = NULL;
    bound_closure->u.pending.len = 0;
  }

  if (orig->func && orig->func->upvalue_count > 0 && orig->upvalues) {
    size_t upvalue_bytes = sizeof(sv_upvalue_t *) * (size_t)orig->func->upvalue_count;
    bound_closure->upvalues = orig->func->upvalue_count <= SV_CLOSURE_INLINE_UPVALS
      ? bound_closure->inline_upvals
      : malloc(upvalue_bytes);
    if (!bound_closure->upvalues) return js_mkerr(js, "oom");
    memcpy(bound_closure->upvalues, orig->upvalues, upvalue_bytes);
  }
  
  int orig_bound_argc = (orig->call_flags & SV_CALL_HAS_BOUND_ARGS) ? orig->bound_argc : 0;
  if (bound_argc > 0 || orig_bound_argc > 0) {
    bound_closure->call_flags |= SV_CALL_HAS_BOUND_ARGS;
    bound_closure->u.bound_argv = NULL;
  }

  ant_value_t async_slot = js_get_slot(func_obj, SLOT_ASYNC);
  if (vtype(async_slot) == kTypeBool && vdata(async_slot) == 1) {
    js_set_slot(bound_func, SLOT_ASYNC, Ant_Value_True);
  }

  ant_value_t target_proto = get_proto(js, call_this);
  if (is_object_type(target_proto) || vtype(target_proto) == kTypeNull) {
    js_set_proto_init(bound_func, target_proto);
  } else if (vtype(async_slot) == kTypeBool && vdata(async_slot) == 1) {
    ant_value_t async_proto = js_get_slot(js_glob(js), SLOT_ASYNC_PROTO);
    if (is_object_type(async_proto)) js_set_proto_init(bound_func, async_proto);
  } else {
    ant_value_t func_proto = js_get_slot(js_glob(js), SLOT_FUNC_PROTO);
    if (vtype(func_proto) == kTypeFunction) js_set_proto_init(bound_func, func_proto);
  }

  ant_value_t data_slot = js_get_slot(func_obj, SLOT_DATA);
  if (vtype(data_slot) != kTypeUndefined) {
    js_set_slot(bound_func, SLOT_DATA, data_slot);
  }

  ant_value_t eval_env = js_get_slot(func_obj, SLOT_EVAL_ENV);
  if (is_object_type(eval_env))
    js_set_slot_wb(js, bound_func, SLOT_EVAL_ENV, eval_env);

  ant_value_t cfunc_slot = js_get_slot(func_obj, SLOT_CFUNC);
  ant_value_t call_target = js_resolve_bound_target(call_this);
  js_set_slot(bound_func, SLOT_TARGET_FUNC, call_target);
  
  if (bound_argc > 0 || orig_bound_argc > 0) if (!bound_argv_copy(
    bound_closure, orig_bound_argc > 0 
      ? orig->u.bound_argv 
      : NULL, orig_bound_argc, bound_args, bound_argc)
  ) return js_mkerr(js, "oom");

  if (vtype(cfunc_slot) == kTypeBuiltin) js_set_slot(bound_func, SLOT_CFUNC, cfunc_slot);
  ant_value_t length_result = js_define_function_length(js, bound_func, bound_length);
  if (is_err(length_result)) return length_result;
  
  ant_value_t name_result = js_set_function_name_prefixed(
    js, bound_func, "bound ", 6, 
    target_name, target_name_len
  );
  
  if (is_err(name_result)) return name_result;
  ant_value_t bound = mkref(kTypeFunction, bound_closure);
  js_mark_constructor(bound_func, js_is_constructor(call_this));
  
  return bound;
}

static ant_value_t builtin_function_has_instance(ant_params_t) {
  return ordinary_has_instance(js, nargs > 0 ? args[0] : Ant_Value_Undefined, call_this);
}

void js_init_function_has_instance(ant_t *js) {
  static const ant_cfunc_meta_t meta = { builtin_function_has_instance, "[Symbol.hasInstance]", 1, 0 };
  mkprop_exact_attrs(js, js->sym.function_proto, js->sym.hasInstance_sym, js_mkfun_meta(&meta), 0);
}

static void init_function_kind(
  ant_t *js, ant_value_t function_proto, ant_value_t function_ctor, internal_slot_t slot,
  ant_value_t cfunc, ant_value_t name, ant_value_t instance_proto, bool is_async
) {
  ant_value_t kind_proto = Ant_Object_CreateBare(js);
  set_proto(js, kind_proto, function_proto);
  
  if (is_async) js_set_slot(kind_proto, SLOT_ASYNC, Ant_Value_True);
  js_set_slot_wb(js, js_glob(js), slot, kind_proto);
  
  ant_value_t ctor_obj = mkobj(js, 0);
  set_proto(js, ctor_obj, function_ctor);
  js_set_slot(ctor_obj, SLOT_CFUNC, cfunc);
  
  mkprop(js, ctor_obj, js->length_str, Ant_Value_FromNumber(1.0), ANT_PROP_ATTR_CONFIGURABLE);
  mkprop(js, ctor_obj, Ant_String_FromLiteral(js, "name"), name, ANT_PROP_ATTR_CONFIGURABLE);
  mkprop_exact_attrs(js, ctor_obj, Ant_String_FromLiteral(js, "prototype"), kind_proto, 0);
  
  if (is_object_type(instance_proto)) {
    mkprop(js, kind_proto, Ant_String_FromLiteral(js, "prototype"), instance_proto, ANT_PROP_ATTR_CONFIGURABLE);
    mkprop(js, instance_proto, Ant_String_FromLiteral(js, "constructor"), kind_proto, ANT_PROP_ATTR_CONFIGURABLE);
  }
  
  mkprop(js, kind_proto, Ant_String_FromLiteral(js, "constructor"), js_obj_to_func(js, ctor_obj), ANT_PROP_ATTR_CONFIGURABLE);
}

void js_function_init_prototype(ant_t *js, ant_value_t function_proto_obj) {
  js_set_slot(function_proto_obj, SLOT_CFUNC, Ant_Function_Create(builtin_function_empty));
  
  defmethod(js, function_proto_obj, "call", 4, Ant_Function_CreateWithArity(builtin_function_call, 1));
  defmethod(js, function_proto_obj, "apply", 5, Ant_Function_CreateWithArity(builtin_function_apply, 2));
  defmethod(js, function_proto_obj, "bind", 4, Ant_Function_CreateWithArity(builtin_function_bind, 1));
  defmethod(js, function_proto_obj, "toString", 8, Ant_Function_Create(builtin_function_toString));
}

void js_function_init_constructor(ant_t *js, ant_value_t func_ctor_obj, ant_value_t function_proto) {
  js_set_slot(func_ctor_obj, SLOT_CFUNC, Ant_Function_CreateWithArity(builtin_Function, 1));
  js_setprop_readonly_nonconfigurable(js, func_ctor_obj, "prototype", 9, function_proto);
  js_set_function_name(js, func_ctor_obj, "Function", sizeof("Function") - 1);
}

void js_function_init_kinds(ant_t *js, ant_value_t function_proto, ant_value_t func_ctor_func) {
  js->sym.generator_proto = Ant_Object_CreateBare(js);
  js->sym.async_generator_proto = Ant_Object_CreateBare(js);
  
  init_function_kind(
    js, function_proto, func_ctor_func, SLOT_ASYNC_PROTO, 
    Ant_Function_Create(builtin_AsyncFunction), Ant_String_FromLiteral(js, "AsyncFunction"), Ant_Value_Undefined, true
  );
  
  init_function_kind(
    js, function_proto, func_ctor_func, SLOT_GENERATOR_PROTO, 
    Ant_Function_Create(builtin_GeneratorFunction), Ant_String_FromLiteral(js, "GeneratorFunction"), js->sym.generator_proto, false
  );
  
  init_function_kind(
    js, function_proto, func_ctor_func, SLOT_ASYNC_GENERATOR_PROTO, 
    Ant_Function_Create(builtin_AsyncGeneratorFunction), Ant_String_FromLiteral(js, "AsyncGeneratorFunction"), js->sym.async_generator_proto, true
  );
}
