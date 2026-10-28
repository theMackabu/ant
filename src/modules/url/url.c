#include <compat.h> // IWYU pragma: keep

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ant.h"
#include "cfunc.h"
#include "ptr.h"
#include "errors.h"
#include "internal.h"
#include "descriptors.h"

#include "modules/blob.h"
#include "modules/crypto.h"
#include "modules/url.h"
#include "url_internal.h"

static inline ant_value_t url_illegal_invocation(ant_t *js) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
}

static ant_value_t url_get_href(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return url_illegal_invocation(js);
  
  char *href = build_href(s);
  if (!href) return js_mkstr(js, "", 0);
  
  ant_value_t ret = js_mkstr(js, href, strlen(href));
  free(href);
  
  return ret;
}

static ant_value_t url_get_protocol(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return url_illegal_invocation(js);
  return js_mkstr(js, s->protocol, strlen(s->protocol));
}

static ant_value_t url_get_username(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return url_illegal_invocation(js);
  return js_mkstr(js, s->username, strlen(s->username));
}

static ant_value_t url_get_password(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return url_illegal_invocation(js);
  return js_mkstr(js, s->password, strlen(s->password));
}

static ant_value_t url_get_host(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return url_illegal_invocation(js);
  if (s->port && *s->port) {
    size_t len = strlen(s->hostname) + strlen(s->port) + 2;
    char *host = malloc(len);
    snprintf(host, len, "%s:%s", s->hostname, s->port);
    ant_value_t ret = js_mkstr(js, host, strlen(host));
    free(host);
    return ret;
  }
  return js_mkstr(js, s->hostname, strlen(s->hostname));
}

static ant_value_t url_get_hostname(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return url_illegal_invocation(js);
  return js_mkstr(js, s->hostname, strlen(s->hostname));
}

static ant_value_t url_get_port(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return url_illegal_invocation(js);
  return js_mkstr(js, s->port, strlen(s->port));
}

static ant_value_t url_get_pathname(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return url_illegal_invocation(js);
  return js_mkstr(js, s->pathname, strlen(s->pathname));
}

static ant_value_t url_get_search(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return url_illegal_invocation(js);
  if (s->search[0] == '?' && s->search[1] == '\0') return js_mkstr(js, "", 0);
  return js_mkstr(js, s->search, strlen(s->search));
}

static ant_value_t url_get_hash(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return url_illegal_invocation(js);
  if (s->hash[0] == '#' && s->hash[1] == '\0') return js_mkstr(js, "", 0);
  return js_mkstr(js, s->hash, strlen(s->hash));
}

static ant_value_t url_get_origin(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return url_illegal_invocation(js);

  char *href = build_href(s);
  if (!href) return js_mkstr(js, "null", 4);
  ada_url url = ada_parse(href, strlen(href));
  free(href);
  if (!url || !ada_is_valid(url)) {
    if (url) ada_free(url);
    return js_mkstr(js, "null", 4);
  }

  ada_owned_string origin = ada_get_origin(url);
  ant_value_t result = origin.data
    ? js_mkstr(js, origin.data, origin.length)
    : js_mkstr(js, "null", 4);
  ada_free_owned_string(origin);
  ada_free(url);
  return result;
}

static ant_value_t url_get_searchParams(ant_params_t) {
  if (!url_get_state(js->this_val)) return url_illegal_invocation(js);
  ant_value_t usp = js_get_slot(js->this_val, SLOT_ENTRIES);
  if (vtype(usp) == kTypeObject) return usp;
  return js_mkundef();
}

static ant_value_t url_set_href(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return url_illegal_invocation(js);

  ant_value_t value = url_string_value(js, nargs ? args[0] : js_mkundef());
  if (is_err(value)) return value;
  size_t value_len = 0;
  const char *val = js_getstr(js, value, &value_len);
  if (!val) return url_invalid_url_error(js, value, js_mkundef());
  
  url_state_t tmp;
  if (parse_url_to_state_n(val, value_len, NULL, 0, &tmp) != 0)
    return url_invalid_url_error(js, value, js_mkundef());
    
  url_state_clear(s);
  *s = tmp;
  
  const char *q = (s->search[0] == '?') ? s->search + 1 : "";
  url_sync_usp(js, js->this_val, q);
  
  return js_mkundef();
}

static ant_value_t url_set_protocol(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return url_illegal_invocation(js);
  return url_apply_setter(js, s, nargs ? args[0] : js_mkundef(), ada_set_protocol);
}

