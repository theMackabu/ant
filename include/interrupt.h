#ifndef ANT_INTERRUPT_H
#define ANT_INTERRUPT_H

#include "internal.h"

ant_value_t js_interrupt_raise(ant_t *js);
bool js_is_termination(ant_value_t value);
void sv_jit_arm_interrupts(ant_t *js);

static inline bool js_interrupt_pending(ant_t *js) {
  return __builtin_expect(atomic_load_explicit(&js->interrupt_pending, memory_order_relaxed), 0);
}

static inline bool js_terminating_error(ant_t *js, ant_value_t value) {
  return __builtin_expect(js->terminating, 0) && is_err(value);
}

#endif
