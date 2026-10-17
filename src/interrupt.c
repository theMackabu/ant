#include "interrupt.h"
#include "errors.h"

ant_value_t js_interrupt_raise(ant_t *js) {
  js->terminating = true;
  
  ant_value_t error = js_take_thrown(js, js_mkerr(js, "Script execution interrupted"));
  if (is_object_type(error)) js_set_slot(error, SLOT_VM_TERMINATION, js_true);
  
  return js_throw(js, error);
}

bool js_is_termination(ant_value_t value) {
  return is_object_type(value) && js_get_slot(value, SLOT_VM_TERMINATION) == js_true;
}
