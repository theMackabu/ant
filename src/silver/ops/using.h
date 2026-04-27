#ifndef SV_USING_H
#define SV_USING_H

#include "silver/engine.h"
#include "errors.h"
#include "gc/roots.h"
#include "modules/symbol.h"

typedef enum {
  SV_DISPOSAL_RECORD_DEFER = 0,
  SV_DISPOSAL_RECORD_ADOPT = 1,
  SV_DISPOSAL_RECORD_USE = 2
} sv_disposal_record_kind_t;

static inline void sv_using_array_clear(ant_value_t arr) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(arr));
  if (ptr && ptr->type_tag == T_ARR) ptr->u.array.len = 0;
}

static inline ant_value_t sv_make_suppressed_error_value(
  ant_t *js, ant_value_t error, ant_value_t suppressed
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, error);
  GC_ROOT_PIN(js, suppressed);

  ant_value_t obj = js_mkobj(js);
  GC_ROOT_PIN(js, obj);
  if (is_err(obj)) {
    GC_ROOT_RESTORE(js, root_mark);
    return obj;
  }

  ant_value_t proto = js_get_ctor_proto(js, "SuppressedError", 15);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);

  js_mkprop_fast(js, obj, "error", 5, error);
  js_mkprop_fast(js, obj, "suppressed", 10, suppressed);
  js_mkprop_fast(js, obj, "message", 7, ANT_STRING("An error was suppressed during disposal."));
  js_mkprop_fast(js, obj, "name", 4, ANT_STRING("SuppressedError"));
  js_set_slot(obj, SLOT_ERROR_BRAND, js_true);
  js_capture_stack(js, obj);

  GC_ROOT_RESTORE(js, root_mark);
  return obj;
}

static inline ant_value_t sv_disposal_error_value(ant_t *js, ant_value_t result) {
  if (js->thrown_exists) {
    ant_value_t thrown = js->thrown_value;
    js->thrown_value = js_mkundef();
    js->thrown_stack = js_mkundef();
    js->thrown_exists = false;
    return thrown;
  }

  if (is_err(result)) {
    if (vdata(result) != 0) return mkval(T_OBJ, vdata(result));
    return js_make_error_silent(js, JS_ERR_INTERNAL, "unknown disposal error");
  }

  return result;
}

static inline ant_value_t sv_suppress_disposal_error(
  ant_t *js, ant_value_t error, ant_value_t previous
) {
  if (vtype(previous) == T_UNDEF) return error;
  return sv_make_suppressed_error_value(js, error, previous);
}

static inline ant_value_t sv_disposal_record_call(ant_t *js, ant_value_t record) {
  ant_value_t kind_v = js_arr_get(js, record, 0);
  ant_value_t value = js_arr_get(js, record, 1);
  ant_value_t method = js_arr_get(js, record, 2);
  
  int kind = vtype(kind_v) == T_NUM 
    ? (int)js_getnum(kind_v) : -1;

  ant_value_t this_arg = kind == SV_DISPOSAL_RECORD_USE ? value : js_mkundef();
  ant_value_t arg = value;
  
  ant_value_t *args = kind == SV_DISPOSAL_RECORD_ADOPT ? &arg : NULL;
  int nargs = kind == SV_DISPOSAL_RECORD_ADOPT ? 1 : 0;

  if (!is_callable(method))
    return js_mkerr_typed(js, JS_ERR_TYPE, "disposer is not callable");
  
  if (vtype(method) == T_CFUNC) {
    ant_value_t saved_this = js->this_val;
    js->this_val = this_arg;
    ant_value_t result = js_as_cfunc(method)(js, args, nargs);
    js->this_val = saved_this;
    return result;
  }

  return sv_vm_call(js->vm, js, method, this_arg, args, nargs, NULL, false);
}

static inline ant_value_t sv_dispose_resource(ant_t *js, ant_value_t resource, bool is_async) {
  if (vtype(resource) == T_NULL || vtype(resource) == T_UNDEF) return js_mkundef();

  ant_value_t method = js_get_sym(js, resource, is_async ? get_asyncDispose_sym() : get_dispose_sym());
  if (is_async && (vtype(method) == T_UNDEF || vtype(method) == T_NULL)) method = js_get_sym(js, resource, get_dispose_sym());
    
  if (!is_callable(method)) return js_mkerr_typed(
    js, JS_ERR_TYPE, is_async 
      ? "resource is not async disposable" 
      : "resource is not disposable"
  );

  if (vtype(method) == T_CFUNC) {
    ant_value_t saved_this = js->this_val;
    js->this_val = resource;
    ant_value_t result = js_as_cfunc(method)(js, NULL, 0);
    js->this_val = saved_this;
    return result;
  }

  return sv_vm_call(js->vm, js, method, resource, NULL, 0, NULL, false);
}

