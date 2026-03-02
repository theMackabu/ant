// stub: minimal node:net implementation (isIP, isIPv4, isIPv6)
// just enough for vite to resolve the module and validate hostnames

#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include "ant.h"
#include "internal.h" // IWYU pragma: keep

static jsval_t net_isIP(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mknum(0);
  size_t len;
  const char *host = js_getstr(js, args[0], &len);
  if (!host) return js_mknum(0);

  struct in_addr addr4;
  struct in6_addr addr6;

  if (inet_pton(AF_INET, host, &addr4) == 1) return js_mknum(4);
  if (inet_pton(AF_INET6, host, &addr6) == 1) return js_mknum(6);
  return js_mknum(0);
}

static jsval_t net_isIPv4(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_false;
  size_t len;
  const char *host = js_getstr(js, args[0], &len);
  if (!host) return js_false;

  struct in_addr addr;
  return inet_pton(AF_INET, host, &addr) == 1 ? js_true : js_false;
}

static jsval_t net_isIPv6(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_false;
  size_t len;
  const char *host = js_getstr(js, args[0], &len);
  if (!host) return js_false;

  struct in6_addr addr;
  return inet_pton(AF_INET6, host, &addr) == 1 ? js_true : js_false;
}

jsval_t net_library(ant_t *js) {
  jsval_t lib = js_mkobj(js);

  js_set(js, lib, "isIP", js_mkfun(net_isIP));
  js_set(js, lib, "isIPv4", js_mkfun(net_isIPv4));
  js_set(js, lib, "isIPv6", js_mkfun(net_isIPv6));

  return lib;
}
