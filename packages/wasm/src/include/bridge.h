#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cfunc.h"    // IWYU pragma: export
#include "internal.h" // IWYU pragma: export
#include "wasm_embed.h"

typedef struct {
  ant_t *js;
  uint8_t *last_result;
  uint32_t last_result_length;
  double deadline_ms;
  bool timed_out;
} ant_wasm_runtime_t;

extern ant_wasm_runtime_t *active_runtime;

static inline bool ant_wasm_deadline_expired(ant_wasm_runtime_t *runtime) {
  if (!runtime || !runtime->js->wasm_interrupt_enabled) return false;
  if (runtime->timed_out) return true;
  if (ant_wasm_now_ms() < runtime->deadline_ms) return false;
  runtime->timed_out = true;
  return true;
}

ant_value_t host_function_call(ant_params_t);
