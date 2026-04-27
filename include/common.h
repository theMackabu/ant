#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ANT_INTERNAL_SLOT_LIST(X) \
  X(SLOT_NONE)                    \
  X(SLOT_ASYNC)                   \
  X(SLOT_CODE)                    \
  X(SLOT_CODE_LEN)                \
  X(SLOT_CFUNC)                   \
  X(SLOT_CORO)                    \
  X(SLOT_PROTO)                   \
  X(SLOT_FUNC_PROTO)              \
  X(SLOT_ASYNC_PROTO)             \
  X(SLOT_GENERATOR_PROTO)         \
  X(SLOT_ASYNC_GENERATOR_PROTO)   \
  X(SLOT_AUX)                     \
  X(SLOT_TARGET_FUNC)             \
  X(SLOT_MODULE_CTX)              \
  X(SLOT_MODULE_LOADING)          \
  X(SLOT_MAP)                     \
  X(SLOT_SET)                     \
  X(SLOT_PRIMITIVE)               \
  X(SLOT_PROXY_REF)               \
  X(SLOT_BUILTIN)                 \
  X(SLOT_BRAND)                   \
  X(SLOT_DATA)                    \
  X(SLOT_EVENT_MAX_LISTENERS)     \
  X(SLOT_CTOR)                    \
  X(SLOT_FS_FLAGS)                \
  X(SLOT_DEFAULT)                 \
  X(SLOT_CONSOLE_STDOUT)          \
  X(SLOT_CONSOLE_STDERR)          \
  X(SLOT_CONSOLE_COUNTS)          \
  X(SLOT_CONSOLE_TIMERS)          \
  X(SLOT_CONSOLE_GROUP_INDENT)    \
  X(SLOT_CONSOLE_GROUP_LEVEL)     \
  X(SLOT_ERROR_BRAND)             \
  X(SLOT_ERR_TYPE)                \
  X(SLOT_OBSERVABLE_SUBSCRIBER)   \
  X(SLOT_SUBSCRIPTION_OBSERVER)   \
  X(SLOT_SUBSCRIPTION_CLEANUP)    \
  X(SLOT_STRICT_ARGS)             \
  X(SLOT_ITER_STATE)              \
  X(SLOT_ENTRIES)                 \
  X(SLOT_SETTLED)                 \
  X(SLOT_WT_ON_MESSAGE)           \
  X(SLOT_WT_ONCE_MESSAGE)         \
  X(SLOT_WT_ON_EXIT)              \
  X(SLOT_WT_ONCE_EXIT)            \
  X(SLOT_WT_PORT_TAG)             \
  X(SLOT_WT_PORT_QUEUE)           \
  X(SLOT_WT_PORT_HEAD)            \
  X(SLOT_WT_PORT_PEER)            \
  X(SLOT_WT_PORT_CLOSED)          \
  X(SLOT_WT_PORT_STARTED)         \
  X(SLOT_WT_PORT_ON_MESSAGE)      \
  X(SLOT_WT_PORT_ONCE_MESSAGE)    \
  X(SLOT_WT_PORT_PROTO)           \
  X(SLOT_WT_ENV_STORE)            \
  X(SLOT_NAPI_EXTERNAL_ID)        \
  X(SLOT_NAPI_WRAP_ID)            \
  X(SLOT_RS_PULL)                 \
  X(SLOT_RS_CANCEL)               \
  X(SLOT_RS_SIZE)                 \
  X(SLOT_RS_CLOSED)               \
  X(SLOT_WS_WRITE)                \
  X(SLOT_WS_CLOSE)                \
  X(SLOT_WS_ABORT)                \
  X(SLOT_WS_READY)                \
  X(SLOT_WS_SIGNAL)               \
  X(SLOT_HEADERS_GUARD)           \
  X(SLOT_REQUEST_HEADERS)         \
  X(SLOT_REQUEST_SIGNAL)          \
  X(SLOT_REQUEST_ABORT_REASON)    \
  X(SLOT_REQUEST_BODY_STREAM)     \
  X(SLOT_RESPONSE_HEADERS)        \
  X(SLOT_RESPONSE_BODY_STREAM)    \
  X(SLOT_PIPE_ABORT_LISTENER)     \
  X(SLOT_REGEXP_FLAGS_MASK)       \
  X(SLOT_REGEXP_FLAGS_STRING)     \
  X(SLOT_REGEXP_NAMED_GROUPS)     \
  X(SLOT_REGEXP_RESULT_GROUPS)    \
  X(SLOT_REGEXP_GROUPS_CACHE)     \
  X(SLOT_MATCHALL_RX)             \
  X(SLOT_MATCHALL_STR)            \
  X(SLOT_MATCHALL_DONE)

#define ANT_DECLARE_INTERNAL_SLOT(name) name,
typedef enum {
  ANT_INTERNAL_SLOT_LIST(ANT_DECLARE_INTERNAL_SLOT)
  SLOT_MAX = 255
} internal_slot_t;
#undef ANT_DECLARE_INTERNAL_SLOT

typedef enum {
  BUILTIN_NONE = 0,
  BUILTIN_OBJECT = 1
} builtin_fn_id_t;

typedef enum {
  BRAND_NONE = 0,
  BRAND_BLOB,
  BRAND_FILE,
  BRAND_HEADERS,
  BRAND_FORMDATA,
  BRAND_URLSEARCHPARAMS,
  BRAND_DATAVIEW,
  BRAND_REQUEST,
  BRAND_RESPONSE,
  BRAND_READABLE_STREAM,
  BRAND_READABLE_STREAM_READER,
  BRAND_READABLE_STREAM_CONTROLLER,
  BRAND_WRITABLE_STREAM,
  BRAND_WRITABLE_STREAM_WRITER,
  BRAND_WRITABLE_STREAM_CONTROLLER,
  BRAND_TRANSFORM_STREAM,
  BRAND_TRANSFORM_STREAM_CONTROLLER,
  BRAND_WASM_MODULE,
  BRAND_WASM_INSTANCE,
  BRAND_WASM_GLOBAL,
  BRAND_WASM_MEMORY,
  BRAND_WASM_TABLE,
  BRAND_WASM_TAG,
  BRAND_WASM_EXCEPTION,
  BRAND_DATE,
  BRAND_MODULE_NAMESPACE,
  BRAND_ABORT_SIGNAL,
  BRAND_EVENTEMITTER,
  BRAND_EVENTTARGET,
  BRAND_DISPOSABLE_STACK,
  BRAND_ASYNC_DISPOSABLE_STACK
} object_brand_id_t;

static inline void *mantissa_chk(void *p, const char *func) {
  if (!p || ((uintptr_t)p >> 47) == 0) goto ok;

  fprintf(
    stderr,
    "FATAL: %s returned pointer %p outside 47-bit NaN-boxing range\n"
    "Please report this issue with your OS/architecture details.\n", func, p
  );
  
  abort();
  ok: return p;
}

#define ant_calloc(size)       mantissa_chk(calloc(1, size),    "calloc")
#define ant_realloc(ptr, size) mantissa_chk(realloc(ptr, size), "realloc")

#endif
