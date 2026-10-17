#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "errors.h"
#include "gc/roots.h"
#include "modules/buffer.h"
#include "dns_internal.h"

typedef enum {
  DNS_SHAPE_PLAIN,
  DNS_SHAPE_TTL,
  DNS_SHAPE_ANY,
} dns_shape_t;

static ant_value_t dns_str(ant_t *js, const char *text) {
  return text ? js_mkstr(js, text, strlen(text)) : js_mkstr(js, "", 0);
}

static void dns_set_str(ant_t *js, ant_value_t obj, const char *key, const ares_dns_rr_t *rr, ares_dns_rr_key_t field) {
  js_set(js, obj, key, dns_str(js, ares_dns_rr_get_str(rr, field)));
}

static void dns_set_num(ant_t *js, ant_value_t obj, const char *key, double value) {
  js_set(js, obj, key, js_mknum(value));
}

static ant_value_t dns_rr_address(ant_t *js, const ares_dns_rr_t *rr, ares_dns_rec_type_t type) {
  char text[INET6_ADDRSTRLEN];
  bool ok = type == ARES_REC_TYPE_A
    ? uv_inet_ntop(AF_INET, ares_dns_rr_get_addr(rr, ARES_RR_A_ADDR), text, sizeof(text)) == 0
    : uv_inet_ntop(AF_INET6, ares_dns_rr_get_addr6(rr, ARES_RR_AAAA_ADDR), text, sizeof(text)) == 0;
  return ok ? js_mkstr(js, text, strlen(text)) : js_mkstr(js, "", 0);
}

static ant_value_t dns_rr_txt(ant_t *js, const ares_dns_rr_t *rr) {
  ant_value_t chunks = js_mkarr(js);
  size_t count = ares_dns_rr_get_abin_cnt(rr, ARES_RR_TXT_DATA);

  for (size_t i = 0; i < count; i++) {
    size_t len = 0;
    const unsigned char *data = ares_dns_rr_get_abin(rr, ARES_RR_TXT_DATA, i, &len);
    js_arr_push(js, chunks, js_mkstr(js, data ? (const char *)data : "", data ? len : 0));
  }

  return chunks;
}