static ant_value_t url_set_username(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return url_illegal_invocation(js);
  return url_apply_setter(js, s, nargs ? args[0] : js_mkundef(), ada_set_username);
}

static ant_value_t url_set_password(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return url_illegal_invocation(js);
  return url_apply_setter(js, s, nargs ? args[0] : js_mkundef(), ada_set_password);
}

static ant_value_t url_set_host(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return url_illegal_invocation(js);
  return url_apply_setter(js, s, nargs ? args[0] : js_mkundef(), ada_set_host);
}

static ant_value_t url_set_hostname(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return url_illegal_invocation(js);
  return url_apply_setter(js, s, nargs ? args[0] : js_mkundef(), ada_set_hostname);
}

static ant_value_t url_set_port(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return url_illegal_invocation(js);
  return url_apply_setter(js, s, nargs ? args[0] : js_mkundef(), ada_set_port);
}

static ant_value_t url_set_pathname(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return url_illegal_invocation(js);
  return url_apply_setter(js, s, nargs ? args[0] : js_mkundef(), ada_set_pathname);
}

static ant_value_t url_set_search(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return url_illegal_invocation(js);
  ant_value_t result = url_apply_setter(js, s, nargs ? args[0] : js_mkundef(), ada_set_search_bool);
  if (is_err(result)) return result;
  const char *query = s->search && s->search[0] == '?' ? s->search + 1 : "";
  url_sync_usp(js, js->this_val, query);
  return result;
}

static ant_value_t url_set_hash(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return url_illegal_invocation(js);
  return url_apply_setter(js, s, nargs ? args[0] : js_mkundef(), ada_set_hash_bool);
}

static ant_value_t url_toString(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return url_illegal_invocation(js);
  char *href = build_href(s);
  if (!href) return js_mkstr(js, "", 0);
  ant_value_t ret = js_mkstr(js, href, strlen(href));
  free(href);
  return ret;
}

static ant_value_t js_URL(ant_params_t) {
  if (is_undefined(call_new_target))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Class constructor URL cannot be invoked without 'new'");
  
  if (nargs < 1) return url_missing_args_error(js, "The \"url\" argument must be specified");

  ant_value_t url_sv = url_string_value(js, args[0]);
  if (is_err(url_sv)) return url_sv;
  size_t url_len = 0;
  const char *url_str = js_getstr(js, url_sv, &url_len);

  const char *base_str = NULL;
  size_t base_len = 0;
  ant_value_t base_sv = js_mkundef();
  
  if (nargs > 1 && !is_undefined(args[1])) {
    base_sv = url_string_value(js, args[1]);
    if (is_err(base_sv)) return base_sv;
    base_str = js_getstr(js, base_sv, &base_len);
  }

  url_state_t *s = calloc(1, sizeof(url_state_t));
  if (!s) return Ant_Error_CreateOutOfMemory(js);

  if (!url_str || parse_url_to_state_n(url_str, url_len, base_str, base_len, s) != 0) {
    free(s);
    return url_invalid_url_error(js, url_sv, base_sv);
  }

  ant_value_t obj = Ant_Object_CreateBare(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, js->builtins.url_proto, call_new_target);
  js_set_proto_init(obj, is_object_type(proto) ? proto : js->builtins.url_proto);
  js_set_native(obj, s, URL_NATIVE_TAG);
  js_set_finalizer(obj, url_finalize);

  const char *query = (s->search && s->search[0] == '?') ? s->search + 1 : "";
  ant_value_t usp = make_usp_for_url(js, obj, query);
  js_set_slot_wb(js, obj, SLOT_ENTRIES, usp);

  return obj;
}

ant_value_t make_url_obj(ant_t *js, url_state_t *s) {
  ant_value_t obj = Ant_Object_CreateBare(js);
  js_set_proto_init(obj, js->builtins.url_proto);
  js_set_native(obj, s, URL_NATIVE_TAG);
  js_set_finalizer(obj, url_finalize);
  
  const char *query = (s->search && s->search[0] == '?') ? s->search + 1 : "";
  ant_value_t usp = make_usp_for_url(js, obj, query);
  js_set_slot_wb(js, obj, SLOT_ENTRIES, usp);
  
  return obj;
}

