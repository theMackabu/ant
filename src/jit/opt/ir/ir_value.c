#include "ir_value.h"
#include <stdio.h>

void ir_value_describe(uint64_t bits, char *buffer, size_t size) {
  const char *name = NULL;
  switch (bits) {
    case IR_VALUE_EMPTY: name = "empty"; break;
    case IR_VALUE_VIRTUAL_UNMAPPED_ARGUMENTS: name = "arguments"; break;
    case IR_VALUE_VIRTUAL_MAPPED_ARGUMENTS: name = "mapped arguments"; break;
    case IR_VALUE_UNDEFINED: name = "undefined"; break;
    case IR_VALUE_NULL: name = "null"; break;
    case IR_VALUE_FALSE: name = "false"; break;
    case IR_VALUE_TRUE: name = "true"; break;
    default: break;
  }
  if (name) {
    snprintf(buffer, size, "%s", name);
    return;
  }
  int32_t integer;
  if (ir_value_as_int32(bits, &integer)) snprintf(buffer, size, "%d", integer);
  else snprintf(buffer, size, "0x%016llx", (unsigned long long)bits);
}
