#include "code.h"

const char *opt_exit_kind_name(opt_exit_kind_t kind) {
  switch (kind) {
    case OPT_EXIT_NO_FEEDBACK: return "NoFeedback";
    case OPT_EXIT_NOT_OBJECT: return "NotObject";
    case OPT_EXIT_BAD_SHAPE: return "BadShape";
    case OPT_EXIT_BAD_CALL_TARGET: return "BadCallTarget";
    case OPT_EXIT_UNEXPECTED_VALUE: return "UnexpectedValue";
    case OPT_EXIT_SLOW_PATH: return "SlowPath";
    case OPT_EXIT_ARGUMENTS_INDEX: return "ArgumentsIndex";
    case OPT_EXIT_NOT_INT32: return "NotInt32";
    case OPT_EXIT_OVERFLOW: return "Overflow";
    case OPT_EXIT_BAD_ELEMENTS: return "BadElements";
    case OPT_EXIT_OUT_OF_BOUNDS: return "OutOfBounds";
    case OPT_EXIT_BAD_TYPE: return "BadType";
    case OPT_EXIT_UNWIND: return "Unwind";
    case OPT_EXIT_HOISTED: return "Hoisted";
    case OPT_EXIT_INVALIDATED: return "Invalidated";
  }
  return "?";
}

const char *opt_repr_name(opt_repr_t repr) {
  switch (repr) {
    case OPT_REPR_NONE: return "None";
    case OPT_REPR_TAGGED: return "Tagged";
    case OPT_REPR_INT32: return "Int32";
    case OPT_REPR_FLOAT64: return "Float64";
    case OPT_REPR_BOOL: return "Bool";
    case OPT_REPR_POINTER: return "Pointer";
  }
  return "?";
}
