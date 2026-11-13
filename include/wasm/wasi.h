#pragma once

#include "types.h"
#include <stdbool.h>
#include <stdint.h>

bool wasi_module_has_wasi_imports(void *c_api_module);
bool wasi_module_is_command_or_reactor(void *c_api_module);

ant_value_t wasi_instantiate(
  ant_t *js, const uint8_t *wasm_bytes, size_t wasm_len,
  ant_value_t module_obj, ant_value_t wasi_opts
);
