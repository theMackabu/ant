#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netdb.h>
#endif

#include "internal.h"
#include "errors.h"
#include "gc/uv.h"
#include "gc/roots.h"
#include "dns_internal.h"

static constexpr int DNS_HINTS_MASK = AI_ADDRCONFIG | AI_V4MAPPED | AI_ALL;

int dns_ip_family(const char *address) {
  char buf[INET6_ADDRSTRLEN + 16];
  unsigned char raw[sizeof(struct in6_addr)];
  if (!address) return 0;
  if (uv_inet_pton(AF_INET, address, raw) == 0) return 4;

  const char *zone = strchr(address, '%');
  size_t len = zone ? (size_t)(zone - address) : strlen(address);
  if (len >= sizeof(buf)) return 0;

  memcpy(buf, address, len);
  buf[len] = '\0';
  return uv_inet_pton(AF_INET6, buf, raw) == 0 ? 6 : 0;
}

typedef struct {
  int family;
  int hints;
  dns_order_t order;
  bool all;
} dns_lookup_options_t;

static ant_value_t dns_invalid_type(ant_t *js, const char *name, const char *expected, ant_value_t value) {
  if (!strchr(name, '.')) return Ant_Error_CreateInvalidArgType(js, name, expected, value);
  char got[160], message[320];
  js_describe_received(js, value, got, sizeof(got));
  snprintf(message, sizeof(message), "The \"%s\" property must be %s. %s", name, expected, got);
  return js_mkerr_code(js, JS_ERR_TYPE, "ERR_INVALID_ARG_TYPE", message);
}

static ant_value_t dns_parse_family(ant_t *js, ant_value_t value, const char *name, int *out) {
  if (vtype(value) == kTypeString) {
    size_t len = 0;
    const char *text = js_getstr(js, value, &len);
    if (len == 4 && memcmp(text, "IPv4", 4) == 0) { *out = 4; return js_mkundef(); }
    if (len == 4 && memcmp(text, "IPv6", 4) == 0) { *out = 6; return js_mkundef(); }
  }

  double family = vtype(value) == kTypeNumber ? js_getnum(value) : -1;
  if (family != 0 && family != 4 && family != 6) return Ant_Error_CreateInvalidArgValue(js, name, "0, 4, 6", value);

  *out = (int)family;
  return js_mkundef();
}

static ant_value_t dns_parse_order(ant_t *js, ant_value_t value, const char *name, dns_order_t *out) {
  static const struct { const char *text; dns_order_t order; } orders[] = {
    { "verbatim", DNS_ORDER_VERBATIM },
    { "ipv4first", DNS_ORDER_IPV4_FIRST },
    { "ipv6first", DNS_ORDER_IPV6_FIRST },
  };

  size_t len = 0;
  const char *text = vtype(value) == kTypeString ? js_getstr(js, value, &len) : NULL;
  for (size_t i = 0; text && i < countof(orders); i++) {
    if (strlen(orders[i].text) != len || memcmp(orders[i].text, text, len) != 0) continue;
    *out = orders[i].order;
    return js_mkundef();
  }

  return Ant_Error_CreateInvalidArgValue(js, name, "'verbatim', 'ipv4first', 'ipv6first'", value);
}

ant_value_t dns_parse_result_order(ant_t *js, ant_value_t value, dns_order_t *out) {
  return dns_parse_order(js, value, "dnsOrder", out);
}

