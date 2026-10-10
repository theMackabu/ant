#include "errors.h"
#include "internal.h"
#include "gc.h"
#include "gc/roots.h"
#include "silver/call.h"
#include "silver/stack_trace.h"

#include <stdlib.h>
#include <string.h>

bool Ant_Exception_Pending(ant_t *js) {
  return js && js->exception != js_mkundef();
}

ant_value_t Ant_Exception_Peek(ant_t *js) {
  return Ant_Exception_Pending(js) ? js->exception : js_mkundef();
}

ant_value_t Ant_Exception_Current(ant_t *js) {
  if (Ant_Exception_Pending(js)) return js->exception;
  return js && is_err(js->exception_oom) ? js->exception_oom : mkval(kTypeError, 0);
}

void Ant_Exception_Set(ant_t *js, ant_value_t completion) {
  js->exception = is_err(completion) ? completion : js_mkundef();
}

void Ant_Exception_Clear(ant_t *js) {
  js->exception = js_mkundef();
}

static ant_object_t *exception_record(ant_t *js, ant_value_t completion) {
  if (!is_err(completion)) return NULL;
  if (!vdata(completion)) completion = js ? js->exception_oom : js_mkundef();
  if (!is_err(completion) || vdata(completion) < 2) return NULL;
  ant_object_t *record = js_obj_ptr(js_as_obj(completion));
  return record && record->type_tag == kTypeError ? record : NULL;
}

ant_value_t Ant_Exception_Value(ant_t *js, ant_value_t completion) {
  ant_object_t *record = exception_record(js, completion);
  return record ? record->u.exception.value : is_err(completion) ? js_mkundef() : completion;
}

ant_value_t Ant_Exception_Stack(ant_t *js, ant_value_t completion) {
  ant_object_t *record = exception_record(js, completion);
  if (!record) return js_mkundef();
  
  ant_value_t stack = record->u.exception.stack;
  ant_value_t value = record->u.exception.value;
  
  if (js_is_error_record(stack)) {
    stack = js_error_record_throw_text(js, value, stack);
    record->u.exception.stack = vtype(stack) == kTypeString ? stack : js_mkundef();
    gc_write_barrier(js, record, record->u.exception.stack);
    return record->u.exception.stack;
  }
  
  if (vtype(stack) == kTypeString || !is_object_type(value)) return stack;
  if (!js_is_error_record(js_get_slot(value, SLOT_ERROR_STACK))) return stack;
  
  stack = js_error_stack_value(js, value);
  return vtype(stack) == kTypeString ? stack : js_mkundef();
}

ant_value_t Ant_Exception_Raise(ant_t *js, ant_value_t value, ant_value_t stack) {
  ant_value_t completion = Ant_Exception_CreateRecord(js, value, stack);
  Ant_Exception_Set(js, completion);
  return completion;
}

ant_value_t js_take_thrown(ant_t *js, ant_value_t fallback) {
  ant_value_t completion = is_err(fallback) ? fallback : Ant_Exception_Peek(js);
  if (!is_err(completion)) return fallback;
  ant_value_t value = Ant_Exception_Value(js, completion);
  if (js->exception == completion) Ant_Exception_Clear(js);
  return value;
}

ant_value_t Ant_Error_ConsumeMarker(ant_t *js, ant_value_t value) {
  return is_err(value) ? js_take_thrown(js, value) : value;
}

ant_value_t Ant_Error_CallCallback(
  ant_t *js, ant_value_t callback, ant_value_t this_value,
  ant_value_t *args, int nargs
) {
  if (nargs < 1 || !is_err(args[0]))
    return sv_vm_call(js->vm, js, callback, this_value, args, nargs, NULL, js_mkundef());

  ant_value_t inline_args[8];
  ant_value_t *call_args = nargs <= 8 ? inline_args : malloc((size_t)nargs * sizeof(*call_args));

  if (!call_args) return js_mkerr(js, "out of memory invoking error callback");
  memcpy(call_args, args, (size_t)nargs * sizeof(*call_args));

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, callback);
  GC_ROOT_PIN(js, this_value);
  for (int i = 0; i < nargs; i++) GC_ROOT_PIN(js, call_args[i]);

  call_args[0] = Ant_Error_ConsumeMarker(js, call_args[0]);
  ant_value_t result = sv_vm_call(js->vm, js, callback, this_value, call_args, nargs, NULL, js_mkundef());

  GC_ROOT_RESTORE(js, root_mark);
  if (call_args != inline_args) free(call_args);

  return result;
}

bool print_uncaught_throw(ant_t *js) {
  if (!Ant_Exception_Pending(js)) return false;
  print_error_value(js, Ant_Exception_Value(js, Ant_Exception_Peek(js)), Ant_Exception_Stack(js, Ant_Exception_Peek(js)), NULL);
  js_take_thrown(js, js_mkundef());
  return true;
}

bool print_unhandled_promise_rejection(ant_t *js, ant_value_t value) {
  print_error_value(js, value, js_mkundef(), "Uncaught (in promise) ");
  return true;
}

js_err_type_t get_error_type(ant_t *js) {
  if (!Ant_Exception_Pending(js)) return JS_ERR_GENERIC;
  ant_value_t err_type = js_get_slot(Ant_Exception_Value(js, Ant_Exception_Peek(js)), SLOT_ERR_TYPE);
  if (vtype(err_type) != kTypeNumber) return JS_ERR_GENERIC;
  return (js_err_type_t)((int)js_getnum(err_type) & ~JS_ERR_NO_STACK);
}
