#ifndef ANT_JIT_OPT_CODE_H
#define ANT_JIT_OPT_CODE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum : uint8_t {
  OPT_REPR_NONE = 0,
  OPT_REPR_TAGGED,
  OPT_REPR_INT32,
  OPT_REPR_FLOAT64,
  OPT_REPR_BOOL,
  OPT_REPR_POINTER,
} opt_repr_t;

typedef enum : uint8_t {
  OPT_EXIT_NO_FEEDBACK,
  OPT_EXIT_NOT_OBJECT,
  OPT_EXIT_BAD_SHAPE,
  OPT_EXIT_BAD_CALL_TARGET,
  OPT_EXIT_UNEXPECTED_VALUE,
  OPT_EXIT_SLOW_PATH,
  OPT_EXIT_ARGUMENTS_INDEX,
  OPT_EXIT_NOT_INT32,
  OPT_EXIT_OVERFLOW,
  OPT_EXIT_BAD_ELEMENTS,
  OPT_EXIT_OUT_OF_BOUNDS,
  OPT_EXIT_BAD_TYPE,
  OPT_EXIT_UNWIND,
  OPT_EXIT_HOISTED,
  OPT_EXIT_INVALIDATED,
} opt_exit_kind_t;

typedef enum : uint8_t {
  OPT_DEPENDENCY_PROTOTYPE_WATCH,
} opt_dependency_kind_t;

typedef struct {
  opt_dependency_kind_t kind;
  uint64_t a;
} opt_dependency_t;

static inline bool opt_dependency_equal(opt_dependency_t x, opt_dependency_t y) {
  return x.kind == y.kind && x.a == y.a;
}

typedef enum : uint8_t {
  OPT_RESUME_AT,
  OPT_RESUME_AFTER,
  OPT_RESUME_RETURN,
} opt_resume_mode_t;

static constexpr uint32_t OPT_COMPLETION_SLOT = 0x80000000u;
static constexpr uint32_t OPT_PENDING_RETURN = 1;
static constexpr uint32_t OPT_PENDING_JUMP = 2;

static constexpr uint32_t OPT_CALLEE_SLOT = UINT32_MAX - 1;
static constexpr uint32_t OPT_INVALID_SLOT = UINT32_MAX;

const char *opt_exit_kind_name(opt_exit_kind_t kind);
const char *opt_repr_name(opt_repr_t repr);

#endif
