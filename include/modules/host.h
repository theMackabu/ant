#pragma once

#include <stdbool.h>

#include "types.h"
#include "gc/modules.h"

#define HOST_HOOK(ret, name, params, stub) ret name params;
#include "modules/host_hooks.h"
