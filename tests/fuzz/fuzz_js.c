// Silver parser and bytecode compiler (src/silver/).
//
// Input: a mode byte, then JavaScript source. The source is parsed and
// compiled the way js_eval_bytecode* does it, but never run.
// Properties:
//   - parsing and compiling any input either succeeds or leaves a pending
//     exception, without crashing;
//   - the operand depth analysis accepts every function the compiler emits
//     (the check tools/check_stack_depth.sh makes on the test suites).
#include "fuzz.h"
#include "internal.h"
#include "runtime.h"
#include "debug.h"
#include "gc.h"
#include "errors.h"
#include "silver/ast.h"
#include "silver/code_unit.h"
#include "silver/compiler.h"
#include "silver/engine.h"

static ant_t *js;

// One isolate serves every input. Under ASan this target runs with
// ASAN_OPTIONS=detect_leaks=0:detect_stack_use_after_return=0 (see
// meson/tests/fuzz/meson.build and maid fuzz): LeakSanitizer does not scan
// the GC heap, so storage owned by GC objects looks leaked, and the
// conservative stack scan must see locals on the real stack, not in ASan's
// fake frames.

static void check_func(const sv_func_t *func) {
  int depth = sv_func_compute_max_stack(func);
  if (depth < 0) {
    fprintf(stderr, "operand depth analysis rejected %s\n",
      func->debug && func->debug->name ? func->debug->name : "<anonymous>");
    FUZZ_CHECK(depth >= 0);
  }
  FUZZ_CHECK(func->max_stack == depth);
  for (int i = 0; i < func->child_func_count; i++) check_func(func->child_funcs[i]);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (!js) {
    // compiled code would make the units immortal (sv_code_unit_make_immortal)
    sv_debug_enable(SV_DEBUG_JITLESS);
    js = Ant_Create(nullptr);
    FUZZ_CHECK(js != NULL);
  }
  // the GC scans the C stack up to this frame, which callers reach from
  // different depths
  js_setstackbase(js, __builtin_frame_address(0));

  fuzz_input_t in = { data, size };
  uint8_t control = fuzz_u8(&in);
  static const sv_compile_mode_t modes[] = { SV_COMPILE_SCRIPT, SV_COMPILE_MODULE, SV_COMPILE_EVAL, SV_COMPILE_REPL };
  sv_compile_mode_t mode = modes[control & 3];
  bool strict = control & 4;
  sv_parse_goal_t goal =
    mode == SV_COMPILE_MODULE ? SV_PARSE_MODULE :
    mode == SV_COMPILE_REPL ? SV_PARSE_REPL : SV_PARSE_SCRIPT;

  // sources reach the parser NUL-terminated
  char *src = malloc(in.size + 1);
  if (!src) abort();
  if (in.size) memcpy(src, in.data, in.size);
  src[in.size] = '\0';

  code_arena_mark_t mark = parse_arena_mark(js);
  sv_ast_t *program = sv_parse(js, src, (ant_offset_t)in.size, goal, strict);
  if (program) {
    ant_value_t env = mode == SV_COMPILE_EVAL ? js->global : Ant_Value_Undefined;
    sv_func_t *func = js_compile_parsed_bytecode(js, program, src, in.size, mode, env);
    if (func) {
      check_func(func);
      sv_code_unit_unpin(func->unit);
    } else FUZZ_CHECK(Ant_Exception_Pending(js));
  } else FUZZ_CHECK(Ant_Exception_Pending(js));
  parse_arena_rewind(js, mark);
  Ant_Exception_Clear(js);
  free(src);

  static unsigned runs;
  if (++runs % 256 == 0) gc_run(js);
  return 0;
}