static ant_value_t dns_rr_value(ant_t *js, const ares_dns_rr_t *rr, dns_rr_t kind, dns_shape_t shape) {
  ares_dns_rec_type_t type = ares_dns_rr_get_type(rr);
  bool tagged = shape == DNS_SHAPE_ANY;
  ant_value_t obj = js_mkundef();

  switch (kind) {
    case DNS_RR_A:
    case DNS_RR_AAAA: {
      ant_value_t address = dns_rr_address(js, rr, type);
      if (shape == DNS_SHAPE_PLAIN) return address;
      obj = Ant_Object_Create(js);
      js_set(js, obj, "address", address);
      dns_set_num(js, obj, "ttl", ares_dns_rr_get_ttl(rr));
      break;
    }

    case DNS_RR_CNAME:
    case DNS_RR_NS:
    case DNS_RR_PTR: {
      ares_dns_rr_key_t field = kind == DNS_RR_CNAME ? ARES_RR_CNAME_CNAME : kind == DNS_RR_NS ? ARES_RR_NS_NSDNAME : ARES_RR_PTR_DNAME;
      ant_value_t name = dns_str(js, ares_dns_rr_get_str(rr, field));
      if (!tagged) return name;
      obj = Ant_Object_Create(js);
      js_set(js, obj, "value", name);
      break;
    }

    case DNS_RR_MX:
      obj = Ant_Object_Create(js);
      dns_set_str(js, obj, "exchange", rr, ARES_RR_MX_EXCHANGE);
      dns_set_num(js, obj, "priority", ares_dns_rr_get_u16(rr, ARES_RR_MX_PREFERENCE));
      break;

    case DNS_RR_TXT: {
      ant_value_t chunks = dns_rr_txt(js, rr);
      if (!tagged) return chunks;
      obj = Ant_Object_Create(js);
      js_set(js, obj, "entries", chunks);
      break;
    }

    case DNS_RR_SRV:
      obj = Ant_Object_Create(js);
      dns_set_str(js, obj, "name", rr, ARES_RR_SRV_TARGET);
      dns_set_num(js, obj, "port", ares_dns_rr_get_u16(rr, ARES_RR_SRV_PORT));
      dns_set_num(js, obj, "priority", ares_dns_rr_get_u16(rr, ARES_RR_SRV_PRIORITY));
      dns_set_num(js, obj, "weight", ares_dns_rr_get_u16(rr, ARES_RR_SRV_WEIGHT));
      break;

    case DNS_RR_SOA:
      obj = Ant_Object_Create(js);
      dns_set_str(js, obj, "nsname", rr, ARES_RR_SOA_MNAME);
      dns_set_str(js, obj, "hostmaster", rr, ARES_RR_SOA_RNAME);
      dns_set_num(js, obj, "serial", ares_dns_rr_get_u32(rr, ARES_RR_SOA_SERIAL));
      dns_set_num(js, obj, "refresh", ares_dns_rr_get_u32(rr, ARES_RR_SOA_REFRESH));
      dns_set_num(js, obj, "retry", ares_dns_rr_get_u32(rr, ARES_RR_SOA_RETRY));
      dns_set_num(js, obj, "expire", ares_dns_rr_get_u32(rr, ARES_RR_SOA_EXPIRE));
      dns_set_num(js, obj, "minttl", ares_dns_rr_get_u32(rr, ARES_RR_SOA_MINIMUM));
      break;

    case DNS_RR_CAA: {
      size_t len = 0;
      const char *tag = ares_dns_rr_get_str(rr, ARES_RR_CAA_TAG);
      const unsigned char *value = ares_dns_rr_get_bin(rr, ARES_RR_CAA_VALUE, &len);
      obj = Ant_Object_Create(js);
      dns_set_num(js, obj, "critical", ares_dns_rr_get_u8(rr, ARES_RR_CAA_CRITICAL));
      if (tag) js_set(js, obj, tag, js_mkstr(js, value ? (const char *)value : "", value ? len : 0));
      break;
    }

    case DNS_RR_NAPTR:
      obj = Ant_Object_Create(js);
      dns_set_str(js, obj, "flags", rr, ARES_RR_NAPTR_FLAGS);
      dns_set_str(js, obj, "service", rr, ARES_RR_NAPTR_SERVICES);
      dns_set_str(js, obj, "regexp", rr, ARES_RR_NAPTR_REGEXP);
      dns_set_str(js, obj, "replacement", rr, ARES_RR_NAPTR_REPLACEMENT);
      dns_set_num(js, obj, "order", ares_dns_rr_get_u16(rr, ARES_RR_NAPTR_ORDER));
      dns_set_num(js, obj, "preference", ares_dns_rr_get_u16(rr, ARES_RR_NAPTR_PREFERENCE));
      break;

    case DNS_RR_TLSA: {
      size_t len = 0;
      const unsigned char *data = ares_dns_rr_get_bin(rr, ARES_RR_TLSA_DATA, &len);
      obj = Ant_Object_Create(js);
      dns_set_num(js, obj, "certUsage", ares_dns_rr_get_u8(rr, ARES_RR_TLSA_CERT_USAGE));
      dns_set_num(js, obj, "selector", ares_dns_rr_get_u8(rr, ARES_RR_TLSA_SELECTOR));
      dns_set_num(js, obj, "match", ares_dns_rr_get_u8(rr, ARES_RR_TLSA_MATCH));
      js_set(js, obj, "data", create_arraybuffer_copy(js, data, data ? len : 0));
      break;
    }

    default:
      return js_mkundef();
  }

  if (tagged) js_set(js, obj, "type", dns_str(js, dns_rr_info[kind].name));
  return obj;
}

