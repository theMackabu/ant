#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "ptr.h"
#include "errors.h"
#include "internal.h"
#include "runtime.h"
#include "sugar.h"

#include "silver/engine.h"
#include "modules/generator.h"
#include "modules/symbol.h"

typedef enum {
  GEN_SUSPENDED_START = 0,
  GEN_SUSPENDED_YIELD = 1,
  GEN_EXECUTING       = 2,
  GEN_COMPLETED       = 3,
} generator_state_t;

enum { GENERATOR_NATIVE_TAG = 0x47454e52u }; // GENR

typedef struct generator_data {
  coroutine_t *coro;
  generator_state_t state;
  bool is_async;
} generator_data_t;

static ant_value_t generator_result(ant_t *js, bool done, ant_value_t value) {
  ant_value_t result = js_mkobj(js);
  js_set(js, result, "done", js_bool(done));
  js_set(js, result, "value", value);
  return result;
}

static generator_data_t *generator_data(ant_value_t gen) {
  if (!js_check_native_tag(gen, GENERATOR_NATIVE_TAG)) return NULL;
  return (generator_data_t *)js_get_native_ptr(gen);
}

static generator_state_t generator_state(ant_value_t gen) {
  generator_data_t *data = generator_data(gen);
  return data ? data->state : GEN_COMPLETED;
}

static void generator_set_state(ant_value_t gen, generator_state_t state) {
  generator_data_t *data = generator_data(gen);
  if (data) data->state = state;
}

static bool generator_is_async(ant_value_t gen) {
  generator_data_t *data = generator_data(gen);
  return data && data->is_async;
}

static ant_value_t generator_async_wrap_result(ant_t *js, ant_value_t result) {
  ant_value_t promise = js_mkpromise(js);
  if (is_err(promise)) return promise;

  if (is_err(result)) {
    ant_value_t reject_value = js->thrown_exists ? js->thrown_value : result;
    js->thrown_exists = false;
    js->thrown_value = js_mkundef();
    js_reject_promise(js, promise, reject_value);
  } else js_resolve_promise(js, promise, result);

  return promise;
}

static coroutine_t *generator_coro(ant_value_t gen) {
  generator_data_t *data = generator_data(gen);
  return data ? data->coro : NULL;
}

coroutine_t *generator_get_coro_for_gc(ant_value_t gen) {
  return generator_coro(gen);
}

static void generator_clear_coro(ant_value_t gen, coroutine_t *coro) {
  generator_data_t *data = generator_data(gen);
  if (data && data->coro == coro) data->coro = NULL;
  if (coro) free_coroutine(coro);
}

static void generator_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t gen = js_obj_from_ptr(obj);
  generator_data_t *data = (generator_data_t *)js_get_native_ptr(gen);
  if (!data) return;

  if (data->coro) {
    free_coroutine(data->coro);
    data->coro = NULL;
  }
  
  js_set_native_ptr(gen, NULL);
  js_set_native_tag(gen, 0);
  free(data);
}

static ant_value_t generator_resume_kind(
  ant_t *js, ant_value_t gen, ant_value_t resume_value, sv_resume_kind_t resume_kind
) {
  coroutine_t *coro = generator_coro(gen);
  if (!coro || !coro->sv_vm) {
    generator_set_state(gen, GEN_COMPLETED);
    if (resume_kind == SV_RESUME_THROW) return js_throw(js, resume_value);
    return generator_result(js, true, resume_kind == SV_RESUME_RETURN ? resume_value : js_mkundef());
  }

  generator_state_t state = generator_state(gen);
  if (state == GEN_EXECUTING) return js_mkerr_typed(
    js, JS_ERR_TYPE, "Generator is already executing"
  );
    
  if (state == GEN_COMPLETED) {
    if (resume_kind == SV_RESUME_THROW) return js_throw(js, resume_value);
    return generator_result(js, true, resume_kind == SV_RESUME_RETURN ? resume_value : js_mkundef());
  }
  
  if (state == GEN_SUSPENDED_START && resume_kind == SV_RESUME_THROW) {
    generator_set_state(gen, GEN_COMPLETED);
    generator_clear_coro(gen, coro);
    return js_throw(js, resume_value);
  }
  
  if (state == GEN_SUSPENDED_START && resume_kind == SV_RESUME_RETURN) {
    generator_set_state(gen, GEN_COMPLETED);
    generator_clear_coro(gen, coro);
    return generator_result(js, true, resume_value);
  }

  generator_set_state(gen, GEN_EXECUTING);
  coroutine_t *saved_active = js->active_async_coro;
  
  coro->active_parent = saved_active;
  js->active_async_coro = coro;

  ant_value_t result;
  if (state == GEN_SUSPENDED_START) {
    sv_closure_t *closure = (vtype(coro->async_func) == T_FUNC) ? js_func_closure(coro->async_func) : NULL;
    if (!closure || !closure->func) result = js_mkerr(js, "invalid generator function");
    else result = sv_execute_closure_entry(
      coro->sv_vm, closure, coro->async_func,
      coro->super_val, coro->this_val, coro->args, coro->nargs, NULL
    );
  } else {
    coro->sv_vm->suspended_resume_value = resume_value;
    coro->sv_vm->suspended_resume_is_error = (resume_kind == SV_RESUME_THROW);
    coro->sv_vm->suspended_resume_kind = resume_kind;
    coro->sv_vm->suspended_resume_pending = true;
    result = sv_resume_suspended(coro->sv_vm);
  }

  js->active_async_coro = saved_active;
  coro->active_parent = NULL;

  if (is_err(result)) {
    generator_set_state(gen, GEN_COMPLETED);
    generator_clear_coro(gen, coro);
    return result;
  }

  if (coro->sv_vm && coro->sv_vm->suspended) {
    generator_set_state(gen, GEN_SUSPENDED_YIELD);
    return generator_result(js, false, result);
  }

  generator_set_state(gen, GEN_COMPLETED);
  generator_clear_coro(gen, coro);
  
  return generator_result(js, true, result);
}

