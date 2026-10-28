#ifndef SV_GENERATORS_H
#define SV_GENERATORS_H

#include "ant.h"
#include "errors.h"
#include "gc/roots.h"
#include "silver/call.h"
#include "silver/engine.h"
#include "iteration.h"
#include "modules/generator.h"
#include "protectors.h"

enum {
  SV_YS_ITER = 0,
  SV_YS_NEXT,
  SV_YS_TAG,
  SV_YS_STATE,
};

typedef enum: uint8_t {
  SV_YS_ASYNC_CONTINUE = 0,
  SV_YS_ASYNC_THROW_CLOSED,
  SV_YS_ASYNC_RETURN,
  SV_YS_ASYNC_RETURN_DIRECT,
} sv_ys_async_mode_t;

typedef enum: uint8_t {
  SV_YS_STEP_SUSPEND = 0,
  SV_YS_STEP_CONTINUE,
  SV_YS_STEP_RETURN,
} sv_ys_step_kind_t;

typedef struct {
  ant_value_t status;
  sv_ys_step_kind_t kind;
} sv_ys_step_t;

#define SV_YS_ASYNC_SYNC_SKIP (                                                 \
  sv_op_size[OP_YIELD_STAR_INIT_ASYNC] + sv_op_size[OP_YIELD_STAR_ASYNC_CALL] + \
  sv_op_size[OP_AWAIT] + sv_op_size[OP_YIELD_STAR_ASYNC_RESULT])

#define SV_YS_ASYNC_RESUME_BACK (sv_op_size[OP_AWAIT] + sv_op_size[OP_YIELD_STAR_ASYNC_CALL])
#define SV_YS_ASYNC_DONE_SKIP (sv_op_size[OP_YIELD_STAR_ASYNC_RESULT] + sv_op_size[OP_YIELD_STAR_NEXT])

static inline void sv_yield_star_store_state(sv_vm_t *vm, ant_value_t *lp, uint16_t base) {
  lp[base + SV_YS_ITER] = vm->stack[vm->sp - 3];
  lp[base + SV_YS_NEXT] = vm->stack[vm->sp - 2];
  lp[base + SV_YS_TAG] = vm->stack[vm->sp - 1];
}

static inline void sv_yield_star_push_state(sv_vm_t *vm, ant_value_t *lp, uint16_t base) {
  vm->stack[vm->sp++] = lp[base + SV_YS_ITER];
  vm->stack[vm->sp++] = lp[base + SV_YS_NEXT];
  vm->stack[vm->sp++] = lp[base + SV_YS_TAG];
}

static inline void sv_yield_star_clear_state(ant_t *js, ant_value_t *lp, uint16_t base) {
  lp[base + SV_YS_ITER] = js_mkundef();
  lp[base + SV_YS_NEXT] = js_mkundef();
  lp[base + SV_YS_TAG] = js_mkundef();
  lp[base + SV_YS_STATE] = js_false;
}

static inline ant_value_t sv_yield_star_unpack_result(
  ant_t *js, ant_value_t result, ant_value_t *out_value, bool *out_done, bool *raw
) {
  bool want_raw = raw && *raw;
  if (raw) *raw = false;

  if (!is_object_type(result))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator result is not an object");

  if (want_raw) {
    GC_ROOT_SAVE(raw_mark, js);
    GC_ROOT_PIN(js, result);
    ant_value_t raw_done = sv_iter_result_get_named(js, result, js->intern.done, "done", 4);
    GC_ROOT_RESTORE(js, raw_mark);
    if (is_err(raw_done)) return raw_done;

    *out_done = js_truthy(js, raw_done);
    if (!*out_done) {
      *out_value = result;
      *raw = true;
      return js_mkundef();
    }

    *out_value = sv_iter_result_get_named(js, result, js->intern.value, "value", 5);
    return is_err(*out_value) ? *out_value : js_mkundef();
  }

  ant_value_t done = js_mkundef();
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, result);
  
  sv_iter_result_unpack(js, result, &done, out_value);
  GC_ROOT_RESTORE(js, root_mark);
  
  if (is_err(done)) return done;
  if (is_err(*out_value)) return *out_value;
  *out_done = js_truthy(js, done);
  
  return js_mkundef();
}