static ant_value_t dns_parse_lookup_object(ant_t *js, ant_value_t options, dns_lookup_options_t *out) {
  if (!is_object_type(options)) return js_mkundef();
  ant_value_t r;

  ant_value_t hints = js_get(js, options, "hints");
  if (vtype(hints) != kTypeUndefined && vtype(hints) != kTypeNull) {
    if (vtype(hints) != kTypeNumber) return dns_invalid_type(js, "options.hints", "of type number", hints);
    uint32_t flags = (uint32_t)(int64_t)js_getnum(hints);
    if (flags & ~(uint32_t)DNS_HINTS_MASK) return Ant_Error_CreateInvalidArgValue(js, "hints", NULL, hints);
    out->hints = (int)flags;
  }

  ant_value_t family = js_get(js, options, "family");
  if (vtype(family) != kTypeUndefined && vtype(family) != kTypeNull)
    if (is_err(r = dns_parse_family(js, family, "options.family", &out->family))) return r;

  ant_value_t all = js_get(js, options, "all");
  if (vtype(all) != kTypeUndefined && vtype(all) != kTypeNull) {
    if (vtype(all) != kTypeBool) return dns_invalid_type(js, "options.all", "of type boolean", all);
    out->all = all == js_true;
  }

  ant_value_t verbatim = js_get(js, options, "verbatim");
  if (vtype(verbatim) != kTypeUndefined && vtype(verbatim) != kTypeNull) {
    if (vtype(verbatim) != kTypeBool) return dns_invalid_type(js, "options.verbatim", "of type boolean", verbatim);
    out->order = verbatim == js_true ? DNS_ORDER_VERBATIM : DNS_ORDER_IPV4_FIRST;
  }

  ant_value_t order = js_get(js, options, "order");
  if (vtype(order) != kTypeUndefined && vtype(order) != kTypeNull)
    if (is_err(r = dns_parse_order(js, order, "options.order", &out->order))) return r;

  return js_mkundef();
}

static ant_value_t dns_address_object(ant_t *js, const char *address, int family) {
  ant_value_t obj = Ant_Object_Create(js);
  js_set(js, obj, "address", address ? js_mkstr(js, address, strlen(address)) : js_mknull());
  js_set(js, obj, "family", js_mknum(family));
  return obj;
}

static void dns_lookup_finish(dns_request_t *req, const char *address, int family, ant_value_t list) {
  ant_t *js = req->js;
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, list);

  if (req->all) {
    dns_request_resolve(req, list, &list, 1);
  } else {
    ant_value_t args[2] = { address ? js_mkstr(js, address, strlen(address)) : js_mknull(), js_mknum(family) };
    GC_ROOT_PIN(js, args[0]);
    ant_value_t value = dns_address_object(js, address, family);
    dns_request_resolve(req, value, args, 2);
  }

  GC_ROOT_RESTORE(js, mark);
}

static int dns_addrinfo_family(const struct addrinfo *ai) {
  return ai->ai_family == AF_INET ? 4 : ai->ai_family == AF_INET6 ? 6 : 0;
}

static bool dns_addrinfo_text(const struct addrinfo *ai, char *out, size_t len) {
  if (ai->ai_family == AF_INET) return uv_ip4_name((const struct sockaddr_in *)ai->ai_addr, out, len) == 0;
  if (ai->ai_family == AF_INET6) return uv_ip6_name((const struct sockaddr_in6 *)ai->ai_addr, out, len) == 0;
  return false;
}

static void dns_getaddrinfo_cb(uv_getaddrinfo_t *uvreq, int status, struct addrinfo *res) {
  GC_UV_CALLBACK();
  dns_request_t *req = uvreq->data;
  ant_t *js = req->js;

  if (js->destroying) {
    if (res) uv_freeaddrinfo(res);
    dns_request_free(req);
    return;
  }

  const char *hostname = js_getstr(js, req->hostname, NULL);
  if (status < 0) {
    dns_request_reject(req, dns_uv_error(js, status, "getaddrinfo", hostname));
    return;
  }

  GC_ROOT_SAVE(mark, js);
  ant_value_t list = js_mkarr(js);
  GC_ROOT_PIN(js, list);

  char first[INET6_ADDRSTRLEN] = "";
  int first_family = 0;

  for (int pass = 0; pass < 2; pass++) for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    int family = dns_addrinfo_family(ai);
    if (!family) continue;
    if (req->order == DNS_ORDER_IPV4_FIRST && family != (pass == 0 ? 4 : 6)) continue;
    if (req->order == DNS_ORDER_IPV6_FIRST && family != (pass == 0 ? 6 : 4)) continue;
    if (req->order == DNS_ORDER_VERBATIM && pass == 1) continue;

    char text[INET6_ADDRSTRLEN];
    if (!dns_addrinfo_text(ai, text, sizeof(text))) continue;
    if (!first_family) { memcpy(first, text, sizeof(text)); first_family = family; }
    if (req->all) js_arr_push(js, list, dns_address_object(js, text, family));
  }

  if (res) uv_freeaddrinfo(res);
  if (!first_family) dns_request_reject(req, dns_uv_error(js, UV_EAI_NODATA, "getaddrinfo", hostname));
  else dns_lookup_finish(req, first, first_family, list);

  GC_ROOT_RESTORE(js, mark);
}

