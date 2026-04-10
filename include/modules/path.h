#ifndef ANT_PATH_MODULE_H
#define ANT_PATH_MODULE_H

#include "types.h"

ant_value_t path_library(ant_t *js);
ant_value_t path_posix_library(ant_t *js);
ant_value_t path_win32_library(ant_t *js);

#endif
