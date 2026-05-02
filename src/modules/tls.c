#include <compat.h> // IWYU pragma: keep

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <tlsuv/tlsuv.h>
#include <tlsuv/tls_engine.h>

#include "ant.h"
#include "ptr.h"
#include "errors.h"

#include "modules/tls.h"
#include "modules/buffer.h"

typedef struct ant_tls_context_wrap_s {
  ant_value_t obj;
  tls_context *ctx;
  tlsuv_private_key_t key;
  tlsuv_certificate_t cert;
  bool closed;
} ant_tls_context_wrap_t;

enum { TLS_CONTEXT_NATIVE_TAG = 0x544c5343u }; // TLSC
static ant_value_t g_tls_context_proto = 0;

static void tls_context_free(ant_tls_context_wrap_t *wrap) {
  if (!wrap || wrap->closed) return;
  wrap->closed = true;

  if (wrap->cert && wrap->cert->free) wrap->cert->free(wrap->cert);
  if (wrap->key && wrap->key->free) wrap->key->free(wrap->key);
  if (wrap->ctx && wrap->ctx->free_ctx) wrap->ctx->free_ctx(wrap->ctx);

  wrap->cert = NULL;
  wrap->key = NULL;
  wrap->ctx = NULL;
}

static ant_tls_context_wrap_t *tls_context_data(ant_value_t value) {
  return (ant_tls_context_wrap_t *)js_get_native(value, TLS_CONTEXT_NATIVE_TAG);
}

static bool tls_value_bytes(
  ant_t *js,
  ant_value_t value,
  const char **bytes_out,
  size_t *len_out,
  ant_value_t *error_out
) {
  ant_value_t str_value = 0;
  const uint8_t *buffer_bytes = NULL;

  if (error_out) *error_out = js_mkundef();
  if (bytes_out) *bytes_out = NULL;
  if (len_out) *len_out = 0;

  if (vtype(value) == T_UNDEF || vtype(value) == T_NULL) return true;
  if (buffer_source_get_bytes(js, value, &buffer_bytes, len_out)) {
    if (bytes_out) *bytes_out = (const char *)buffer_bytes;
    return true;
  }

  str_value = js_tostring_val(js, value);
  if (is_err(str_value)) {
    if (error_out) *error_out = str_value;
    return false;
  }

  if (!bytes_out || !len_out) return true;
  *bytes_out = js_getstr(js, str_value, len_out);
  
  if (!*bytes_out) {
    if (error_out) *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TLS input");
    return false;
  }

  return true;
}

static ant_value_t tls_make_error(ant_t *js, tls_context *ctx, long code, const char *fallback) {
  const char *message = fallback;
  if (ctx && ctx->strerror) {
    const char *detail = ctx->strerror(code);
    if (detail && *detail) message = detail;
  }
  return js_mkerr_typed(js, JS_ERR_TYPE, "%s", message ? message : "TLS error");
}

static ant_value_t js_tls_context_close(ant_t *js, ant_value_t *args, int nargs) {
  ant_tls_context_wrap_t *wrap = tls_context_data(js_getthis(js));
  if (!wrap) return js_getthis(js);
  tls_context_free(wrap);
  js_clear_native(wrap->obj, TLS_CONTEXT_NATIVE_TAG);
  return js_getthis(js);
}

static void tls_init_context_proto(ant_t *js) {
  if (g_tls_context_proto) return;

  g_tls_context_proto = js_mkobj(js);
  js_set(js, g_tls_context_proto, "close", js_mkfun(js_tls_context_close));
}

