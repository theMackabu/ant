#pragma once

#include <stdint.h>
#include "bridge.h"

void clear_last_result(ant_wasm_runtime_t *runtime);
ant_value_t error_payload(ant_wasm_runtime_t *runtime, ant_value_t fallback);

const uint8_t *serialize_timeout(ant_wasm_runtime_t *runtime);
const uint8_t *serialize_transfer_error(ant_wasm_runtime_t *runtime);
const uint8_t *serialize_value(ant_wasm_runtime_t *runtime, uint8_t status, ant_value_t value);
