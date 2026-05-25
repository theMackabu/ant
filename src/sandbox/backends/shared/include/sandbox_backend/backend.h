#pragma once

#if defined(__APPLE__)
#include "darwin/backend.h"
#elif defined(__linux__)
#include "linux/backend.h"
#else
#error "unsupported sandbox backend platform"
#endif
