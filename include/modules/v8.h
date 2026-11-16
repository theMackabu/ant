#pragma once

#include "types.h"
#include "uthash.h"

typedef struct {
  ant_value_t key;
  uint32_t id;
  UT_hash_handle hh;
} ser_ref_t;

typedef struct {
  ant_t *js;
  
  ant_value_t self;
  ant_value_t seen;
  
  uint8_t *data;
  size_t len;
  size_t cap;
  
  ser_ref_t *refs;
  ser_ref_t *transfers;
  
  bool oom;
  bool views_as_host_objects;
} ser_t;

typedef struct {
  ant_t *js;
  
  ant_value_t self;
  ant_value_t refs;
  ant_value_t transfers;
  
  const uint8_t *data;
  size_t len;
  size_t pos;
  
  uint64_t version;
} des_t;

__attribute__((cold)) 
ant_value_t v8_library(ant_t *js);

ant_value_t des_value(des_t *d);
ant_value_t ser_value(ser_t *s, ant_value_t value);