static inline ant_value_t sv_yield_star_resume_generator(
  ant_t *js, ant_value_t iterator, ant_value_t arg, sv_resume_kind_t kind,
  ant_value_t *out_value, bool *out_done
) {
  *out_value = js_generator_resume_sync(js, iterator, arg, kind, out_done);
  return is_err(*out_value) ? *out_value : js_mkundef();
}

static inline ant_value_t sv_yield_star_call_return(
  sv_vm_t *vm, ant_t *js, ant_value_t iterator,
  ant_value_t arg, ant_value_t *out_value, bool *out_done, bool *raw
) {
  if (raw) *raw = false;
  ant_value_t method = js_getprop_fallback(js, iterator, "return");
  if (is_err(method)) return method;
  
  if (is_undefined(method) || is_null(method)) {
    *out_value = arg;
    *out_done = true;
    return js_mkundef();
  }
  
  if (!is_callable(method))
    return js_mkerr_typed(js, JS_ERR_TYPE, "iterator.return is not a function");

  if (js_generator_can_resume_sync(iterator, method, SV_RESUME_RETURN))
    return sv_yield_star_resume_generator(js, iterator, arg, SV_RESUME_RETURN, out_value, out_done);

  ant_value_t call_args[1] = { arg };
  ant_value_t result = sv_vm_call(vm, js, method, iterator, call_args, 1, NULL, js_mkundef());
  if (is_err(result)) return result;

  return sv_yield_star_unpack_result(js, result, out_value, out_done, raw);
}

static inline ant_value_t sv_yield_star_close_iterator(sv_vm_t *vm, ant_t *js, ant_value_t iterator) {
  ant_value_t close_value = js_mkundef();
  bool close_done = true;
  return sv_yield_star_call_return(vm, js, iterator, js_mkundef(), &close_value, &close_done, NULL);
}

static inline ant_value_t sv_yield_star_next(
  sv_vm_t *vm, ant_t *js, ant_value_t *lp, uint16_t base,
  ant_value_t sent, ant_value_t *out_value, bool *out_done, bool *raw
) {
  int tag = (int)js_getnum(lp[base + SV_YS_TAG]);
  bool first = js_truthy(js, lp[base + SV_YS_STATE]);
  lp[base + SV_YS_STATE] = js_false;

  if (tag == SV_ITER_GENERIC) {
    ant_value_t iterator = lp[base + SV_YS_ITER];
    ant_value_t next_method = lp[base + SV_YS_NEXT];
    
    if (!is_callable(next_method))
      return js_mkerr(js, "iterator.next is not a function");

    if (js_generator_can_resume_sync(iterator, next_method, SV_RESUME_NEXT)) {
      *raw = false;
      return sv_yield_star_resume_generator(js, iterator, sent, SV_RESUME_NEXT, out_value, out_done);
    }
      
    ant_value_t call_args[1] = { sent };
    ant_value_t result = sv_vm_call(
      vm, js, next_method, iterator, first ? NULL : call_args, first ? 0 : 1,
      NULL, js_mkundef()
    );
    
    if (is_err(result)) return result;
    return sv_yield_star_unpack_result(js, result, out_value, out_done, raw);
  }

  *raw = false;
  sv_yield_star_push_state(vm, lp, base);
  ant_value_t status = sv_iter_advance(vm, js, out_value, out_done);
  if (is_err(status)) return status;
  
  sv_yield_star_store_state(vm, lp, base);
  vm->sp -= 3;
  
  return js_mkundef();
}

static inline ant_value_t sv_yield_star_make_generic(ant_t *js, ant_value_t *lp, uint16_t base) {
  if ((int)js_getnum(lp[base + SV_YS_TAG]) == SV_ITER_GENERIC) return js_mkundef();

  ant_value_t state[3] = { lp[base + SV_YS_ITER], lp[base + SV_YS_NEXT], lp[base + SV_YS_TAG] };
  ant_value_t iterator = sv_iter_record_iterator(js, state);
  if (is_err(iterator)) return iterator;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, iterator);
  
  ant_value_t next_method = js_getprop_fallback(js, iterator, "next");
  GC_ROOT_RESTORE(js, root_mark);
  if (is_err(next_method)) return next_method;

  lp[base + SV_YS_ITER] = iterator;
  lp[base + SV_YS_NEXT] = next_method;
  lp[base + SV_YS_TAG] = tov(SV_ITER_GENERIC);

  return js_mkundef();
}

