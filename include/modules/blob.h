#pragma once

#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include "types.h"

typedef struct blob_storage {
  _Atomic int refs;
  size_t size;
  uint8_t bytes[];
} blob_storage_t;

typedef struct {
  uint8_t *data;
  size_t size;
  char *type;
  char *name;
  int64_t last_modified;
  blob_storage_t *storage;
} blob_data_t;

void init_blob_module(ant_t *js);
bool blob_is_blob(ant_t *js, ant_value_t obj);

blob_data_t *blob_get_data(ant_value_t obj);
ant_value_t blob_create(ant_t *js, const uint8_t *data, size_t size, const char *type);
ant_value_t blob_create_shared(ant_t *js, blob_storage_t *storage, const char *type);

blob_storage_t *blob_storage_retain(blob_storage_t *storage);
void blob_storage_release(blob_storage_t *storage);
