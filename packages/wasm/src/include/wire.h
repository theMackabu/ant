#ifndef ANT_WASM_WIRE_H
#define ANT_WASM_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wasm_abi.h"
#include "bridge.h"

typedef struct {
  uint8_t *data;
  size_t length;
  size_t capacity;
} ant_wire_writer_t;

typedef struct {
  ant_object_t **items;
  size_t length;
  size_t capacity;
} ant_wire_active_t;

char *wire_utf16_to_wtf8(const uint8_t *units, size_t unit_count, size_t *length);

bool wire_writer_u8(ant_wire_writer_t *writer, uint8_t value);
bool wire_writer_u32(ant_wire_writer_t *writer, uint32_t value);
bool wire_writer_string(ant_wire_writer_t *writer, const char *bytes, size_t length);

bool wire_encode(ant_wasm_runtime_t *runtime, ant_value_t value, ant_wire_writer_t *writer);
bool wire_decode(ant_t *js, const void *data, size_t length, ant_value_t *output);

bool wire_encode_value(
  ant_wasm_runtime_t *runtime, ant_value_t value, 
  ant_wire_writer_t *writer, ant_wire_active_t *active, unsigned depth
);

#endif