static const dns_rr_t dns_any_order[] = {
  DNS_RR_A, DNS_RR_AAAA, DNS_RR_CNAME, DNS_RR_MX, DNS_RR_NS, DNS_RR_TXT,
  DNS_RR_SRV, DNS_RR_PTR, DNS_RR_NAPTR, DNS_RR_SOA, DNS_RR_TLSA, DNS_RR_CAA,
};

static void dns_collect(ant_t *js, ant_value_t list, const ares_dns_record_t *rec, size_t count, dns_rr_t kind, dns_shape_t shape) {
  for (size_t i = 0; i < count; i++) {
    const ares_dns_rr_t *rr = ares_dns_record_rr_get_const(rec, ARES_SECTION_ANSWER, i);
    if (ares_dns_rr_get_type(rr) != dns_rr_info[kind].type) continue;
    ant_value_t value = dns_rr_value(js, rr, kind, shape);
    if (vtype(value) != kTypeUndefined) js_arr_push(js, list, value);
  }
}

static ant_value_t dns_records(ant_t *js, const ares_dns_record_t *rec, dns_rr_t kind, dns_shape_t shape) {
  size_t count = rec ? ares_dns_record_rr_cnt(rec, ARES_SECTION_ANSWER) : 0;
  GC_ROOT_SAVE(mark, js);
  ant_value_t list = js_mkarr(js);
  GC_ROOT_PIN(js, list);

  if (kind == DNS_RR_ANY) {
    for (size_t i = 0; i < countof(dns_any_order); i++) dns_collect(js, list, rec, count, dns_any_order[i], shape);
  } else dns_collect(js, list, rec, count, kind, shape);

  ant_value_t result = js_arr_len(js, list) == 0 ? js_mkundef()
    : kind == DNS_RR_SOA ? js_arr_get(js, list, 0) : list;
  
  GC_ROOT_RESTORE(js, mark);
  return result;
}

static void dns_query_cb(void *arg, ares_status_t status, size_t timeouts, const ares_dns_record_t *rec) {
  (void)timeouts;
  dns_request_t *req = arg;
  ant_t *js = req->js;
  dns_channel_query_finished(req->channel);

  if (js->destroying) {
    dns_request_free(req);
    return;
  }

  const char *hostname = js_getstr(js, req->hostname, NULL);
  const char *syscall = req->kind == DNS_REQ_REVERSE ? "getHostByAddr" : dns_rr_info[req->rr].syscall;
  if (status != ARES_SUCCESS) {
    dns_request_reject(req, dns_ares_error(js, status, syscall, hostname));
    return;
  }

  dns_shape_t shape = req->rr == DNS_RR_ANY ? DNS_SHAPE_ANY : req->ttl ? DNS_SHAPE_TTL : DNS_SHAPE_PLAIN;
  ant_value_t value = dns_records(js, rec, req->rr, shape);
  if (vtype(value) == kTypeUndefined) {
    dns_request_reject(req, dns_ares_error(js, ARES_ENODATA, syscall, hostname));
    return;
  }

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, value);
  dns_request_resolve(req, value, &value, 1);
  GC_ROOT_RESTORE(js, mark);
}

static ant_value_t dns_query_issue(
  ant_t *js, dns_channel_t *channel, dns_req_kind_t kind, dns_rr_t rr,
  ant_value_t hostname, const char *qname, ant_value_t callback, bool ttl
) {
  if (!channel || !channel->ares) return js_mkerr(js, "DNS channel is not available");

  dns_request_t *req = dns_request_new(js, kind, callback, hostname);
  if (!req) return js_mkerr(js, "out of memory");

  req->rr = rr;
  req->ttl = ttl;
  req->channel = channel;
  req->owner = js_getthis(js);

  ant_value_t result = vtype(req->promise) == kTypePromise ? req->promise : js_mkundef();
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, result);

  bool completed = false;
  req->completed = &completed;
  dns_channel_query_started(channel);
  ares_query_dnsrec(channel->ares, qname, ARES_CLASS_IN, dns_rr_info[rr].type, dns_query_cb, req, NULL);
  
  if (!completed) {
    req->completed = NULL;
    dns_request_result(req);
  }
  
  dns_channel_schedule(channel);

  GC_ROOT_RESTORE(js, mark);
  return result;
}

