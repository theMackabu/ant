#ifndef ANT_HANDLES_H
#define ANT_HANDLES_H

#include "types.h"
#include <stdint.h>

uint64_t ant_cfunc_handle_intern(const ant_cfunc_meta_t *meta);
uint64_t ant_cfunc_handle_create(const ant_cfunc_meta_t *meta);
uint64_t ant_cfunc_handle_for_entrypoint(ant_cfunc_t fn);
const ant_cfunc_meta_t *ant_cfunc_handle_get(uint64_t handle);

uint64_t ant_external_handle_intern(void *ptr);
void *ant_external_handle_get(uint64_t handle);

#endif
