#ifndef ANT_JIT_OPT_CODEGEN_H
#define ANT_JIT_OPT_CODEGEN_H

#include "../tier.h"

opt_code_t *opt_cg_generate(ant_t *js, const ir_graph_t *graph, const ir_frame_layout_t *layout, asm_code_heap_t *heap, bool verify, opt_text_t *dump, const char **failure);

#endif