ant_value_t dns_lookup_impl(ant_native_params_t, bool promises) {
  ant_value_t hostname = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t options = nargs > 1 ? args[1] : js_mkundef();
  
  ant_value_t callback = promises ? js_mkundef() : nargs > 2 ? args[2] : js_mkundef();
  dns_lookup_options_t opts = { .order = (dns_order_t)js->dns_result_order };
  ant_value_t r;

  if (js_truthy(js, hostname) && vtype(hostname) != kTypeString)
    return Ant_Error_CreateInvalidArgType(js, "hostname", "of type string", hostname);
  if (is_err(r = Ant_Error_CheckNoNullBytes(js, "hostname", hostname))) return r;

  if (!promises && is_callable(options)) callback = options;
  else if (vtype(options) == kTypeNumber) {
    if (!promises && !is_callable(callback)) return Ant_Error_CreateInvalidArgType(js, "callback", "of type function", callback);
    if (is_err(r = dns_parse_family(js, options, "family", &opts.family))) return r;
  } else if (vtype(options) != kTypeUndefined && !is_object_type(options)) {
    ant_value_t cb = nargs == 2 ? options : callback;
    if (!promises && !is_callable(cb)) return Ant_Error_CreateInvalidArgType(js, "callback", "of type function", cb);
    return Ant_Error_CreateInvalidArgType(js, "options", "of type object or integer", options);
  } else {
    if (!promises && !is_callable(callback)) return Ant_Error_CreateInvalidArgType(js, "callback", "of type function", callback);
    if (is_err(r = dns_parse_lookup_object(js, options, &opts))) return r;
  }

  dns_request_t *req = dns_request_new(js, DNS_REQ_LOOKUP, callback, hostname);
  if (!req) return Ant_Error_CreateOutOfMemory(js);
  req->family = opts.family;
  req->all = opts.all;
  req->order = opts.order;

  ant_value_t result = vtype(req->promise) == kTypePromise ? req->promise : js_mkundef();
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, result);

  const char *host = js_truthy(js, hostname) ? js_getstr(js, hostname, NULL) : NULL;
  int literal = dns_ip_family(host);

  if (!host || literal) {
    ant_value_t list = js_mkarr(js);
    GC_ROOT_PIN(js, list);
    int family = host ? literal : opts.family == 6 ? 6 : 4;
    if (host && opts.all) js_arr_push(js, list, dns_address_object(js, host, family));
    dns_lookup_finish(req, host, family, list);
    GC_ROOT_RESTORE(js, mark);
    return result;
  }

  struct addrinfo hints = {
    .ai_family = opts.family == 4 ? AF_INET : opts.family == 6 ? AF_INET6 : AF_UNSPEC,
    .ai_socktype = SOCK_STREAM,
    .ai_flags = opts.hints,
  };

  req->uv.getaddrinfo.data = req;
  int rc = uv_getaddrinfo(js->loop, &req->uv.getaddrinfo, dns_getaddrinfo_cb, host, NULL, &hints);
  if (rc < 0) dns_request_reject(req, dns_uv_error(js, rc, "getaddrinfo", host));
  else dns_request_result(req);

  GC_ROOT_RESTORE(js, mark);
  return result;
}

