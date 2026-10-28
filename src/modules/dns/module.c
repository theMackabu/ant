#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <netdb.h>
#endif

#include "internal.h"
#include "cfunc.h"
#include "errors.h"
#include "ptr.h"
#include "gc/roots.h"
#include "gc/modules.h"
#include "esm/library.h"
#include "modules/dns.h"
#include "dns_internal.h"

static constexpr uint32_t DNS_RESOLVER_NATIVE_TAG = Ant_NativeTag("DNSR");

static const char *const dns_order_names[] = {
  [DNS_ORDER_VERBATIM] = "verbatim",
  [DNS_ORDER_IPV4_FIRST] = "ipv4first",
  [DNS_ORDER_IPV6_FIRST] = "ipv6first",
};

dns_channel_t *dns_this_channel(ant_t *js) {
  ant_value_t self = js_getthis(js);
  dns_channel_t **cell = js_get_native(self, DNS_RESOLVER_NATIVE_TAG);
  return cell ? *cell : dns_default_channel(js);
}

#define DNS_RECORD_TYPE(name, method, syscall, ares)                                                                   \
  static ant_value_t dns_cb_##method(ant_params_t) { return dns_resolve_impl(js, args, nargs, DNS_RR_##name, false); } \
  static ant_value_t dns_promise_##method(ant_params_t) { return dns_resolve_impl(js, args, nargs, DNS_RR_##name, true); }
#include "dns/record_types.h"

static ant_value_t dns_cb_lookup(ant_params_t) { return dns_lookup_impl(js, args, nargs, false); }
static ant_value_t dns_promise_lookup(ant_params_t) { return dns_lookup_impl(js, args, nargs, true); }
static ant_value_t dns_cb_lookup_service(ant_params_t) { return dns_lookup_service_impl(js, args, nargs, false); }
static ant_value_t dns_promise_lookup_service(ant_params_t) { return dns_lookup_service_impl(js, args, nargs, true); }
static ant_value_t dns_cb_resolve(ant_params_t) { return dns_resolve_impl(js, args, nargs, -1, false); }
static ant_value_t dns_promise_resolve(ant_params_t) { return dns_resolve_impl(js, args, nargs, -1, true); }
static ant_value_t dns_cb_reverse(ant_params_t) { return dns_reverse_impl(js, args, nargs, false); }
static ant_value_t dns_promise_reverse(ant_params_t) { return dns_reverse_impl(js, args, nargs, true); }

static ant_value_t dns_get_servers(ant_params_t) {
  return dns_channel_get_servers(js, dns_this_channel(js));
}