static inline ant_value_t sv_using_push(
  ant_t *js, ant_value_t entries, ant_value_t resource, bool is_async
) {
  if (vtype(resource) == T_NULL || vtype(resource) == T_UNDEF)
    return resource;
  
  if (vtype(entries) != T_ARR)
    return js_mkerr_typed(js, JS_ERR_TYPE, "invalid using disposal stack");

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, entries);
  GC_ROOT_PIN(js, resource);

  ant_value_t method = js_get_sym(js, resource, is_async ? get_asyncDispose_sym() : get_dispose_sym());
  if (is_async && (vtype(method) == T_UNDEF || vtype(method) == T_NULL)) method = js_get_sym(js, resource, get_dispose_sym());
  
  GC_ROOT_PIN(js, method);
  if (!is_callable(method)) {
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkerr_typed(js, JS_ERR_TYPE, "resource is not disposable");
  }

  ant_value_t record = js_mkarr(js);
  GC_ROOT_PIN(js, record);
  if (is_err(record)) {
    GC_ROOT_RESTORE(js, root_mark);
    return record;
  }
  
  js_arr_push(js, record, js_mknum((double)SV_DISPOSAL_RECORD_USE));
  js_arr_push(js, record, resource);
  js_arr_push(js, record, method);
  js_arr_push(js, entries, record);
  GC_ROOT_RESTORE(js, root_mark);
  
  return resource;
}

static inline ant_value_t sv_using_dispose_sync(
  ant_t *js, ant_value_t entries, ant_value_t completion, bool throw_completion
) {
  if (vtype(entries) != T_ARR) return js_mkerr_typed(js, JS_ERR_TYPE, "invalid using disposal stack");

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, entries);
  GC_ROOT_PIN(js, completion);

  ant_offset_t len = js_arr_len(js, entries);
  ant_value_t work = js_mkarr(js);
  GC_ROOT_PIN(js, work);
  
  if (is_err(work)) {
    GC_ROOT_RESTORE(js, root_mark);
    return work;
  }

  for (ant_offset_t i = 0; i < len; i++) {
    GC_ROOT_SAVE(copy_mark, js);
    ant_value_t record = js_arr_get(js, entries, i);
    GC_ROOT_PIN(js, record);
    js_arr_push(js, work, record);
    GC_ROOT_RESTORE(js, copy_mark);
  }
  
  sv_using_array_clear(entries);
  len = js_arr_len(js, work);
  
  for (ant_offset_t i = len; i > 0; i--) {
    GC_ROOT_SAVE(iter_mark, js);
    ant_value_t record = js_arr_get(js, work, i - 1);
    
    GC_ROOT_PIN(js, record);
    ant_value_t result = sv_disposal_record_call(js, record);
    
    if (is_err(result) || js->thrown_exists) {
      ant_value_t error = sv_disposal_error_value(js, result);
      GC_ROOT_PIN(js, error);
      completion = sv_suppress_disposal_error(js, error, completion);
      
      if (is_err(completion)) {
        GC_ROOT_RESTORE(js, root_mark);
        return completion;
      }
    }
    
    GC_ROOT_RESTORE(js, iter_mark);
  }

  if (throw_completion && vtype(completion) != T_UNDEF) {
    ant_value_t thrown = js_throw(js, completion);
    GC_ROOT_RESTORE(js, root_mark);
    return thrown;
  }

  GC_ROOT_RESTORE(js, root_mark);
  return completion;
}

static inline ant_value_t sv_async_dispose_continue(
  ant_t *js,
  ant_value_t state,
  bool rejected,
  ant_value_t reason
);

static inline ant_value_t sv_async_dispose_on_fulfilled(
  ant_t *js,
  ant_value_t *args,
  int nargs
) {
  (void)args; (void)nargs;
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  return sv_async_dispose_continue(js, state, false, js_mkundef());
}

static inline ant_value_t sv_async_dispose_on_rejected(
  ant_t *js,
  ant_value_t *args,
  int nargs
) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  return sv_async_dispose_continue(js, state, true, nargs > 0 ? args[0] : js_mkundef());
}