static ant_value_t generator_resume(ant_t *js, ant_value_t gen, ant_value_t resume_value) {
  return generator_resume_kind(js, gen, resume_value, SV_RESUME_NEXT);
}

static ant_value_t generator_next(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t gen = js->this_val;
  if (vtype(gen) != T_GENERATOR)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Generator.prototype.next called on incompatible receiver");

  ant_value_t resume_value = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t result = generator_resume(js, gen, resume_value);
  
  return generator_is_async(gen) ? generator_async_wrap_result(js, result) : result;
}

static ant_value_t generator_return(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t gen = js->this_val;
  if (vtype(gen) != T_GENERATOR)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Generator.prototype.return called on incompatible receiver");

  generator_state_t state = generator_state(gen);
  if (state == GEN_EXECUTING)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Generator is already executing");

  ant_value_t value = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t result = generator_resume_kind(js, gen, value, SV_RESUME_RETURN);
  
  return generator_is_async(gen) ? generator_async_wrap_result(js, result) : result;
}

static ant_value_t generator_throw(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t gen = js->this_val;
  if (vtype(gen) != T_GENERATOR)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Generator.prototype.throw called on incompatible receiver");

  generator_state_t state = generator_state(gen);
  if (state == GEN_EXECUTING)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Generator is already executing");

  ant_value_t value = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t result = generator_resume_kind(js, gen, value, SV_RESUME_THROW);
  
  return generator_is_async(gen) ? generator_async_wrap_result(js, result) : result;
}

void init_generator_module(void) {
  ant_t *js = rt->js;
  ant_value_t proto = js_mkobj(js);
  
  js->sym.generator_proto = proto;
  js_set_proto_init(proto, js->sym.iterator_proto);
  
  js_set(js, proto, "next", js_mkfun(generator_next));
  js_set(js, proto, "return", js_mkfun(generator_return));
  js_set(js, proto, "throw", js_mkfun(generator_throw));
  js_set_sym(js, proto, get_toStringTag_sym(), js_mkstr(js, "Generator", 9));
}

ant_value_t sv_call_generator_closure_dispatch(
  sv_vm_t *caller_vm, ant_t *js, sv_closure_t *closure,
  ant_value_t callee_func, ant_value_t super_val,
  ant_value_t this_val, ant_value_t *args, int argc
) {
  if (!closure || !closure->func) return js_mkerr(js, "invalid generator function");

  sv_vm_t *gen_vm = sv_vm_create(js, SV_VM_ASYNC);
  if (!gen_vm) return js_mkerr(js, "out of memory for generator VM");

  coroutine_t *coro = (coroutine_t *)CORO_MALLOC(sizeof(coroutine_t));
  if (!coro) {
    sv_vm_destroy(gen_vm);
    return js_mkerr(js, "out of memory for generator");
  }

  ant_value_t *copied_args = NULL;
  if (argc > 0 && args) {
    copied_args = (ant_value_t *)CORO_MALLOC(sizeof(ant_value_t) * (size_t)argc);
    if (!copied_args) {
      sv_vm_destroy(gen_vm);
      CORO_FREE(coro);
      return js_mkerr(js, "out of memory for generator args");
    }
    memcpy(copied_args, args, sizeof(ant_value_t) * (size_t)argc);
  }

  ant_value_t gen = js_mkgenerator(js);
  if (is_err(gen)) {
    if (copied_args) CORO_FREE(copied_args);
    sv_vm_destroy(gen_vm);
    CORO_FREE(coro);
    return gen;
  }

  generator_data_t *data = (generator_data_t *)calloc(1, sizeof(*data));
  if (!data) {
    if (copied_args) CORO_FREE(copied_args);
    sv_vm_destroy(gen_vm);
    CORO_FREE(coro);
    return js_mkerr(js, "out of memory for generator data");
  }

  *coro = (coroutine_t){
    .js = js,
    .type = CORO_GENERATOR,
    .this_val = this_val,
    .super_val = super_val,
    .new_target = js->new_target,
    .awaited_promise = js_mkundef(),
    .result = js_mkundef(),
    .async_func = callee_func,
    .args = copied_args,
    .nargs = argc,
    .active_parent = NULL,
    .is_settled = false,
    .is_error = false,
    .is_done = false,
    .resume_point = 0,
    .yield_value = js_mkundef(),
    .async_promise = js_mkundef(),
    .next = NULL,
    .mco = NULL,
    .mco_started = false,
    .is_ready = false,
    .did_suspend = false,
    .sv_vm = gen_vm,
  };

  *data = (generator_data_t){
    .coro = coro,
    .state = GEN_SUSPENDED_START,
    .is_async = closure->func->is_async,
  };

  js_set_native_ptr(gen, data);
  js_set_native_tag(gen, GENERATOR_NATIVE_TAG);
  js_set_finalizer(gen, generator_finalize);

  ant_value_t instance_proto = js_get(js, callee_func, "prototype");
  if (is_object_type(instance_proto)) js_set_proto_wb(js, gen, instance_proto);
  if (data->is_async)
    js_set_sym(js, gen, get_asyncIterator_sym(), js_mkfun(sym_this_cb));

  return gen;
}
