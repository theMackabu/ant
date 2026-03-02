// stub: minimal node:dns implementation (dns.promises.lookup)
// just enough for vite to resolve localhost addresses

#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#endif

#include "ant.h"
#include "errors.h"

static jsval_t dns_promises_lookup(ant_t *js, jsval_t *args, int nargs) {
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

  jsval_t result = js_mkobj(js);
  js_set(js, result, "address", js_mkstr(js, addr_str, strlen(addr_str)));
  js_set(js, result, "family", js_mknum(family));

  jsval_t promise = js_mkpromise(js);
  js_resolve_promise(js, promise, result);
  return promise;
}

jsval_t dns_library(ant_t *js) {
  jsval_t lib = js_mkobj(js);
  jsval_t promises = js_mkobj(js);

  js_set(js, promises, "lookup", js_mkfun(dns_promises_lookup));
  js_set(js, lib, "promises", promises);

  return lib;
}
