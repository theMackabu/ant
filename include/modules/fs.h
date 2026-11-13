#pragma once

#include "types.h"

__attribute__((cold)) void init_fs_module(ant_t *js);
__attribute__((cold)) ant_value_t fs_library(ant_t *js);
__attribute__((cold)) ant_value_t fs_promises_library(ant_t *js);
__attribute__((cold)) ant_value_t fs_constants_library(ant_t *js);

int has_pending_fs_ops(ant_t *js);
void cleanup_fs_module(ant_t *js);
ant_value_t fs_make_constants(ant_t *js);
