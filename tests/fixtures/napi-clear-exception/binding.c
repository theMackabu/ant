#include <stdbool.h>
#include <stddef.h>

typedef struct napi_env__ *napi_env;
typedef struct napi_value__ *napi_value;
typedef struct napi_callback_info__ *napi_callback_info;
typedef int napi_status;
typedef napi_value (*napi_callback)(napi_env, napi_callback_info);
extern napi_status napi_get_cb_info(napi_env, napi_callback_info, size_t *, napi_value *, napi_value *, void **);
extern napi_status napi_throw(napi_env, napi_value);
extern napi_status napi_throw_error(napi_env, const char *, const char *);
extern napi_status napi_get_and_clear_last_exception(napi_env, napi_value *);
extern napi_status napi_is_exception_pending(napi_env, bool *);
extern napi_status napi_create_function(napi_env, const char *, size_t, napi_callback, void *, napi_value *);
extern napi_status napi_set_named_property(napi_env, napi_value, const char *, napi_value);

static napi_value clear_exception(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value value = NULL, cleared = NULL;
  bool pending = true;
  if (napi_get_cb_info(env, info, &argc, &value, NULL, NULL) != 0 || argc != 1 ||
      napi_throw(env, value) != 0 || napi_get_and_clear_last_exception(env, &cleared) != 0 ||
      napi_is_exception_pending(env, &pending) != 0 || pending) {
    napi_throw_error(env, NULL, "exception was not cleared");
    return NULL;
  }
  return cleared;
}

__attribute__((visibility("default")))
napi_value napi_register_module_v1(napi_env env, napi_value exports) {
  napi_value fn = NULL;
  if (napi_create_function(env, "clear", 5, clear_exception, NULL, &fn) == 0)
    napi_set_named_property(env, exports, "clear", fn);
  return exports;
}
