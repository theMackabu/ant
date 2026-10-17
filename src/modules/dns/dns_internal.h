#ifndef ANT_DNS_INTERNAL_H
#define ANT_DNS_INTERNAL_H

#include <ares.h>
#include <uv.h>
#include <stdbool.h>

#include "types.h"

typedef enum {
  DNS_ORDER_VERBATIM,
  DNS_ORDER_IPV4_FIRST,
  DNS_ORDER_IPV6_FIRST,
} dns_order_t;

typedef enum {
#define DNS_RECORD_TYPE(name, method, syscall, ares) DNS_RR_##name,
#include "dns/record_types.h"
  DNS_RR_COUNT,
} dns_rr_t;

typedef struct {
  const char *name;
  const char *method;
  const char *syscall;
  ares_dns_rec_type_t type;
} dns_rr_info_t;

extern const dns_rr_info_t dns_rr_info[DNS_RR_COUNT];

typedef struct dns_poll_s {
  struct dns_poll_s *next;
  dns_channel_t *channel;
  uv_poll_t handle;
  ares_socket_t fd;
} dns_poll_t;

struct dns_channel_s {
  dns_channel_t *next;
  dns_channel_t **pprev;
  ant_t *js;
  ares_channel_t *ares;
  dns_channel_t **owner_ref;
  dns_poll_t *polls;
  uv_timer_t timer;
  int queries;
  int open_handles;
  bool destroyed;
};

typedef enum {
  DNS_REQ_LOOKUP,
  DNS_REQ_LOOKUP_SERVICE,
  DNS_REQ_QUERY,
  DNS_REQ_REVERSE,
} dns_req_kind_t;

struct dns_request_s {
  dns_request_t *next;
  dns_request_t **pprev;
  ant_t *js;
  ant_value_t promise;
  ant_value_t callback;
  ant_value_t hostname;
  ant_value_t owner;
  dns_channel_t *channel;
  union {
    uv_getaddrinfo_t getaddrinfo;
    uv_getnameinfo_t getnameinfo;
  } uv;
  dns_req_kind_t kind;
  dns_rr_t rr;
  dns_order_t order;
  int family;
  bool *completed;
  bool all;
  bool ttl;
  bool issuing;
};

int dns_ip_family(const char *address);

void dns_request_free(dns_request_t *req);
void dns_channel_destroy(dns_channel_t *channel);
void dns_channel_schedule(dns_channel_t *channel);
void dns_channel_query_started(dns_channel_t *channel);
void dns_channel_query_finished(dns_channel_t *channel);

void dns_request_reject(dns_request_t *req, ant_value_t error);
void dns_request_resolve(dns_request_t *req, ant_value_t value, const ant_value_t *cb_args, int cb_argc);

dns_channel_t *dns_this_channel(ant_t *js);
dns_channel_t *dns_default_channel(ant_t *js);
dns_channel_t *dns_channel_create(ant_t *js, int timeout_ms, int tries);
dns_request_t *dns_request_new(ant_t *js, dns_req_kind_t kind, ant_value_t callback, ant_value_t hostname);

ant_value_t dns_request_result(dns_request_t *req);
ant_value_t dns_error(ant_t *js, const char *syscall, const char *code, int errno_value, bool has_errno, const char *hostname);
ant_value_t dns_ares_error(ant_t *js, int status, const char *syscall, const char *hostname);
ant_value_t dns_uv_error(ant_t *js, int status, const char *syscall, const char *hostname);
ant_value_t dns_parse_result_order(ant_t *js, ant_value_t value, dns_order_t *out);
ant_value_t dns_lookup_impl(ant_t *js, ant_value_t *args, int nargs, bool promises);
ant_value_t dns_lookup_service_impl(ant_t *js, ant_value_t *args, int nargs, bool promises);
ant_value_t dns_resolve_impl(ant_t *js, ant_value_t *args, int nargs, int rr, bool promises);
ant_value_t dns_reverse_impl(ant_t *js, ant_value_t *args, int nargs, bool promises);
ant_value_t dns_channel_get_servers(ant_t *js, dns_channel_t *channel);
ant_value_t dns_channel_set_servers(ant_t *js, dns_channel_t *channel, ant_value_t servers);

#endif