static ant_value_t url_canParse(ant_params_t) {
  if (nargs < 1) return url_missing_args_error(js, "The \"url\" argument must be specified");
  
  ant_value_t url_sv = url_string_value(js, args[0]);
  if (is_err(url_sv)) return url_sv;
  
  size_t url_len = 0;
  const char *url_str = js_getstr(js, url_sv, &url_len);
  if (!url_str) return js_false;
  
  const char *base_str = NULL;
  size_t base_len = 0;
  ant_value_t base_sv = js_mkundef();
  
  if (nargs > 1 && !is_undefined(args[1])) {
    base_sv = url_string_value(js, args[1]);
    if (is_err(base_sv)) return base_sv;
    base_str = js_getstr(js, base_sv, &base_len);
  }
  
  url_state_t s;
  if (parse_url_to_state_n(url_str, url_len, base_str, base_len, &s) != 0) return js_false;
  url_state_clear(&s);
  
  return js_true;
}

static ant_value_t url_parse(ant_params_t) {
  if (nargs < 1) return url_missing_args_error(js, "The \"url\" argument must be specified");
  
  ant_value_t url_sv = url_string_value(js, args[0]);
  if (is_err(url_sv)) return url_sv;
  
  size_t url_len = 0;
  const char *url_str = js_getstr(js, url_sv, &url_len);
  if (!url_str) return js_mknull();
  
  const char *base_str = NULL;
  size_t base_len = 0;
  ant_value_t base_sv = js_mkundef();
  
  if (nargs > 1 && !is_undefined(args[1])) {
    base_sv = url_string_value(js, args[1]);
    if (is_err(base_sv)) return base_sv;
    base_str = js_getstr(js, base_sv, &base_len);
  }
  
  url_state_t *s = calloc(1, sizeof(url_state_t));
  if (!s) return js_mknull();
  
  if (parse_url_to_state_n(url_str, url_len, base_str, base_len, s) != 0) {
    free(s);
    return js_mknull();
  }
  
  return make_url_obj(js, s);
}

static size_t object_url_key_length(const char *url) {
  const char *hash = url ? strchr(url, '#') : NULL;
  return hash ? (size_t)(hash - url) : (url ? strlen(url) : 0);
}

ant_value_t url_resolve_object_url(ant_t *js, const char *url) {
  if (!url || strncmp(url, "blob:", 5) != 0 || !is_object_type(js->builtins.url_ctor))
    return js_mkundef();

  ant_value_t store = js_get_slot(js->builtins.url_ctor, SLOT_DATA);
  if (vtype(store) != kTypeArray) return js_mkundef();

  size_t url_len = object_url_key_length(url);
  ant_offset_t len = js_arr_len(js, store);
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t entry = js_arr_get(js, store, i);
    ant_value_t key = js_arr_get(js, entry, 0);
    
    size_t key_len = 0;
    const char *key_str = js_getstr(js, key, &key_len);
    if (key_str && key_len == url_len && memcmp(key_str, url, url_len) == 0)
      return js_arr_get(js, entry, 1);
  }

  return js_mkundef();
}

static ant_value_t url_create_object_url(ant_params_t) {
  if (nargs < 1)
    return js_mkerr_typed(js, JS_ERR_TYPE, "URL.createObjectURL requires at least 1 argument");
  if (!blob_is_blob(js, args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "URL.createObjectURL requires a Blob");

  char uuid[37];
  if (crypto_random_uuid(uuid) < 0) return js_mkerr(js, "failed to generate blob URL");

  char url[sizeof("blob:ant/") + 36];
  snprintf(url, sizeof(url), "blob:ant/%s", uuid);
  ant_value_t url_value = js_mkstr(js, url, strlen(url));

  ant_value_t store = js_get_slot(js->builtins.url_ctor, SLOT_DATA);
  if (vtype(store) != kTypeArray) {
    store = js_mkarr(js);
    js_set_slot_wb(js, js->builtins.url_ctor, SLOT_DATA, store);
  }

  ant_value_t entry = js_mkarr(js);
  js_arr_push(js, entry, url_value);
  js_arr_push(js, entry, args[0]);
  js_arr_push(js, store, entry);
  return url_value;
}

static ant_value_t url_revoke_object_url(ant_params_t) {
  if (nargs < 1)
    return js_mkerr_typed(js, JS_ERR_TYPE, "URL.revokeObjectURL requires at least 1 argument");

  ant_value_t value = url_string_value(js, args[0]);
  if (is_err(value)) return value;
  size_t url_len = 0;
  const char *url = js_getstr(js, value, &url_len);
  if (!url || url_len < 5 || strncmp(url, "blob:", 5) != 0) return js_mkundef();
  url_len = object_url_key_length(url);

  ant_value_t store = js_get_slot(js->builtins.url_ctor, SLOT_DATA);
  if (vtype(store) != kTypeArray) return js_mkundef();

  ant_value_t next = js_mkarr(js);
  ant_offset_t len = js_arr_len(js, store);
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t entry = js_arr_get(js, store, i);
    ant_value_t key = js_arr_get(js, entry, 0);
    size_t key_len = 0;
    const char *key_str = js_getstr(js, key, &key_len);
    if (key_str && key_len == url_len && memcmp(key_str, url, url_len) == 0) continue;
    js_arr_push(js, next, entry);
  }
  
  js_set_slot_wb(js, js->builtins.url_ctor, SLOT_DATA, next);
  return js_mkundef();
}