static ant_value_t dns_set_servers(ant_params_t) {
  return dns_channel_set_servers(js, dns_this_channel(js), nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t dns_set_default_result_order(ant_params_t) {
  dns_order_t order;
  ant_value_t r = dns_parse_result_order(js, nargs > 0 ? args[0] : js_mkundef(), &order);
  if (is_err(r)) return r;
  js->dns_result_order = (uint8_t)order;
  return js_mkundef();
}

static ant_value_t dns_get_default_result_order(ant_params_t) {
  const char *name = dns_order_names[js->dns_result_order];
  return js_mkstr(js, name, strlen(name));
}

static ant_value_t dns_resolver_cancel(ant_params_t) {
  dns_channel_t *channel = dns_this_channel(js);
  if (channel && channel->ares) ares_cancel(channel->ares);
  return js_mkundef();
}

static ant_value_t dns_resolver_set_local_address(ant_params_t) {
  dns_channel_t *channel = dns_this_channel(js);
  if (!channel || !channel->ares) return js_mkundef();

  for (int i = 0; i < 2 && i < nargs; i++) {
    if (vtype(args[i]) == kTypeUndefined) continue;
    if (vtype(args[i]) != kTypeString) return Ant_Error_CreateInvalidArgType(js, i ? "ipv6" : "ipv4", "of type string", args[i]);
    ant_value_t nul = Ant_Error_CheckNoNullBytes(js, i ? "ipv6" : "ipv4", args[i]);
    if (is_err(nul)) return nul;

    const char *text = js_getstr(js, args[i], NULL);
    unsigned char raw[16];
    int family = dns_ip_family(text);

    if (family == 4 && uv_inet_pton(AF_INET, text, raw) == 0) {
      ares_set_local_ip4(channel->ares, ((unsigned)raw[0] << 24) | ((unsigned)raw[1] << 16) | ((unsigned)raw[2] << 8) | raw[3]);
    } else if (family == 6 && uv_inet_pton(AF_INET6, text, raw) == 0) {
      ares_set_local_ip6(channel->ares, raw);
    } else {
      char message[320];
      snprintf(message, sizeof(message), "Invalid IP address: %s", text);
      return js_mkerr_code(js, JS_ERR_TYPE, "ERR_INVALID_IP_ADDRESS", message);
    }
  }

  return js_mkundef();
}

static void dns_resolver_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  dns_channel_t **cell = js_get_native(value, DNS_RESOLVER_NATIVE_TAG);
  if (!cell) return;

  js_clear_native(value, DNS_RESOLVER_NATIVE_TAG);
  if (*cell) dns_channel_destroy(*cell);
  free(cell);
}

static ant_value_t dns_resolver_option(ant_t *js, ant_value_t options, const char *name, int min, int fallback, int *out) {
  *out = fallback;
  ant_value_t value = is_object_type(options) ? js_get(js, options, name) : js_mkundef();
  if (vtype(value) == kTypeUndefined) return js_mkundef();

  char label[32];
  snprintf(label, sizeof(label), "options.%s", name);
  if (vtype(value) != kTypeNumber) return Ant_Error_CreateInvalidArgType(js, label, "of type number", value);

  double number = js_getnum(value);
  if (number != (double)(int)number || number < min) {
    char message[160];
    snprintf(message, sizeof(message), "The value of \"%s\" is out of range. It must be >= %d and <= 2147483647. Received %g", label, min, number);
    return js_mkerr_code(js, JS_ERR_RANGE, "ERR_OUT_OF_RANGE", message);
  }

  *out = (int)number;
  return js_mkundef();
}

static ant_value_t dns_resolver_construct(ant_native_params_t, ant_value_t new_target, ant_value_t fallback_proto) {
  if (!is_object_type(new_target))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Class constructor Resolver cannot be invoked without 'new'");

  ant_value_t options = nargs > 0 ? args[0] : js_mkundef();
  int timeout = -1, tries = 4;
  ant_value_t r = dns_resolver_option(js, options, "timeout", -1, -1, &timeout);
  
  if (is_err(r)) return r;
  if (is_err(r = dns_resolver_option(js, options, "tries", 1, 4, &tries))) return r;

  dns_channel_t **cell = malloc(sizeof(dns_channel_t *));
  if (!cell) return Ant_Error_CreateOutOfMemory(js);

  *cell = dns_channel_create(js, timeout, tries);
  if (!*cell) {
    free(cell);
    return js_mkerr(js, "failed to create DNS channel");
  }

  (*cell)->owner_ref = cell;
  ant_value_t obj = Ant_Object_CreateBare(js);
  
  js_set_proto_init(obj, js_instance_proto_from_new_target(js, fallback_proto, new_target));
  js_set_native(obj, cell, DNS_RESOLVER_NATIVE_TAG);
  js_set_finalizer(obj, dns_resolver_finalize);
  
  return obj;
}

static ant_value_t dns_resolver_ctor(ant_params_t) {
  return dns_resolver_construct(js, args, nargs, call_new_target, js->builtins.dns_resolver_proto);
}

static ant_value_t dns_promises_resolver_ctor(ant_params_t) {
  return dns_resolver_construct(js, args, nargs, call_new_target, js->builtins.dns_promises_resolver_proto);
}

static const ant_cfunc_meta_t dns_callback_resolvers[] = {
  { dns_cb_resolve, "resolve", 3, 0 },
  { dns_cb_reverse, "reverse", 2, 0 },
#define DNS_RECORD_TYPE(name, method, syscall, ares) { dns_cb_##method, #method, 3, 0 },
#include "dns/record_types.h"
};

static const ant_cfunc_meta_t dns_promise_resolvers[] = {
  { dns_promise_resolve, "resolve", 2, 0 },
  { dns_promise_reverse, "reverse", 1, 0 },
#define DNS_RECORD_TYPE(name, method, syscall, ares) { dns_promise_##method, #method, 2, 0 },
#include "dns/record_types.h"
};

static const ant_cfunc_meta_t dns_shared_methods[] = {
  { dns_get_servers, "getServers", 0, 0 },
  { dns_set_servers, "setServers", 1, 0 },
};

static const ant_cfunc_meta_t dns_resolver_methods[] = {
  { dns_resolver_cancel, "cancel", 0, 0 },
  { dns_resolver_set_local_address, "setLocalAddress", 2, 0 },
};

static const ant_cfunc_meta_t dns_order_methods[] = {
  { dns_set_default_result_order, "setDefaultResultOrder", 1, 0 },
  { dns_get_default_result_order, "getDefaultResultOrder", 0, 0 },
};

static void dns_define_error_codes(ant_t *js, ant_value_t target) {
#define DNS_ERROR_CODE(name, ares) js_set(js, target, #name, js_mkstr(js, "E" #name, sizeof("E" #name) - 1));
#include "dns/error_codes.h"
}

static ant_value_t dns_make_resolver_class(ant_t *js, bool promises) {
  ant_value_t proto = Ant_Object_Create(js);
  js_define_methods(js, proto, promises ? dns_promise_resolvers : dns_callback_resolvers, promises ? countof(dns_promise_resolvers) : countof(dns_callback_resolvers), ANT_PROP_ATTR_METHOD);
  js_define_methods(js, proto, dns_shared_methods, countof(dns_shared_methods), ANT_PROP_ATTR_METHOD);
  js_define_methods(js, proto, dns_resolver_methods, countof(dns_resolver_methods), ANT_PROP_ATTR_METHOD);

  if (promises) {
    js->builtins.dns_promises_resolver_proto = proto;
    return Ant_Function_CreateConstructor(js, dns_promises_resolver_ctor, proto, "Resolver", 0);
  }

  js->builtins.dns_resolver_proto = proto;
  return Ant_Function_CreateConstructor(js, dns_resolver_ctor, proto, "Resolver", 0);
}

static ant_value_t dns_make_promises(ant_t *js) {
  ant_value_t promises = Ant_Object_CreateBare(js);
  js_set(js, promises, "lookup", Ant_Function_CreateWithArity(dns_promise_lookup, 2));
  js_set(js, promises, "lookupService", Ant_Function_CreateWithArity(dns_promise_lookup_service, 2));
  js_set(js, promises, "Resolver", dns_make_resolver_class(js, true));
  js_define_methods(js, promises, dns_order_methods, countof(dns_order_methods), ANT_PROP_ATTR_DEFAULT);
  js_define_methods(js, promises, dns_shared_methods, countof(dns_shared_methods), ANT_PROP_ATTR_DEFAULT);
  js_define_methods(js, promises, dns_promise_resolvers, countof(dns_promise_resolvers), ANT_PROP_ATTR_DEFAULT);
  dns_define_error_codes(js, promises);
  js_set(js, promises, "default", promises);
  return promises;
}

ant_value_t dns_library(ant_t *js) {
  GC_ROOT_SAVE(mark, js);
  ant_value_t lib = Ant_Object_CreateBare(js);
  GC_ROOT_PIN(js, lib);

  js_set(js, lib, "lookup", Ant_Function_CreateWithArity(dns_cb_lookup, 3));
  js_set(js, lib, "lookupService", Ant_Function_CreateWithArity(dns_cb_lookup_service, 3));
  js_set(js, lib, "Resolver", dns_make_resolver_class(js, false));
  js_define_methods(js, lib, dns_order_methods, countof(dns_order_methods), ANT_PROP_ATTR_DEFAULT);
  js_define_methods(js, lib, dns_shared_methods, countof(dns_shared_methods), ANT_PROP_ATTR_DEFAULT);

  js_set(js, lib, "ADDRCONFIG", js_mknum(AI_ADDRCONFIG));
  js_set(js, lib, "ALL", js_mknum(AI_ALL));
  js_set(js, lib, "V4MAPPED", js_mknum(AI_V4MAPPED));
  dns_define_error_codes(js, lib);

  js_define_methods(js, lib, dns_callback_resolvers, countof(dns_callback_resolvers), ANT_PROP_ATTR_DEFAULT);
  js_set(js, lib, "promises", dns_make_promises(js));
  js_set(js, lib, "default", lib);

  GC_ROOT_RESTORE(js, mark);
  return lib;
}

ant_value_t dns_promises_library(ant_t *js) {
  ant_library_entry_t *entry = ant_library_find("dns", 3);
  ant_value_t lib = entry ? ant_library_load(js, entry) : dns_library(js);
  return is_err(lib) ? lib : js_get(js, lib, "promises");
}

void gc_mark_dns(ant_t *js, gc_mark_fn mark) {
  for (dns_request_t *req = js->dns_requests; req; req = req->next) {
    mark(js, req->promise);
    mark(js, req->callback);
    mark(js, req->hostname);
    mark(js, req->owner);
  }
}

void cleanup_dns_module(ant_t *js) {
  for (dns_request_t *req = js->dns_requests; req; req = req->next) {
    if (req->kind == DNS_REQ_LOOKUP) uv_cancel((uv_req_t *)&req->uv.getaddrinfo);
    else if (req->kind == DNS_REQ_LOOKUP_SERVICE) uv_cancel((uv_req_t *)&req->uv.getnameinfo);
  }

  while (js->dns_channels) dns_channel_destroy(js->dns_channels);
}
