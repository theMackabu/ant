#include <stdlib.h>
#include <string.h>
#include <ares.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define dns_strncasecmp _strnicmp
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <strings.h>
#define dns_strncasecmp strncasecmp
#endif

#include "internal.h"

#define DNS_CLASS_IN 1
#define DNS_TYPE_A 1
#define DNS_TYPE_TXT 16
#define DNS_TYPE_AAAA 28
#define DNS_TYPE_SRV 33

static ant_value_t dns_promises_lookup(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "hostname is required");

  size_t len;
  const char *hostname = js_getstr(js, args[0], &len);
  if (!hostname) return js_mkerr(js, "hostname must be a string");

  struct addrinfo hints = {0}, *res = NULL;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  int err = getaddrinfo(hostname, NULL, &hints, &res);
  if (err != 0 || !res) {
    return js_mkerr(js, "getaddrinfo failed for '%s'", hostname);
  }

  char addr_str[INET6_ADDRSTRLEN];
  int family = 0;

  if (res->ai_family == AF_INET) {
    struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
    inet_ntop(AF_INET, &sa->sin_addr, addr_str, sizeof(addr_str));
    family = 4;
  } else if (res->ai_family == AF_INET6) {
    struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)res->ai_addr;
    inet_ntop(AF_INET6, &sa6->sin6_addr, addr_str, sizeof(addr_str));
    family = 6;
  } else {
    freeaddrinfo(res);
    return js_mkerr(js, "unsupported address family");
  }

  freeaddrinfo(res);

  ant_value_t result = js_mkobj(js);
  js_set(js, result, "address", js_mkstr(js, addr_str, strlen(addr_str)));
  js_set(js, result, "family", js_mknum(family));

  ant_value_t promise = js_mkpromise(js);
  js_resolve_promise(js, promise, result);
  return promise;
}

typedef struct {
  int done;
  int status;
  unsigned char *abuf;
  int alen;
} dns_query_result_t;

static void dns_query_cb(void *arg, int status, int timeouts, unsigned char *abuf, int alen) {
  (void)timeouts;
  dns_query_result_t *result = (dns_query_result_t *)arg;
  result->done = 1;
  result->status = status;
  result->alen = alen;
  if (abuf && alen > 0) {
    result->abuf = malloc((size_t)alen);
    if (result->abuf) memcpy(result->abuf, abuf, (size_t)alen);
    else result->status = ARES_ENOMEM;
  }
}

static int dns_rrtype_from_value(ant_t *js, ant_value_t value) {
  if (vtype(value) == kTypeUndefined) return DNS_TYPE_A;
  size_t len = 0;
  const char *rrtype = js_getstr(js, value, &len);
  if (!rrtype) return -1;
  if (len == 1 && (rrtype[0] == 'A' || rrtype[0] == 'a')) return DNS_TYPE_A;
  if (len == 4 && dns_strncasecmp(rrtype, "AAAA", 4) == 0) return DNS_TYPE_AAAA;
  if (len == 3 && dns_strncasecmp(rrtype, "SRV", 3) == 0) return DNS_TYPE_SRV;
  if (len == 3 && dns_strncasecmp(rrtype, "TXT", 3) == 0) return DNS_TYPE_TXT;
  return -1;
}

static ant_value_t dns_rejected_promise(ant_t *js, const char *message) {
  ant_value_t promise = js_mkpromise(js);
  js_reject_promise(js, promise, js_mkerr(js, "%s", message));
  return promise;
}

static bool dns_process_query(ares_channel_t *channel, dns_query_result_t *result) {
  while (!result->done) {
    fd_set read_fds, write_fds;
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    int nfds = ares_fds(channel, &read_fds, &write_fds);
    if (nfds == 0) break;

    struct timeval tv, *tvp = ares_timeout(channel, NULL, &tv);
    int rc = select(nfds, &read_fds, &write_fds, NULL, tvp);
    if (rc < 0) return false;
    ares_process(channel, &read_fds, &write_fds);
  }
  return result->done;
}

static ant_value_t dns_parse_a(ant_t *js, const unsigned char *abuf, int alen) {
  struct hostent *host = NULL;
  int status = ares_parse_a_reply(abuf, alen, &host, NULL, NULL);
  if (status != ARES_SUCCESS) return js_mkerr(js, "DNS A parse failed: %s", ares_strerror(status));

  ant_value_t arr = js_mkarr(js);
  for (char **addr = host->h_addr_list; addr && *addr; addr++) {
    char ip[INET_ADDRSTRLEN];
    if (ares_inet_ntop(AF_INET, *addr, ip, sizeof(ip)))
      js_arr_push(js, arr, js_mkstr(js, ip, strlen(ip)));
  }
  ares_free_hostent(host);
  return arr;
}