static ant_value_t js_tls_create_context(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t options = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t obj = 0;
  ant_value_t error = js_mkundef();
  
  ant_tls_context_wrap_t *wrap = NULL;
  ant_value_t ca_v = js_mkundef();
  ant_value_t key_v = js_mkundef();
  ant_value_t cert_v = js_mkundef();
  ant_value_t partial_v = js_mkundef();
  
  const char *ca = NULL;
  const char *key_data = NULL;
  const char *cert_data = NULL;
  
  size_t ca_len = 0;
  size_t key_len = 0;
  size_t cert_len = 0;
  int rc = 0;

  if (vtype(options) != T_UNDEF && vtype(options) != T_NULL && vtype(options) != T_OBJ)
    return js_mkerr_typed(js, JS_ERR_TYPE, "TLS context options must be an object");

  tls_init_context_proto(js);

  if (vtype(options) == T_OBJ) {
    ca_v = js_get(js, options, "ca");
    key_v = js_get(js, options, "key");
    cert_v = js_get(js, options, "cert");
    partial_v = js_get(js, options, "allowPartialChain");
  }

  if (!tls_value_bytes(js, ca_v, &ca, &ca_len, &error)) return error;
  if (!tls_value_bytes(js, key_v, &key_data, &key_len, &error)) return error;
  if (!tls_value_bytes(js, cert_v, &cert_data, &cert_len, &error)) return error;

  wrap = calloc(1, sizeof(*wrap));
  if (!wrap) return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");

  wrap->ctx = default_tls_context(ca, ca_len);
  if (!wrap->ctx) {
    free(wrap);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to create TLS context");
  }

  if (wrap->ctx->allow_partial_chain && js_truthy(js, partial_v)) {
  rc = wrap->ctx->allow_partial_chain(wrap->ctx, 1);
  if (rc != 0) {
    ant_value_t err = tls_make_error(js, wrap->ctx, rc, "Failed to enable partial chain verification");
    tls_context_free(wrap);
    free(wrap);
    return err;
  }}

  if (key_data) {
    if (!wrap->ctx->load_key) {
      tls_context_free(wrap);
      free(wrap);
      return js_mkerr_typed(js, JS_ERR_TYPE, "TLS engine does not support loading private keys");
    }
    
    rc = wrap->ctx->load_key(&wrap->key, key_data, key_len);
    if (rc != 0) {
      ant_value_t err = tls_make_error(js, wrap->ctx, rc, "Failed to load TLS private key");
      tls_context_free(wrap);
      free(wrap);
      return err;
    }
    
    if (cert_data) {
    if (!wrap->ctx->load_cert) {
      tls_context_free(wrap);
      free(wrap);
      return js_mkerr_typed(js, JS_ERR_TYPE, "TLS engine does not support loading certificates");
    }
    
    rc = wrap->ctx->load_cert(&wrap->cert, cert_data, cert_len);
    if (rc != 0) {
      ant_value_t err = tls_make_error(js, wrap->ctx, rc, "Failed to load TLS certificate");
      tls_context_free(wrap);
      free(wrap);
      return err;
    }}

    if (!wrap->ctx->set_own_cert) {
      tls_context_free(wrap);
      free(wrap);
      return js_mkerr_typed(js, JS_ERR_TYPE, "TLS engine does not support own certificate configuration");
    }

    rc = wrap->ctx->set_own_cert(wrap->ctx, wrap->key, wrap->cert);
    if (rc != 0) {
      ant_value_t err = tls_make_error(js, wrap->ctx, rc, "Failed to configure TLS certificate");
      tls_context_free(wrap);
      free(wrap);
      return err;
    }
  } else if (cert_data) {
    tls_context_free(wrap);
    free(wrap);
    return js_mkerr_typed(js, JS_ERR_TYPE, "TLS certificate requires a private key");
  }

  obj = js_mkobj(js);
  js_set_proto_init(obj, g_tls_context_proto);
  wrap->obj = obj;
  
  js_set_native(obj, wrap, TLS_CONTEXT_NATIVE_TAG);
  
  return obj;
}

static ant_value_t js_tls_is_context(ant_t *js, ant_value_t *args, int nargs) {
  ant_tls_context_wrap_t *wrap = nargs > 0 ? tls_context_data(args[0]) : NULL;
  return js_bool(wrap && !wrap->closed && wrap->ctx);
}

static ant_value_t js_tls_set_config_path(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t str_value = js_mkundef();
  const char *path = NULL;
  int rc = 0;

  if (nargs < 1 || vtype(args[0]) == T_UNDEF || vtype(args[0]) == T_NULL) {
    rc = tlsuv_set_config_path(NULL);
    if (rc != 0) return js_mkerr_typed(js, JS_ERR_TYPE, "%s", uv_strerror(rc));
    return js_mkundef();
  }

  str_value = js_tostring_val(js, args[0]);
  if (is_err(str_value)) return str_value;
  
  path = js_getstr(js, str_value, NULL);
  if (!path) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TLS config path");

  rc = tlsuv_set_config_path(path);
  if (rc != 0) return js_mkerr_typed(js, JS_ERR_TYPE, "%s", uv_strerror(rc));
  
  return js_mkundef();
}

ant_value_t internal_tls_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);
  ant_value_t version = js_mkstr(js, "unknown", 7);
  
  tls_context *ctx = default_tls_context(NULL, 0);
  const char *version_str = NULL;

  if (ctx && ctx->version) {
    version_str = ctx->version();
    if (version_str) version = js_mkstr(js, version_str, strlen(version_str));
  }
  
  if (ctx && ctx->free_ctx) ctx->free_ctx(ctx);
  tls_init_context_proto(js);
  
  js_set(js, lib, "version", version);
  js_set(js, lib, "createContext", js_mkfun(js_tls_create_context));
  js_set(js, lib, "isContext", js_mkfun(js_tls_is_context));
  js_set(js, lib, "setConfigPath", js_mkfun(js_tls_set_config_path));
  
  return lib;
}