static inline ant_value_t sv_async_dispose_continue(
  ant_t *js,
  ant_value_t state,
  bool rejected,
  ant_value_t reason
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, state);
  GC_ROOT_PIN(js, reason);

  ant_value_t entries = js_get_slot(state, SLOT_ENTRIES);
  ant_value_t result_promise = js_get_slot(state, SLOT_DATA);
  ant_value_t completion = js_get_slot(state, SLOT_AUX);
  GC_ROOT_PIN(js, entries);
  GC_ROOT_PIN(js, result_promise);
  GC_ROOT_PIN(js, completion);

  if (rejected) {
    completion = sv_suppress_disposal_error(js, reason, completion);
    GC_ROOT_PIN(js, completion);
    js_set_slot(state, SLOT_AUX, completion);
  }

  ant_value_t idx_v = js_get_slot(state, SLOT_ITER_STATE);
  ant_offset_t idx = vtype(idx_v) == T_NUM ? (ant_offset_t)js_getnum(idx_v) : 0;

  while (idx > 0) {
    GC_ROOT_SAVE(iter_mark, js);
    idx--;
    js_set_slot(state, SLOT_ITER_STATE, js_mknum((double)idx));
    
    ant_value_t record = js_arr_get(js, entries, idx);
    GC_ROOT_PIN(js, record);
    ant_value_t result = sv_disposal_record_call(js, record);
    GC_ROOT_PIN(js, result);
    
    if (is_err(result) || js->thrown_exists) {
      ant_value_t error = sv_disposal_error_value(js, result);
      GC_ROOT_PIN(js, error);
      completion = sv_suppress_disposal_error(js, error, completion);
      js_set_slot(state, SLOT_AUX, completion);
      GC_ROOT_RESTORE(js, iter_mark);
      continue;
    }
    
    if (vtype(result) == T_PROMISE) {
      ant_value_t on_fulfilled = js_heavy_mkfun(js, sv_async_dispose_on_fulfilled, state);
      GC_ROOT_PIN(js, on_fulfilled);
      ant_value_t on_rejected = js_heavy_mkfun(js, sv_async_dispose_on_rejected, state);
      GC_ROOT_PIN(js, on_rejected);
      js_promise_then(js, result, on_fulfilled, on_rejected);
      GC_ROOT_RESTORE(js, iter_mark);
      GC_ROOT_RESTORE(js, root_mark);
      return result_promise;
    }

    GC_ROOT_RESTORE(js, iter_mark);
  }

  if (vtype(completion) != T_UNDEF) js_reject_promise(js, result_promise, completion);
  else js_resolve_promise(js, result_promise, js_mkundef());

  GC_ROOT_RESTORE(js, root_mark);
  return result_promise;
}

static inline ant_value_t sv_using_dispose_async(
  ant_t *js,
  ant_value_t entries,
  ant_value_t completion
) {
  ant_value_t result_promise = js_mkpromise(js);
  if (is_err(result_promise)) return result_promise;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, result_promise);
  GC_ROOT_PIN(js, completion);

  if (vtype(entries) != T_ARR) {
    ant_value_t error = js_make_error_silent(js, JS_ERR_TYPE, "invalid using disposal stack");
    GC_ROOT_PIN(js, error);
    js_reject_promise(js, result_promise, error);
    GC_ROOT_RESTORE(js, root_mark);
    return result_promise;
  }

  GC_ROOT_PIN(js, entries);

  ant_offset_t len = js_arr_len(js, entries);
  ant_value_t work = js_mkarr(js);
  GC_ROOT_PIN(js, work);
  if (is_err(work)) {
    GC_ROOT_RESTORE(js, root_mark);
    return work;
  }

  for (ant_offset_t i = 0; i < len; i++) {
    GC_ROOT_SAVE(copy_mark, js);
    ant_value_t record = js_arr_get(js, entries, i);
    GC_ROOT_PIN(js, record);
    js_arr_push(js, work, record);
    GC_ROOT_RESTORE(js, copy_mark);
  }
  
  sv_using_array_clear(entries);
  ant_value_t state = js_mkobj(js);
  
  GC_ROOT_PIN(js, state);
  if (is_err(state)) {
    GC_ROOT_RESTORE(js, root_mark);
    return state;
  }

  js_set_slot(state, SLOT_ENTRIES, work);
  js_set_slot(state, SLOT_DATA, result_promise);
  js_set_slot(state, SLOT_AUX, completion);
  js_set_slot(state, SLOT_ITER_STATE, js_mknum((double)js_arr_len(js, work)));

  ant_value_t result = sv_async_dispose_continue(js, state, false, js_mkundef());
  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static inline ant_value_t sv_using_dispose(
  ant_t *js,
  ant_value_t entries,
  ant_value_t completion,
  bool is_async,
  bool suppressed
) {
  ant_value_t actual_completion = suppressed ? completion : js_mkundef();
  if (is_async) return sv_using_dispose_async(js, entries, actual_completion);
  return sv_using_dispose_sync(js, entries, actual_completion, !suppressed);
}

static inline ant_value_t sv_op_using_push(sv_vm_t *vm, ant_t *js, bool is_async) {
  ant_value_t resource = vm->stack[--vm->sp];
  ant_value_t entries = vm->stack[--vm->sp];
  ant_value_t result = sv_using_push(js, entries, resource, is_async);
  if (!is_err(result)) vm->stack[vm->sp++] = result;
  return result;
}

static inline ant_value_t sv_op_dispose_resource(sv_vm_t *vm, ant_t *js, bool is_async) {
  ant_value_t resource = vm->stack[--vm->sp];
  ant_value_t result = sv_dispose_resource(js, resource, is_async);
  if (!is_err(result)) vm->stack[vm->sp++] = result;
  return result;
}

static inline ant_value_t sv_op_using_dispose(
  sv_vm_t *vm,
  ant_t *js,
  bool is_async,
  bool suppressed
) {
  ant_value_t completion = suppressed ? vm->stack[--vm->sp] : js_mkundef();
  ant_value_t entries = vm->stack[--vm->sp];
  ant_value_t result = sv_using_dispose(js, entries, completion, is_async, suppressed);
  if (!is_err(result)) vm->stack[vm->sp++] = result;
  return result;
}

#endif
