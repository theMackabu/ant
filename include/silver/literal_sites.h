#pragma once

#include "silver/engine.h"

ant_value_t Silver_Literal_Create(ant_t *js, sv_func_t *func, uint32_t index);
void Silver_Literal_InitElement(ant_t *js, ant_value_t arr, uint32_t index, ant_value_t value);