void init_url_module(ant_t *js) {
  url_init_search_params(js);
  js->builtins.url_proto = Ant_Object_CreateBare(js);
  
  js_set_accessor_desc(js, js->builtins.url_proto, "href", 4, Ant_Function_Create(url_get_href), Ant_Function_Create(url_set_href), JS_DESC_E | JS_DESC_C);
  js_set_accessor_desc(js, js->builtins.url_proto, "protocol", 8, Ant_Function_Create(url_get_protocol), Ant_Function_Create(url_set_protocol), JS_DESC_E | JS_DESC_C);
  js_set_accessor_desc(js, js->builtins.url_proto, "username", 8,  Ant_Function_Create(url_get_username), Ant_Function_Create(url_set_username), JS_DESC_E | JS_DESC_C);
  js_set_accessor_desc(js, js->builtins.url_proto, "password", 8,  Ant_Function_Create(url_get_password), Ant_Function_Create(url_set_password), JS_DESC_E | JS_DESC_C);
  js_set_accessor_desc(js, js->builtins.url_proto, "host", 4,  Ant_Function_Create(url_get_host), Ant_Function_Create(url_set_host), JS_DESC_E | JS_DESC_C);
  js_set_accessor_desc(js, js->builtins.url_proto, "hostname", 8,  Ant_Function_Create(url_get_hostname), Ant_Function_Create(url_set_hostname), JS_DESC_E | JS_DESC_C);
  js_set_accessor_desc(js, js->builtins.url_proto, "port", 4,  Ant_Function_Create(url_get_port), Ant_Function_Create(url_set_port), JS_DESC_E | JS_DESC_C);
  js_set_accessor_desc(js, js->builtins.url_proto, "pathname", 8,  Ant_Function_Create(url_get_pathname), Ant_Function_Create(url_set_pathname), JS_DESC_E | JS_DESC_C);
  js_set_accessor_desc(js, js->builtins.url_proto, "search", 6,  Ant_Function_Create(url_get_search), Ant_Function_Create(url_set_search), JS_DESC_E | JS_DESC_C);
  js_set_accessor_desc(js, js->builtins.url_proto, "hash", 4,  Ant_Function_Create(url_get_hash), Ant_Function_Create(url_set_hash), JS_DESC_E | JS_DESC_C);
  
  js_set_getter_desc(js, js->builtins.url_proto, "origin", 6,  Ant_Function_Create(url_get_origin), JS_DESC_E | JS_DESC_C);
  js_set_getter_desc(js, js->builtins.url_proto, "searchParams", 12, Ant_Function_Create(url_get_searchParams), JS_DESC_E | JS_DESC_C);
  
  js_set(js, js->builtins.url_proto, "toString", Ant_Function_Create(url_toString));
  js_set(js, js->builtins.url_proto, "toJSON",   Ant_Function_Create(url_toString));
  js_set_tostringtag(js, js->builtins.url_proto, js_mkstr(js, "URL", 3));

  js->builtins.url_ctor = Ant_Function_CreateIntrinsicConstructor(js, js_URL, js->builtins.url_proto, "URL", 1);
  js_set_slot(js->builtins.url_ctor, SLOT_DATA, js_mkarr(js));
  
  js_set(js, js->builtins.url_ctor, "canParse",        Ant_Function_CreateWithArity(url_canParse, 1));
  js_set(js, js->builtins.url_ctor, "createObjectURL", Ant_Function_CreateWithArity(url_create_object_url, 1));
  js_set(js, js->builtins.url_ctor, "parse",           Ant_Function_CreateWithArity(url_parse, 1));
  js_set(js, js->builtins.url_ctor, "revokeObjectURL", Ant_Function_CreateWithArity(url_revoke_object_url, 1));
  js_set_global_builtin(js, "URL", js->builtins.url_ctor);
}