static inline ant_value_t sv_yield_star_throw(
  sv_vm_t *vm, ant_t *js, ant_value_t *lp, uint16_t base,
  ant_value_t thrown, ant_value_t *out_value, bool *out_done, bool *raw
) {
  bool want_raw = *raw;
  *raw = false;
  
  GC_ROOT_SAVE(thrown_mark, js);
  GC_ROOT_PIN(js, thrown);
  
  ant_value_t generic = sv_yield_star_make_generic(js, lp, base);
  GC_ROOT_RESTORE(js, thrown_mark);
  if (is_err(generic)) return generic;

  ant_value_t iterator = lp[base + SV_YS_ITER];
  ant_value_t throw_method = js_getprop_fallback(js, iterator, "throw");
  if (is_err(throw_method)) return throw_method;
  
  if (!is_callable(throw_method)) {
    ant_value_t close_status = sv_yield_star_close_iterator(vm, js, iterator);
    if (is_err(close_status)) return close_status;
    return js_mkerr_typed(js, JS_ERR_TYPE, "The iterator does not provide a 'throw' method");
  }

  if (js_generator_can_resume_sync(iterator, throw_method, SV_RESUME_THROW))
    return sv_yield_star_resume_generator(js, iterator, thrown, SV_RESUME_THROW, out_value, out_done);

  ant_value_t call_args[1] = { thrown };
  ant_value_t result = sv_vm_call(vm, js, throw_method, iterator, call_args, 1, NULL, js_mkundef());
  if (is_err(result)) return result;

  *raw = want_raw;
  return sv_yield_star_unpack_result(js, result, out_value, out_done, raw);
}

static inline ant_value_t sv_yield_star_return(
  sv_vm_t *vm, ant_t *js, ant_value_t *lp, uint16_t base,
  ant_value_t value, ant_value_t *out_value, bool *out_done, bool *raw
) {
  int tag = (int)js_getnum(lp[base + SV_YS_TAG]);
  bool want_raw = *raw;
  *raw = false;

  if (tag != SV_ITER_GENERIC && ant_protector_intact(js, ANT_PROTECTOR_ITERATOR_RETURN)) {
    *out_value = value;
    *out_done = true;
    return js_mkundef();
  }

  GC_ROOT_SAVE(value_mark, js);
  GC_ROOT_PIN(js, value);
  
  ant_value_t generic = sv_yield_star_make_generic(js, lp, base);
  if (is_err(generic)) {
    GC_ROOT_RESTORE(js, value_mark);
    return generic;
  }

  *raw = want_raw;
  ant_value_t result = sv_yield_star_call_return(vm, js, lp[base + SV_YS_ITER], value, out_value, out_done, raw);
  GC_ROOT_RESTORE(js, value_mark);
  
  return result;
}

static inline ant_value_t sv_yield_star_init(sv_vm_t *vm, ant_t *js, ant_value_t *lp, uint16_t base) {
  ant_value_t status = sv_op_for_of(vm, js);
  if (is_err(status)) return status;
  
  sv_yield_star_store_state(vm, lp, base);
  vm->sp -= 3;
  lp[base + SV_YS_STATE] = js_true;
  vm->stack[vm->sp++] = js_mkundef();
  
  return status;
}

static inline __attribute__((noinline)) ant_value_t sv_yield_star_init_async(
  sv_vm_t *vm, ant_t *js, ant_value_t *lp, uint16_t base
) {
  ant_value_t iterable = vm->stack[vm->sp - 1];
  ant_value_t method = sv_array_lacks_async_iterator(js, iterable)
    ? js_mkundef()
    : js_get_sym(js, iterable, js->sym.asyncIterator_sym);
  
  if (is_err(method)) return method;
  
  if (vtype(method) == kTypeUndefined || vtype(method) == kTypeNull) {
    ant_value_t status = sv_yield_star_init(vm, js, lp, base);
    return is_err(status) ? status : js_false;
  }
  
  if (!is_callable(method))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Symbol.asyncIterator is not a function");

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, method);
  
  ant_value_t iterator = sv_vm_call(vm, js, method, iterable, NULL, 0, NULL, js_mkundef());
  GC_ROOT_RESTORE(js, root_mark);
  
  if (is_err(iterator)) return iterator;
  if (!is_object_type(iterator))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Result of the Symbol.asyncIterator method is not an object");

  lp[base + SV_YS_ITER] = iterator;
  ant_value_t next_method = js_getprop_fallback(js, iterator, "next");
  if (is_err(next_method)) return next_method;
  
  lp[base + SV_YS_NEXT] = next_method;
  lp[base + SV_YS_TAG] = js_mkundef();
  lp[base + SV_YS_STATE] = tov(SV_YS_ASYNC_CONTINUE);
  vm->stack[vm->sp - 1] = js_mkundef();
  
  return js_true;
}

