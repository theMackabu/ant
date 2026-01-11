#ifndef JS_CONFIG_H
#define JS_CONFIG_H

#ifndef ANT_VERSION
#define ANT_VERSION "unknown"
#endif

#ifndef ANT_GIT_HASH
#define ANT_GIT_HASH "unknown"
#endif

#ifndef ANT_BUILD_TIMESTAMP
#define ANT_BUILD_TIMESTAMP 0
#endif

#ifndef ANT_TARGET_TRIPLE
#define ANT_TARGET_TRIPLE "unknown"
#endif

#define SLOTMASK ~(((jsoff_t) ~0) >> 1)
#define CONSTMASK (~(((jsoff_t) ~0) >> 1) >> 1)
#define ARRMASK (~(((jsoff_t) ~0) >> 1) >> 2)
#define NONCONFIGMASK (~(((jsoff_t) ~0) >> 1) >> 3)
#define FLAGMASK (SLOTMASK | CONSTMASK | ARRMASK | NONCONFIGMASK)

typedef enum {
  SLOT_NONE = 0,
  SLOT_PID,
  SLOT_ASYNC,
  SLOT_WITH,
  SLOT_SCOPE,
  SLOT_THIS,
  SLOT_BOUND_THIS,
  SLOT_BOUND_ARGS,
  SLOT_FIELD_COUNT,
  SLOT_SOURCE,
  SLOT_FIELDS,
  SLOT_STRICT,
  SLOT_CODE,
  SLOT_CFUNC,
  SLOT_CORO,
  SLOT_PROTO,
  SLOT_FROZEN,
  SLOT_SEALED,
  SLOT_EXTENSIBLE,
  SLOT_BUFFER,
  SLOT_TARGET_FUNC,
  SLOT_VERSION,
  SLOT_NAME,
  SLOT_MAP,
  SLOT_SET,
  SLOT_PRIMITIVE,
  SLOT_PROXY_REF,
  SLOT_BUILTIN,
  SLOT_DATA,
  SLOT_MAX = 255
} internal_slot_t;

typedef enum {
  BUILTIN_NONE = 0,
  BUILTIN_OBJECT = 1
} builtin_fn_id_t;

#endif
