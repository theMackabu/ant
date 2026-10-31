#include "gc.h"
#include "errors.h"

#include "silver/call.h"
#include "silver/feedback.h"
#include "silver/generic_step.h"
#include "silver/jit.h"
#include "modules/regex.h"
#include "silver/glue.h"
#include "ops/literals.h"
#include "ops/stack.h"
#include "ops/locals.h"
#include "ops/upvalues.h"
#include "ops/globals.h"
#include "ops/property.h"
#include "ops/optional.h"
#include "ops/private.h"
#include "ops/super.h"
#include "ops/arithmetic.h"
#include "ops/comparison.h"
#include "ops/bitwise.h"
#include "ops/unary.h"
#include "ops/control.h"
#include "ops/calls.h"
#include "ops/returns.h"
#include "ops/exceptions.h"
#include "ops/async.h"
#include "ops/iteration.h"
#include "ops/generators.h"
#include "ops/using.h"
#include "ops/objects.h"
#include "ops/coercion.h"

static const bool sv_generic_step_table[OP__COUNT] = {
#define SV_GENERIC_STEP_TABLE
#include "generic_step_cases.h"
#undef SV_GENERIC_STEP_TABLE
};

bool sv_generic_step_supports(sv_op_t op) {
  return op < OP__COUNT && sv_generic_step_table[op];
}

ant_value_t sv_generic_step(sv_vm_t *vm, sv_frame_t *frame, uint8_t *ip, uint32_t depth) {
  ant_t *js = vm->js;
  sv_func_t *func = frame->func;
  
  ant_value_t *lp = frame->lp;
  ant_value_t sv_err = Ant_Value_Undefined;
  
  vm->sp = (int)(lp - vm->stack) + func->max_locals + (int)depth;
  frame->ip = ip;

  #define VM_CHECK(expr) ({            \
    sv_err = (expr);                   \
    if (is_err(sv_err)) goto sv_throw; \
  })
  
  #define VM_THROW(value) do { sv_err = (value); goto sv_throw; } while (0)
  #define NEXT(op) goto sv_next

  switch ((sv_op_t)*ip) {
  #define SV_GENERIC_STEP_CASES
  #include "generic_step_cases.h"
  #undef SV_GENERIC_STEP_CASES
    default: return js_mkerr_typed(js, JS_ERR_INTERNAL, "no generic step for opcode %d", (int)*ip);
  }

  sv_next:
  return 0;

  sv_throw:
  return is_err(sv_err) ? sv_err : js_throw(js, sv_err);

  #undef VM_CHECK
  #undef VM_THROW
  #undef NEXT
}
