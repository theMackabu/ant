#pragma once

#include "internal.h"

typedef struct {
  sv_frame_t *direct_frame;
  sv_upvalue_t **cells;
  int frame_index;
  uint32_t mapped_count;
  uint8_t in_setter;
  uint8_t deleted[];
} ant_arguments_state_t;

typedef struct {
  bool accessor;
  bool has_value;
  bool has_writable;
  bool writable;
  bool has_enumerable;
  bool enumerable;
  bool has_configurable;
  bool configurable;
  bool abrupt;
  ant_value_t value;
} define_desc_fields_t;

ant_arguments_state_t *js_arguments_state(ant_value_t obj);

ant_value_t js_arguments_getter(ant_t *js, ant_value_t obj, const char *key, size_t key_len);
ant_value_t define_abrupt(define_desc_fields_t *fields, ant_value_t err);

void js_arguments_after_define(
  ant_t *js, ant_value_t obj, const char *key, 
  size_t key_len, const define_desc_fields_t *desc
);

void js_arguments_unmap_all(ant_t *js, ant_value_t obj);
bool js_arguments_setter(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t value);
