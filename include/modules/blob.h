#ifndef BLOB_H
#define BLOB_H

#include <stdint.h>
#include <stddef.h>
#include "types.h"

typedef struct {
  uint8_t *data;
  size_t size;
  char *type;
  char *name;
  int64_t last_modified;
} blob_data_t;

void init_blob_module(void);

blob_data_t *blob_get_data(ant_value_t obj);
ant_value_t  blob_create(ant_t *js, const uint8_t *data, size_t size, const char *type);

extern ant_value_t g_blob_proto;
extern ant_value_t g_file_proto;

#endif