static ant_value_t dns_parse_port(ant_t *js, ant_value_t value, int *out) {
  double port = (double)NAN;
  if (vtype(value) == kTypeNumber) port = js_getnum(value);
  else if (vtype(value) == kTypeString) {
    size_t len = 0;
    const char *text = js_getstr(js, value, &len);
    char *end = NULL;
    if (len > 0) port = strtod(text, &end);
    if (!end || *end) port = (double)NAN;
  }

  if (!(port >= 0 && port < 65536) || port != floor(port)) {
    char got[160], message[256];
    js_describe_received(js, value, got, sizeof(got));
    snprintf(message, sizeof(message), "Port should be >= 0 and < 65536. %s.", got);
    return js_mkerr_code(js, JS_ERR_RANGE, "ERR_SOCKET_BAD_PORT", message);
  }

  *out = (int)port;
  return js_mkundef();
}

static void dns_getnameinfo_cb(uv_getnameinfo_t *uvreq, int status, const char *hostname, const char *service) {
  GC_UV_CALLBACK();
  dns_request_t *req = uvreq->data;
  ant_t *js = req->js;

  if (js->destroying) {
    dns_request_free(req);
    return;
  }

  if (status < 0) {
    dns_request_reject(req, dns_uv_error(js, status, "getnameinfo", js_getstr(js, req->hostname, NULL)));
    return;
  }

  GC_ROOT_SAVE(mark, js);
  ant_value_t args[2] = { js_mkstr(js, hostname, strlen(hostname)), js_mkstr(js, service, strlen(service)) };
  GC_ROOT_PIN(js, args[0]);
  GC_ROOT_PIN(js, args[1]);

  ant_value_t value = Ant_Object_Create(js);
  GC_ROOT_PIN(js, value);
  js_set(js, value, "hostname", args[0]);
  js_set(js, value, "service", args[1]);

  dns_request_resolve(req, value, args, 2);
  GC_ROOT_RESTORE(js, mark);
}

ant_value_t dns_lookup_service_impl(ant_native_params_t, bool promises) {
  if (nargs < (promises ? 2 : 3)) return js_mkerr_code(
    js, JS_ERR_TYPE, "ERR_MISSING_ARGS", promises 
      ? "The \"address\" and \"port\" arguments must be specified"
      : "The \"address\", \"port\", and \"callback\" arguments must be specified"
  );

  ant_value_t address = args[0];
  ant_value_t callback = promises ? js_mkundef() : args[2];
  if (vtype(address) != kTypeString) return Ant_Error_CreateInvalidArgType(js, "address", "of type string", address);

  const char *text = js_getstr(js, address, NULL);
  int family = dns_ip_family(text);
  if (!family) return Ant_Error_CreateInvalidArgValue(js, "address", NULL, address);

  int port = 0;
  ant_value_t r = dns_parse_port(js, args[1], &port);
  if (is_err(r)) return r;
  if (!promises && !is_callable(callback)) return Ant_Error_CreateInvalidArgType(js, "callback", "of type function", callback);

  struct sockaddr_storage addr;
  int rc = family == 4
    ? uv_ip4_addr(text, port, (struct sockaddr_in *)&addr)
    : uv_ip6_addr(text, port, (struct sockaddr_in6 *)&addr);
  if (rc < 0) return Ant_Error_CreateInvalidArgValue(js, "address", NULL, address);

  dns_request_t *req = dns_request_new(js, DNS_REQ_LOOKUP_SERVICE, callback, address);
  if (!req) return Ant_Error_CreateOutOfMemory(js);

  ant_value_t result = vtype(req->promise) == kTypePromise ? req->promise : js_mkundef();
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, result);

  req->uv.getnameinfo.data = req;
  rc = uv_getnameinfo(js->loop, &req->uv.getnameinfo, dns_getnameinfo_cb, (const struct sockaddr *)&addr, NI_NAMEREQD);
  if (rc < 0) dns_request_reject(req, dns_uv_error(js, rc, "getnameinfo", text));
  else dns_request_result(req);

  GC_ROOT_RESTORE(js, mark);
  return result;
}
