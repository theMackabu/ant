#ifndef FORMDATA_H
#define FORMDATA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "types.h"

typedef struct fd_entry {
  char *name;
  bool is_file;
  char *str_value;
  size_t val_idx;
  struct fd_entry *next;
} fd_entry_t;

typedef struct {
  fd_entry_t *head;
  fd_entry_t **tail;
  size_t count;
} fd_data_t;

void init_formdata_module(void);
bool formdata_is_empty(ant_value_t fd);
bool formdata_is_formdata(ant_t *js, ant_value_t obj);

ant_value_t formdata_create_empty(ant_t *js);
ant_value_t formdata_append_string(ant_t *js, ant_value_t fd, ant_value_t name_v, ant_value_t value_v);
ant_value_t formdata_append_file(ant_t *js, ant_value_t fd, ant_value_t name_v, ant_value_t blob_v, ant_value_t filename_v);

#endif
