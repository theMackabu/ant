#ifndef JS_CONFIG_H
#define JS_CONFIG_H

#ifndef ANT_VERSION
#define ANT_VERSION "unknown"
#endif

#ifndef ANT_GIT_HASH
#define ANT_GIT_HASH "unknown"
#endif

#ifndef ANT_BUILD_DATE
#define ANT_BUILD_DATE "unknown"
#endif

#define JS_EXPR_MAX 20
#define JS_GC_THRESHOLD 0.75

#define SLOTMASK ~(((jsoff_t) ~0) >> 1)
#define CONSTMASK (~(((jsoff_t) ~0) >> 1) >> 1)
#define ARRMASK (~(((jsoff_t) ~0) >> 1) >> 2)

typedef enum {
  SLOT_NONE = 0,
  SLOT_PID,
  SLOT_MAX = 255
} internal_slot_t;

#endif