static int dns_rr_from_name(ant_t *js, ant_value_t value) {
  size_t len = 0;
  const char *name = js_getstr(js, value, &len);
  for (int i = 0; name && i < DNS_RR_COUNT; i++)
    if (strlen(dns_rr_info[i].name) == len && memcmp(dns_rr_info[i].name, name, len) == 0) return i;
  return -1;
}

ant_value_t dns_resolve_impl(ant_t *js, ant_value_t *args, int nargs, int rr, bool promises) {
  ant_value_t hostname = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t second = nargs > 1 ? args[1] : js_mkundef();
  ant_value_t callback = promises ? js_mkundef() : nargs > 2 ? args[2] : js_mkundef();
  bool ttl = false;

  if (rr < 0) {
    if (!promises && is_callable(second)) { rr = DNS_RR_A; callback = second; }
    else if (vtype(second) == kTypeUndefined && promises) rr = DNS_RR_A;
    else if (vtype(second) != kTypeString) return Ant_Error_CreateInvalidArgType(js, "rrtype", "of type string", second);
    else if ((rr = dns_rr_from_name(js, second)) < 0) return Ant_Error_CreateInvalidArgValue(js, "rrtype", NULL, second);
  } else if (!promises && is_callable(second)) callback = second;
  else ttl = is_object_type(second) && js_truthy(js, js_get(js, second, "ttl"));

  if (rr != DNS_RR_A && rr != DNS_RR_AAAA) ttl = false;
  if (vtype(hostname) != kTypeString) return Ant_Error_CreateInvalidArgType(js, "name", "of type string", hostname);
  if (!promises && !is_callable(callback)) return Ant_Error_CreateInvalidArgType(js, "callback", "of type function", callback);

  dns_channel_t *channel = dns_this_channel(js);
  return dns_query_issue(js, channel, DNS_REQ_QUERY, (dns_rr_t)rr, hostname, js_getstr(js, hostname, NULL), callback, ttl);
}

ant_value_t dns_reverse_impl(ant_t *js, ant_value_t *args, int nargs, bool promises) {
  ant_value_t ip = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t callback = promises ? js_mkundef() : nargs > 1 ? args[1] : js_mkundef();
  if (vtype(ip) != kTypeString) return Ant_Error_CreateInvalidArgType(js, "ip", "of type string", ip);
  if (!promises && !is_callable(callback)) return Ant_Error_CreateInvalidArgType(js, "callback", "of type function", callback);

  const char *text = js_getstr(js, ip, NULL);
  struct ares_addr addr = { 0 };
  int family = dns_ip_family(text);

  if (family == 4) {
    addr.family = AF_INET;
    uv_inet_pton(AF_INET, text, &addr.addr.addr4);
  } else if (family == 6) {
    addr.family = AF_INET6;
    uv_inet_pton(AF_INET6, text, &addr.addr.addr6);
  }

  if (!family) {
    ant_value_t error = dns_error(js, "getHostByAddr", "EINVAL", UV_EINVAL, true, text);
    if (!promises) return js_throw(js, error);
    ant_value_t promise = js_mkpromise(js);
    js_reject_promise(js, promise, error);
    return promise;
  }

  char *qname = ares_dns_addr_to_ptr(&addr);
  if (!qname) return js_mkerr(js, "out of memory");

  ant_value_t result = dns_query_issue(js, dns_this_channel(js), DNS_REQ_REVERSE, DNS_RR_PTR, ip, qname, callback, false);
  ares_free_string(qname);

  return result;
}
