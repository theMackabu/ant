#include <stdlib.h>
#include "gc/objects.h"
#include "ops/eval_env.h"
#include "silver/eval_env.h"

void sv_eval_env_gc_mark(ant_t *js, ant_object_t *obj) {
  if (!obj) return;
  
  sv_eval_env_state_t *state = sv_eval_env_state(js_obj_from_ptr(obj));
  if (!state) return;
  
  gc_mark_value(js, state->arguments_obj);
  gc_mark_upvalue_cells(js, state->cells, state->cell_count);
}

void sv_eval_env_gc_free(ant_object_t *obj) {
  ant_object_sidecar_t *sidecar = ant_object_sidecar(obj);
  if (!sidecar || !sidecar->eval_env_state) return;
  
  free(sidecar->eval_env_state);
  sidecar->eval_env_state = NULL;
}
