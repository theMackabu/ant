#ifndef ANT_HANDLES_H
#define ANT_HANDLES_H

#include "types.h"
#include <stdint.h>

const ant_cfunc_meta_t *ant_cfunc_meta_intern(const ant_cfunc_meta_t *meta);
const ant_cfunc_meta_t *ant_cfunc_meta_create(const ant_cfunc_meta_t *meta);
const ant_cfunc_meta_t *ant_cfunc_meta_for_entrypoint(ant_cfunc_t fn);

uint64_t ant_external_handle_intern(void *ptr);
void *ant_external_handle_get(uint64_t handle);

#endif
