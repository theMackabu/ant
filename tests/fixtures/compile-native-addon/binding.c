#include <stddef.h>

typedef struct napi_env__ *napi_env;
typedef struct napi_value__ *napi_value;
typedef struct napi_handle_scope__ *napi_handle_scope;
typedef struct napi_escapable_handle_scope__ *napi_escapable_handle_scope;
typedef int napi_status;

extern napi_status napi_create_string_utf8(
  napi_env env, const char *str, size_t length, napi_value *result
);
extern napi_status napi_set_named_property(
  napi_env env, napi_value object, const char *name, napi_value value
);
extern napi_status napi_open_handle_scope(napi_env env, napi_handle_scope *result);
extern napi_status napi_close_handle_scope(napi_env env, napi_handle_scope scope);
extern napi_status napi_open_escapable_handle_scope(
  napi_env env, napi_escapable_handle_scope *result
);
extern napi_status napi_close_escapable_handle_scope(
  napi_env env, napi_escapable_handle_scope scope
);
extern napi_status napi_escape_handle(
  napi_env env,
  napi_escapable_handle_scope scope,
  napi_value escapee,
  napi_value *result
);

#ifdef _WIN32
#define NAPI_EXPORT __declspec(dllexport)
#else
#define NAPI_EXPORT __attribute__((visibility("default")))
#endif

#ifndef NATIVE_VALUE
#define NATIVE_VALUE "native-ok"
#endif

NAPI_EXPORT napi_value napi_register_module_v1(napi_env env, napi_value exports) {
  napi_value value = NULL;
  if (napi_create_string_utf8(env, NATIVE_VALUE, sizeof(NATIVE_VALUE) - 1, &value) == 0)
    (void)napi_set_named_property(env, exports, "value", value);

  napi_handle_scope scope = NULL;
  if (napi_open_handle_scope(env, &scope) == 0) {
    napi_value scoped = NULL;
    if (napi_create_string_utf8(env, "scope-ok", 8, &scoped) == 0)
      (void)napi_set_named_property(env, exports, "scoped", scoped);
    (void)napi_close_handle_scope(env, scope);
  }

  napi_escapable_handle_scope escapable = NULL;
  if (napi_open_escapable_handle_scope(env, &escapable) == 0) {
    napi_value local = NULL;
    napi_value escaped = NULL;
    int escaped_ok = napi_create_string_utf8(env, "escape-ok", 9, &local) == 0
      && napi_escape_handle(env, escapable, local, &escaped) == 0;
    if (napi_close_escapable_handle_scope(env, escapable) == 0 && escaped_ok)
      (void)napi_set_named_property(env, exports, "escaped", escaped);
  }

  return exports;
}
