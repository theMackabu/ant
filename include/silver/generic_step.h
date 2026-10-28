#ifndef SILVER_GENERIC_STEP_H
#define SILVER_GENERIC_STEP_H

#include "silver/engine.h"

bool sv_generic_step_supports(sv_op_t op);
ant_value_t sv_generic_step(sv_vm_t *vm, sv_frame_t *frame, uint8_t *ip, uint32_t depth);

#endif
