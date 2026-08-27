#include <stddef.h>

typedef struct napi_env__ *napi_env;
typedef struct napi_value__ *napi_value;
typedef int napi_status;

extern napi_status napi_create_string_utf8(
  napi_env env, const char *str, size_t length, napi_value *result
);
extern napi_status napi_set_named_property(
  napi_env env, napi_value object, const char *name, napi_value value
);

#ifdef _WIN32
#define NAPI_EXPORT __declspec(dllexport)
#else
#define NAPI_EXPORT __attribute__((visibility("default")))
#endif

NAPI_EXPORT napi_value napi_register_module_v1(napi_env env, napi_value exports) {
  napi_value value = NULL;
  if (napi_create_string_utf8(env, "native-ok", 9, &value) == 0)
    (void)napi_set_named_property(env, exports, "value", value);
  return exports;
}