static inline __attribute__((noinline)) ant_value_t sv_yield_star_async_call(
  sv_vm_t *vm, ant_t *js, ant_value_t *lp, uint16_t base, sv_resume_kind_t kind
) {
  ant_value_t iterator = lp[base + SV_YS_ITER];
  ant_value_t received = vm->stack[vm->sp - 1];
  ant_value_t method = js_mkundef();
  sv_ys_async_mode_t mode = SV_YS_ASYNC_CONTINUE;

  switch (kind) {
    case SV_RESUME_NEXT:
      method = lp[base + SV_YS_NEXT];
      if (!is_callable(method)) return js_mkerr_typed(js, JS_ERR_TYPE, "iterator.next is not a function");
      break;
    
    case SV_RESUME_RETURN:
      method = js_getprop_fallback(js, iterator, "return");
      if (is_err(method)) return method;
      if (!is_callable(method)) {
        lp[base + SV_YS_STATE] = tov(SV_YS_ASYNC_RETURN_DIRECT);
        return js_mkundef();
      }
      mode = SV_YS_ASYNC_RETURN;
      break;

    case SV_RESUME_THROW:
      method = js_getprop_fallback(js, iterator, "throw");
      if (is_err(method)) return method;
      if (is_callable(method)) break;
      
      method = js_getprop_fallback(js, iterator, "return");
      if (is_err(method)) return method;
      if (!is_callable(method))
        return js_mkerr_typed(js, JS_ERR_TYPE, "The iterator does not provide a 'throw' method");
      
      mode = SV_YS_ASYNC_THROW_CLOSED;
      break;
  }

  lp[base + SV_YS_STATE] = tov(mode);
  ant_value_t result = sv_vm_call(
    vm, js, method, iterator, &received,
    mode == SV_YS_ASYNC_THROW_CLOSED ? 0 : 1, NULL, js_mkundef()
  );
  
  if (is_err(result)) return result;
  vm->stack[vm->sp - 1] = result;
  
  return js_mkundef();
}

static inline __attribute__((noinline)) sv_ys_step_t sv_yield_star_async_result(
  sv_vm_t *vm, ant_t *js, ant_value_t *lp, uint16_t base
) {
  sv_ys_async_mode_t mode = (sv_ys_async_mode_t)js_getnum(lp[base + SV_YS_STATE]);
  ant_value_t result = vm->stack[vm->sp - 1];
  
  ant_value_t value = result;
  bool done = true;

  if (mode == SV_YS_ASYNC_THROW_CLOSED) {
    ant_value_t err = is_object_type(result)
      ? js_mkerr_typed(js, JS_ERR_TYPE, "The iterator does not provide a 'throw' method")
      : js_mkerr_typed(js, JS_ERR_TYPE, "Iterator result is not an object");
    return (sv_ys_step_t){ .status = err };
  }
  
  if (mode != SV_YS_ASYNC_RETURN_DIRECT) {
    ant_value_t status = sv_yield_star_unpack_result(js, result, &value, &done, NULL);
    if (is_err(status)) return (sv_ys_step_t){ .status = status };
  }
  
  vm->stack[vm->sp - 1] = value;
  if (!done) return (sv_ys_step_t){ .status = js_mkundef(), .kind = SV_YS_STEP_SUSPEND };

  sv_yield_star_clear_state(js, lp, base);
  bool is_return = mode == SV_YS_ASYNC_RETURN || mode == SV_YS_ASYNC_RETURN_DIRECT;

  return (sv_ys_step_t){
    .status = js_mkundef(),
    .kind = is_return ? SV_YS_STEP_RETURN : SV_YS_STEP_CONTINUE,
  };
}

#endif
