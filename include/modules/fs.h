#pragma once

#include "types.h"

ant_value_t fs_library(ant_t *js);
ant_value_t fs_promises_library(ant_t *js);
ant_value_t fs_constants_library(ant_t *js);
ant_value_t fs_make_constants(ant_t *js);

int has_pending_fs_ops(ant_t *js);
void init_fs_module(ant_t *js);
void cleanup_fs_module(ant_t *js);