static ant_value_t dns_parse_aaaa(ant_t *js, const unsigned char *abuf, int alen) {
  struct hostent *host = NULL;
  int status = ares_parse_aaaa_reply(abuf, alen, &host, NULL, NULL);
  if (status != ARES_SUCCESS) return js_mkerr(js, "DNS AAAA parse failed: %s", ares_strerror(status));

  ant_value_t arr = js_mkarr(js);
  for (char **addr = host->h_addr_list; addr && *addr; addr++) {
    char ip[INET6_ADDRSTRLEN];
    if (ares_inet_ntop(AF_INET6, *addr, ip, sizeof(ip)))
      js_arr_push(js, arr, js_mkstr(js, ip, strlen(ip)));
  }
  ares_free_hostent(host);
  return arr;
}

static ant_value_t dns_parse_srv(ant_t *js, const unsigned char *abuf, int alen) {
  struct ares_srv_reply *srv = NULL;
  int status = ares_parse_srv_reply(abuf, alen, &srv);
  if (status != ARES_SUCCESS) return js_mkerr(js, "DNS SRV parse failed: %s", ares_strerror(status));

  ant_value_t arr = js_mkarr(js);
  for (struct ares_srv_reply *cur = srv; cur; cur = cur->next) {
    ant_value_t record = js_mkobj(js);
    js_set(js, record, "name", js_mkstr(js, cur->host, strlen(cur->host)));
    js_set(js, record, "port", js_mknum((double)cur->port));
    js_set(js, record, "priority", js_mknum((double)cur->priority));
    js_set(js, record, "weight", js_mknum((double)cur->weight));
    js_arr_push(js, arr, record);
  }
  ares_free_data(srv);
  return arr;
}

static ant_value_t dns_parse_txt(ant_t *js, const unsigned char *abuf, int alen) {
  struct ares_txt_reply *txt = NULL;
  int status = ares_parse_txt_reply(abuf, alen, &txt);
  if (status != ARES_SUCCESS) return js_mkerr(js, "DNS TXT parse failed: %s", ares_strerror(status));

  ant_value_t arr = js_mkarr(js);
  for (struct ares_txt_reply *cur = txt; cur; cur = cur->next) {
    ant_value_t chunks = js_mkarr(js);
    js_arr_push(js, chunks, js_mkstr(js, cur->txt, cur->length));
    js_arr_push(js, arr, chunks);
  }
  ares_free_data(txt);
  return arr;
}

static ant_value_t dns_promises_resolve(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return dns_rejected_promise(js, "hostname is required");

  size_t len = 0;
  const char *hostname = js_getstr(js, args[0], &len);
  if (!hostname) return dns_rejected_promise(js, "hostname must be a string");

  int rrtype = dns_rrtype_from_value(js, nargs >= 2 ? args[1] : js_mkundef());
  if (rrtype < 0) return dns_rejected_promise(js, "unsupported DNS record type");

  int init_status = ares_library_init(ARES_LIB_INIT_ALL);
  if (init_status != ARES_SUCCESS)
    return dns_rejected_promise(js, ares_strerror(init_status));

  ares_channel_t *channel = NULL;
  int status = ares_init(&channel);
  if (status != ARES_SUCCESS) {
    ares_library_cleanup();
    return dns_rejected_promise(js, ares_strerror(status));
  }

  dns_query_result_t result = {0};
  ares_query(channel, hostname, DNS_CLASS_IN, rrtype, dns_query_cb, &result);
  if (!dns_process_query(channel, &result) && result.status == ARES_SUCCESS)
    result.status = ARES_ETIMEOUT;

  ant_value_t promise = js_mkpromise(js);
  if (result.status != ARES_SUCCESS) {
    js_reject_promise(js, promise, js_mkerr(js, "%s", ares_strerror(result.status)));
  } else {
    ant_value_t records;
    if (rrtype == DNS_TYPE_A) records = dns_parse_a(js, result.abuf, result.alen);
    else if (rrtype == DNS_TYPE_AAAA) records = dns_parse_aaaa(js, result.abuf, result.alen);
    else if (rrtype == DNS_TYPE_SRV) records = dns_parse_srv(js, result.abuf, result.alen);
    else records = dns_parse_txt(js, result.abuf, result.alen);

    if (is_err(records)) js_reject_promise(js, promise, records);
    else js_resolve_promise(js, promise, records);
  }

  free(result.abuf);
  ares_destroy(channel);
  ares_library_cleanup();
  
  return promise;
}

ant_value_t dns_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);
  ant_value_t promises = js_mkobj(js);

  js_set(js, promises, "lookup", js_mkfun(dns_promises_lookup));
  js_set(js, promises, "resolve", js_mkfun(dns_promises_resolve));
  js_set(js, lib, "promises", promises);

  return lib;
}
