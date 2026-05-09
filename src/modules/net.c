#include <compat.h> // IWYU pragma: keep

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include "ant.h"
#include "ptr.h"
#include "errors.h"
#include "internal.h"

#include "gc/roots.h"
#include "gc/modules.h"
#include "net/connection.h"
#include "net/listener.h"
#include "silver/engine.h"

#include "modules/buffer.h"
#include "modules/events.h"
#include "modules/net.h"
#include "modules/symbol.h"

typedef struct net_server_s net_server_t;
typedef struct net_socket_s net_socket_t;

typedef struct net_listen_args_s net_listen_args_t;
typedef struct net_connect_args_s net_connect_args_t;
typedef struct net_write_args_s  net_write_args_t;

struct net_socket_s {
  ant_t *js;
  ant_value_t obj;
  ant_conn_t *conn;
  ant_listener_t client_listener;
  net_server_t *server;
  ant_value_t encoding;
  struct net_socket_s *next_active;
  struct net_socket_s *next_in_server;
  bool allow_half_open;
  bool destroyed;
  bool connecting;
  bool had_error;
  bool active;
};

struct net_server_s {
  ant_t *js;
  ant_value_t obj;
  ant_listener_t listener;
  net_socket_t *sockets;
  struct net_server_s *next_active;
  char *host;
  char *path;
  int port;
  int backlog;
  int max_connections;
  bool listening;
  bool closing;
  bool allow_half_open;
  bool pause_on_connect;
  bool no_delay;
  bool keep_alive;
  unsigned int keep_alive_initial_delay_secs;
};

struct net_listen_args_s {
  const char *host;
  const char *path;
  int port;
  int backlog;
  ant_value_t callback;
  ant_value_t error;
};

struct net_connect_args_s {
  const char *host;
  const char *path;
  int port;
  uint64_t timeout_ms;
  bool allow_half_open;
  bool no_delay;
  bool keep_alive;
  unsigned int keep_alive_initial_delay_secs;
  ant_value_t callback;
  ant_value_t error;
};

struct net_write_args_s {
  const uint8_t *bytes;
  size_t len;
  ant_value_t callback;
  ant_value_t error;
};

static ant_value_t g_net_server_proto = 0;
static ant_value_t g_net_socket_proto = 0;
static ant_value_t g_net_server_ctor = 0;
static ant_value_t g_net_socket_ctor = 0;

static net_server_t *g_active_servers = NULL;
static net_socket_t *g_active_sockets = NULL;

static bool g_default_auto_select_family = true;
static double g_default_auto_select_family_attempt_timeout = 250.0;

enum {
  NET_SERVER_NATIVE_TAG = 0x4e455453u, // NETS
  NET_SOCKET_NATIVE_TAG = 0x4e45544bu, // NETK
};

static net_server_t *net_server_data(ant_value_t value) {
  return (net_server_t *)js_get_native(value, NET_SERVER_NATIVE_TAG);
}

static net_socket_t *net_socket_data(ant_value_t value) {
  return (net_socket_t *)js_get_native(value, NET_SOCKET_NATIVE_TAG);
}

static void net_add_active_server(net_server_t *server) {
  server->next_active = g_active_servers;
  g_active_servers = server;
}

static void net_remove_active_server(net_server_t *server) {
  net_server_t **it = NULL;
  for (it = &g_active_servers; *it; it = &(*it)->next_active) {
  if (*it == server) {
    *it = server->next_active;
    server->next_active = NULL;
    return;
  }}
}

static void net_add_active_socket(net_socket_t *socket) {
  if (!socket || socket->active) return;
  socket->active = true;
  socket->next_active = g_active_sockets;
  g_active_sockets = socket;
}

static void net_remove_active_socket(net_socket_t *socket) {
  net_socket_t **it = NULL;
  for (it = &g_active_sockets; *it; it = &(*it)->next_active) {
  if (*it == socket) {
    *it = socket->next_active;
    socket->next_active = NULL;
    socket->active = false;
    return;
  }}
}

static ant_value_t net_call_value(
  ant_t *js,
  ant_value_t fn,
  ant_value_t this_val,
  ant_value_t *args,
  int nargs
) {
  ant_value_t saved_this = js->this_val;
  ant_value_t result = js_mkundef();

  js->this_val = this_val;
  if (vtype(fn) == T_CFUNC) result = js_as_cfunc(fn)(js, args, nargs);
  else result = sv_vm_call(js->vm, js, fn, this_val, args, nargs, NULL, false);
  js->this_val = saved_this;
  return result;
}

static bool net_emit(ant_t *js, ant_value_t target, const char *event, ant_value_t *args, int nargs) {
  return eventemitter_emit_args(js, target, event, args, nargs);
}

static bool net_add_listener(
  ant_t *js,
  ant_value_t target,
  const char *event,
  ant_value_t listener,
  bool once
) {
  return eventemitter_add_listener(js, target, event, listener, once);
}

static net_server_t *net_require_server(ant_t *js, ant_value_t this_val) {
  net_server_t *server = net_server_data(this_val);
  if (!server) {
    js->thrown_exists = true;
    js->thrown_value = js_mkerr_typed(js, JS_ERR_TYPE, "Invalid net.Server");
    return NULL;
  }
  return server;
}

static net_socket_t *net_require_socket(ant_t *js, ant_value_t this_val) {
  net_socket_t *socket = net_socket_data(this_val);
  if (!socket) {
    js->thrown_exists = true;
    js->thrown_value = js_mkerr_typed(js, JS_ERR_TYPE, "Invalid net.Socket");
    return NULL;
  }
  return socket;
}

static bool net_parse_write_args(ant_t *js, ant_value_t *args, int nargs, net_write_args_t *out) {
  ant_value_t value = 0;

  if (!out) return false;
  memset(out, 0, sizeof(*out));
  out->callback = js_mkundef();
  out->error = js_mkundef();

  if (nargs < 1 || vtype(args[0]) == T_UNDEF || vtype(args[0]) == T_NULL) return true;
  value = args[0];

  if (!buffer_source_get_bytes(js, value, &out->bytes, &out->len)) {
    size_t slen = 0;
    ant_value_t str_val = js_tostring_val(js, value);
    const char *str = NULL;

    if (is_err(str_val)) {
      out->error = str_val;
      return false;
    }

    str = js_getstr(js, str_val, &slen);
    if (!str) {
      out->error = js_mkerr_typed(js, JS_ERR_TYPE, "Invalid socket write data");
      return false;
    }

    out->bytes = (const uint8_t *)str;
    out->len = slen;
  }

  if (nargs > 1 && is_callable(args[1])) out->callback = args[1];
  else if (nargs > 2 && is_callable(args[2])) out->callback = args[2];
  return true;
}

static bool net_parse_listen_args(ant_t *js, ant_value_t *args, int nargs, net_listen_args_t *out) {
  if (!out) return false;
  
  memset(out, 0, sizeof(*out));
  out->host = "0.0.0.0";
  out->backlog = 511;
  out->callback = js_mkundef();
  out->error = js_mkundef();

  if (nargs == 0) return true;

  if (vtype(args[0]) == T_NUM) {
    out->port = (int)js_getnum(args[0]);
    if (nargs > 1 && vtype(args[1]) == T_STR) {
      size_t len = 0;
      out->host = js_getstr(js, args[1], &len);
    }
    if (nargs > 2 && vtype(args[2]) == T_NUM) out->backlog = (int)js_getnum(args[2]);
    if (nargs > 3 && is_callable(args[3])) out->callback = args[3];
    else if (nargs > 2 && is_callable(args[2])) out->callback = args[2];
    else if (nargs > 1 && is_callable(args[1])) out->callback = args[1];
    return true;
  }

  if (vtype(args[0]) == T_OBJ) {
    ant_value_t value = js_get(js, args[0], "path");
    if (vtype(value) == T_STR) {
      out->path = js_getstr(js, value, NULL);
    }

    value = js_get(js, args[0], "port");
    if (vtype(value) == T_NUM) out->port = (int)js_getnum(value);
    value = js_get(js, args[0], "host");
    if (vtype(value) == T_STR) {
      size_t len = 0;
      out->host = js_getstr(js, value, &len);
    }
    
    value = js_get(js, args[0], "backlog");
    if (vtype(value) == T_NUM) out->backlog = (int)js_getnum(value);
    if (nargs > 1 && is_callable(args[1])) out->callback = args[1];
    
    return true;
  }

  if (vtype(args[0]) == T_STR) {
    out->path = js_getstr(js, args[0], NULL);
    if (nargs > 1 && vtype(args[1]) == T_NUM) out->backlog = (int)js_getnum(args[1]);
    if (nargs > 2 && is_callable(args[2])) out->callback = args[2];
    else if (nargs > 1 && is_callable(args[1])) out->callback = args[1];
    return true;
  }

  if (is_callable(args[0])) {
    out->callback = args[0];
    return true;
  }

  return true;
}

static bool net_parse_connect_args(ant_t *js, ant_value_t *args, int nargs, net_connect_args_t *out) {
  ant_value_t value = 0;

  if (!out) return false;
  memset(out, 0, sizeof(*out));
  out->host = "localhost";
  out->callback = js_mkundef();
  out->error = js_mkundef();
  out->no_delay = true;

  if (nargs == 0) {
    out->error = js_mkerr_typed(js, JS_ERR_TYPE, "net.connect requires options, port, or path");
    return false;
  }

  if (vtype(args[0]) == T_NUM) {
    out->port = (int)js_getnum(args[0]);
    if (nargs > 1 && vtype(args[1]) == T_STR) out->host = js_getstr(js, args[1], NULL);
    if (nargs > 2 && is_callable(args[2])) out->callback = args[2];
    else if (nargs > 1 && is_callable(args[1])) out->callback = args[1];
    return true;
  }

  if (vtype(args[0]) == T_STR) {
    out->path = js_getstr(js, args[0], NULL);
    if (nargs > 1 && is_callable(args[1])) out->callback = args[1];
    return true;
  }

  if (vtype(args[0]) == T_OBJ) {
    value = js_get(js, args[0], "path");
    if (vtype(value) == T_STR) out->path = js_getstr(js, value, NULL);

    value = js_get(js, args[0], "socketPath");
    if (!out->path && vtype(value) == T_STR) out->path = js_getstr(js, value, NULL);

    value = js_get(js, args[0], "port");
    if (vtype(value) == T_NUM) out->port = (int)js_getnum(value);

    value = js_get(js, args[0], "host");
    if (vtype(value) == T_STR) out->host = js_getstr(js, value, NULL);

    value = js_get(js, args[0], "hostname");
    if (vtype(value) == T_STR) out->host = js_getstr(js, value, NULL);

    value = js_get(js, args[0], "allowHalfOpen");
    if (vtype(value) != T_UNDEF) out->allow_half_open = js_truthy(js, value);

    value = js_get(js, args[0], "timeout");
    if (vtype(value) == T_NUM && js_getnum(value) > 0) out->timeout_ms = (uint64_t)js_getnum(value);

    value = js_get(js, args[0], "noDelay");
    if (vtype(value) != T_UNDEF) out->no_delay = js_truthy(js, value);

    value = js_get(js, args[0], "keepAlive");
    if (vtype(value) != T_UNDEF) out->keep_alive = js_truthy(js, value);

    value = js_get(js, args[0], "keepAliveInitialDelay");
    if (vtype(value) == T_NUM && js_getnum(value) > 0)
      out->keep_alive_initial_delay_secs = (unsigned int)(js_getnum(value) / 1000.0);

    if (nargs > 1 && is_callable(args[1])) out->callback = args[1];
    return true;
  }

  if (is_callable(args[0])) {
    out->callback = args[0];
    return true;
  }

  out->error = js_mkerr_typed(js, JS_ERR_TYPE, "Invalid net.connect options");
  return false;
}

static ant_value_t net_make_buffer_chunk(ant_t *js, const char *data, size_t len) {
  ArrayBufferData *ab = create_array_buffer_data(len);
  if (!ab) return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
  if (len > 0 && data) memcpy(ab->data, data, len);
  return create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, len, "Buffer");
}

static void net_socket_sync_state(net_socket_t *socket) {
  ant_value_t obj = 0;
  const char *ready_state = "open";

  if (!socket) return;
  obj = socket->obj;
  if (!is_object_type(obj)) return;

  if (socket->destroyed) ready_state = "closed";
  else if (socket->connecting) ready_state = "opening";
  js_set(socket->js, obj, "destroyed", js_bool(socket->destroyed));
  js_set(socket->js, obj, "pending", js_bool(socket->conn == NULL && !socket->destroyed));
  js_set(socket->js, obj, "connecting", js_bool(socket->connecting));
  js_set(socket->js, obj, "readyState", js_mkstr(socket->js, ready_state, strlen(ready_state)));
  js_set(socket->js, obj, "bytesRead", js_mknum((double)(socket->conn ? ant_conn_bytes_read(socket->conn) : 0)));
  js_set(socket->js, obj, "bytesWritten", js_mknum((double)(socket->conn ? ant_conn_bytes_written(socket->conn) : 0)));
  js_set(socket->js, obj, "timeout", js_mknum((double)(socket->conn ? ant_conn_timeout_ms(socket->conn) : 0)));
}

static void net_server_sync_state(net_server_t *server) {
  if (!server || !is_object_type(server->obj)) return;
  js_set(server->js, server->obj, "listening", js_bool(server->listening));
  js_set(server->js, server->obj, "maxConnections", js_mknum((double)server->max_connections));
  js_set(server->js, server->obj, "dropMaxConnection", js_false);
}

static int net_server_socket_count(net_server_t *server) {
  int count = 0;
  net_socket_t *socket = NULL;

  for (socket = server ? server->sockets : NULL; socket; socket = socket->next_in_server)
    count++;
  return count;
}

static void net_server_maybe_finish_close(net_server_t *server) {
  if (!server || !server->closing) return;
  if (!ant_listener_is_closed(&server->listener)) return;
  if (server->sockets) return;

  server->closing = false;
  server->listening = false;
  net_server_sync_state(server);
  net_emit(server->js, server->obj, "close", NULL, 0);
  net_remove_active_server(server);
}

static void net_socket_detach(net_socket_t *socket) {
  if (!socket) return;

  if (socket->server) {
    net_socket_t **it = NULL;
    for (it = &socket->server->sockets; *it; it = &(*it)->next_in_server) if (*it == socket) {
      *it = socket->next_in_server;
      break;
    }
    socket->next_in_server = NULL;
  }

  net_remove_active_socket(socket);
  if (is_object_type(socket->obj))
    js_clear_native(socket->obj, NET_SOCKET_NATIVE_TAG);
  
  net_server_maybe_finish_close(socket->server);
  free(socket);
}

static void net_socket_emit_error(net_socket_t *socket, const char *message) {
  ant_value_t arg = js_mkerr_typed(socket->js, JS_ERR_TYPE, "%s", message);
  socket->had_error = true;
  net_emit(socket->js, socket->obj, "error", &arg, 1);
}

static net_socket_t *net_socket_create(ant_t *js, bool allow_half_open) {
  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, g_net_socket_proto);
  net_socket_t *socket = calloc(1, sizeof(*socket));

  if (!socket) return NULL;
  if (is_object_type(proto)) js_set_proto_init(obj, proto);

  socket->js = js;
  socket->obj = obj;
  socket->encoding = js_mkundef();
  socket->allow_half_open = allow_half_open;

  js_set_native(obj, socket, NET_SOCKET_NATIVE_TAG);
  js_set(js, obj, "remoteAddress", js_mkundef());
  js_set(js, obj, "remotePort", js_mkundef());
  js_set(js, obj, "remoteFamily", js_mkundef());
  js_set(js, obj, "localAddress", js_mkundef());
  js_set(js, obj, "localPort", js_mkundef());
  js_set(js, obj, "localFamily", js_mkundef());
  net_socket_sync_state(socket);
  
  return socket;
}

static void net_socket_attach_conn(net_socket_t *socket, ant_conn_t *conn) {
  if (!socket || !conn) return;

  socket->conn = conn;
  ant_conn_set_user_data(conn, socket);
  if (ant_conn_has_remote_addr(conn)) {
    js_set(socket->js, socket->obj, "remoteAddress", js_mkstr(socket->js, ant_conn_remote_addr(conn), strlen(ant_conn_remote_addr(conn))));
    js_set(socket->js, socket->obj, "remotePort", js_mknum(ant_conn_remote_port(conn)));
    js_set(socket->js, socket->obj, "remoteFamily", js_mkstr(socket->js, ant_conn_remote_family(conn), strlen(ant_conn_remote_family(conn))));
  }
  if (ant_conn_has_local_addr(conn)) {
    js_set(socket->js, socket->obj, "localAddress", js_mkstr(socket->js, ant_conn_local_addr(conn), strlen(ant_conn_local_addr(conn))));
    js_set(socket->js, socket->obj, "localPort", js_mknum(ant_conn_local_port(conn)));
    js_set(socket->js, socket->obj, "localFamily", js_mkstr(socket->js, ant_conn_local_family(conn), strlen(ant_conn_local_family(conn))));
  }
  net_socket_sync_state(socket);
}

static net_server_t *net_server_create(ant_t *js) {
  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, g_net_server_proto);
  net_server_t *server = calloc(1, sizeof(*server));

  if (!server) return NULL;
  if (is_object_type(proto)) js_set_proto_init(obj, proto);

  server->js = js;
  server->obj = obj;
  server->host = strdup("0.0.0.0");
  server->backlog = 511;
  if (!server->host) {
    free(server);
    return NULL;
  }

  js_set_native(obj, server, NET_SERVER_NATIVE_TAG);
  net_server_sync_state(server);
  return server;
}

static ant_value_t net_isIP(ant_t *js, ant_value_t *args, int nargs) {
  size_t len = 0;
  const char *host = NULL;
  struct in_addr addr4;
  struct in6_addr addr6;

  if (nargs < 1) return js_mknum(0);
  host = js_getstr(js, args[0], &len);
  if (!host) return js_mknum(0);

  if (inet_pton(AF_INET, host, &addr4) == 1) return js_mknum(4);
  if (inet_pton(AF_INET6, host, &addr6) == 1) return js_mknum(6);
  return js_mknum(0);
}

static ant_value_t net_isIPv4(ant_t *js, ant_value_t *args, int nargs) {
  if (js_getnum(net_isIP(js, args, nargs)) == 4.0) return js_true;
  return js_false;
}

static ant_value_t net_isIPv6(ant_t *js, ant_value_t *args, int nargs) {
  if (js_getnum(net_isIP(js, args, nargs)) == 6.0) return js_true;
  return js_false;
}

static void net_server_apply_options(ant_t *js, net_server_t *server, ant_value_t options) {
  ant_value_t value = 0;

  if (!server || vtype(options) != T_OBJ) return;

  value = js_get(js, options, "allowHalfOpen");
  if (vtype(value) != T_UNDEF) server->allow_half_open = js_truthy(js, value);

  value = js_get(js, options, "pauseOnConnect");
  if (vtype(value) != T_UNDEF) server->pause_on_connect = js_truthy(js, value);

  value = js_get(js, options, "noDelay");
  if (vtype(value) != T_UNDEF) server->no_delay = js_truthy(js, value);

  value = js_get(js, options, "keepAlive");
  if (vtype(value) != T_UNDEF) server->keep_alive = js_truthy(js, value);

  value = js_get(js, options, "keepAliveInitialDelay");
  if (vtype(value) == T_NUM && js_getnum(value) > 0)
    server->keep_alive_initial_delay_secs = (unsigned int)(js_getnum(value) / 1000.0);

  value = js_get(js, options, "backlog");
  if (vtype(value) == T_NUM && js_getnum(value) > 0)
    server->backlog = (int)js_getnum(value);
}

static void net_socket_on_read(ant_conn_t *conn, ssize_t nread, void *user_data) {
  net_socket_t *socket = (net_socket_t *)ant_conn_get_user_data(conn);
  const char *buffer = NULL;
  ant_value_t chunk = 0;
  size_t total = 0;
  size_t offset = 0;

  if (!socket || !conn || nread <= 0) return;

  total = ant_conn_buffer_len(conn);
  if ((size_t)nread > total) return;
  offset = total - (size_t)nread;
  buffer = ant_conn_buffer(conn) + offset;

  if (vtype(socket->encoding) == T_STR)
    chunk = js_mkstr(socket->js, buffer, (size_t)nread);
  else chunk = net_make_buffer_chunk(socket->js, buffer, (size_t)nread);

  ant_conn_consume(conn, total);
  net_socket_sync_state(socket);
  net_emit(socket->js, socket->obj, "data", &chunk, 1);
}

static void net_socket_on_end(ant_conn_t *conn, void *user_data) {
  net_socket_t *socket = (net_socket_t *)ant_conn_get_user_data(conn);

  if (!socket) return;
  net_emit(socket->js, socket->obj, "end", NULL, 0);
  if (!socket->allow_half_open && socket->conn) ant_conn_shutdown(socket->conn);
}

static void net_socket_on_error(ant_conn_t *conn, int status, void *user_data) {
  net_socket_t *socket = (net_socket_t *)ant_conn_get_user_data(conn);

  if (!socket) return;
  socket->had_error = true; {
    ant_value_t err = js_mkerr_typed(socket->js, JS_ERR_TYPE, "%s", uv_strerror(status));
    net_emit(socket->js, socket->obj, "error", &err, 1);
  }
}

static void net_socket_on_timeout(ant_conn_t *conn, void *user_data) {
  net_socket_t *socket = (net_socket_t *)ant_conn_get_user_data(conn);
  if (!socket) return;
  net_emit(socket->js, socket->obj, "timeout", NULL, 0);
}

static void net_server_on_conn_close(ant_conn_t *conn, void *user_data) {
  net_socket_t *socket = (net_socket_t *)ant_conn_get_user_data(conn);
  ant_value_t arg = 0;

  if (!socket) return;
  arg = js_bool(socket->had_error);
  socket->conn = NULL;
  socket->destroyed = true;
  net_socket_sync_state(socket);
  net_emit(socket->js, socket->obj, "close", &arg, 1);
  ant_conn_set_user_data(conn, NULL);
  net_socket_detach(socket);
}

static void net_socket_on_connect(ant_conn_t *conn, int status, void *user_data) {
  net_socket_t *socket = (net_socket_t *)user_data;

  if (!socket) return;
  socket->connecting = false;

  if (status != 0) {
    socket->had_error = true;
    net_socket_sync_state(socket);
    net_socket_on_error(conn, status, user_data);
    if (conn) ant_conn_close(conn);
    return;
  }

  net_socket_attach_conn(socket, conn);
  ant_conn_start(conn);
  net_emit(socket->js, socket->obj, "connect", NULL, 0);
}

static void net_server_on_listener_close(ant_listener_t *listener, void *user_data) {
  net_server_maybe_finish_close((net_server_t *)user_data);
}

static void net_server_on_accept(ant_listener_t *listener, ant_conn_t *conn, void *user_data) {
  net_server_t *server = (net_server_t *)user_data;
  net_socket_t *socket = NULL;
  ant_value_t arg = 0;

  if (!server || !conn) return;

  if (server->max_connections > 0 && net_server_socket_count(server) >= server->max_connections) {
    ant_conn_close(conn);
    return;
  }

  socket = net_socket_create(server->js, server->allow_half_open);
  if (!socket) {
    ant_conn_close(conn);
    return;
  }

  socket->server = server;
  socket->next_in_server = server->sockets;
  server->sockets = socket;
  net_add_active_socket(socket);
  net_socket_attach_conn(socket, conn);

  if (server->no_delay) ant_conn_set_no_delay(conn, true);
  if (server->keep_alive)
    ant_conn_set_keep_alive(conn, true, server->keep_alive_initial_delay_secs);
  if (server->pause_on_connect) ant_conn_pause_read(conn);

  arg = socket->obj;
  net_emit(server->js, server->obj, "connection", &arg, 1);
}

static bool net_server_parse_host(const char *input, const char **out) {
  if (!input || !*input) {
    *out = "0.0.0.0";
    return true;
  }
  *out = input;
  return true;
}

static ant_value_t js_net_socket_ctor(ant_t *js, ant_value_t *args, int nargs) {
  net_socket_t *socket = NULL;
  bool allow_half_open = false;

  if (nargs > 0 && vtype(args[0]) == T_OBJ) {
    ant_value_t value = js_get(js, args[0], "allowHalfOpen");
    allow_half_open = js_truthy(js, value);
  }

  socket = net_socket_create(js, allow_half_open);
  if (!socket) return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
  return socket->obj;
}

static ant_value_t js_net_socket_address(ant_t *js, ant_value_t *args, int nargs) {
  net_socket_t *socket = net_require_socket(js, js_getthis(js));
  ant_value_t out = js_mkobj(js);

  if (!socket) return js->thrown_value;
  if (!socket || !socket->conn || !ant_conn_has_local_addr(socket->conn)) return out;
  js_set(js, out, "address", js_mkstr(js, ant_conn_local_addr(socket->conn), strlen(ant_conn_local_addr(socket->conn))));
  js_set(js, out, "port", js_mknum(ant_conn_local_port(socket->conn)));
  js_set(js, out, "family", js_mkstr(js, ant_conn_local_family(socket->conn), strlen(ant_conn_local_family(socket->conn))));
  return out;
}

static ant_value_t js_net_socket_pause(ant_t *js, ant_value_t *args, int nargs) {
  net_socket_t *socket = net_require_socket(js, js_getthis(js));
  if (!socket) return js->thrown_value;
  if (socket && socket->conn) ant_conn_pause_read(socket->conn);
  return js_getthis(js);
}

static ant_value_t js_net_socket_resume(ant_t *js, ant_value_t *args, int nargs) {
  net_socket_t *socket = net_require_socket(js, js_getthis(js));
  if (!socket) return js->thrown_value;
  if (socket && socket->conn) ant_conn_resume_read(socket->conn);
  return js_getthis(js);
}

static ant_value_t js_net_socket_setEncoding(ant_t *js, ant_value_t *args, int nargs) {
  net_socket_t *socket = net_require_socket(js, js_getthis(js));
  if (!socket) return js->thrown_value;
  socket->encoding = nargs > 0 && vtype(args[0]) != T_UNDEF ? js_tostring_val(js, args[0]) : js_mkundef();
  return js_getthis(js);
}

static ant_value_t js_net_socket_setTimeout(ant_t *js, ant_value_t *args, int nargs) {
  net_socket_t *socket = net_require_socket(js, js_getthis(js));
  double timeout = 0;

  if (!socket) return js->thrown_value;
  if (nargs > 0 && vtype(args[0]) == T_NUM) timeout = js_getnum(args[0]);
  if (socket->conn) ant_conn_set_timeout_ms(socket->conn, timeout > 0 ? (uint64_t)timeout : 0);
  net_socket_sync_state(socket);

  if (nargs > 1 && is_callable(args[1]))
    net_add_listener(js, socket->obj, "timeout", args[1], true);

  return js_getthis(js);
}

static ant_value_t js_net_socket_setNoDelay(ant_t *js, ant_value_t *args, int nargs) {
  net_socket_t *socket = net_require_socket(js, js_getthis(js));
  bool enable = nargs == 0 || js_truthy(js, args[0]);
  if (!socket) return js->thrown_value;
  if (socket && socket->conn) ant_conn_set_no_delay(socket->conn, enable);
  return js_getthis(js);
}

static ant_value_t js_net_socket_setKeepAlive(ant_t *js, ant_value_t *args, int nargs) {
  net_socket_t *socket = net_require_socket(js, js_getthis(js));
  bool enable = nargs > 0 && js_truthy(js, args[0]);
  unsigned int delay = nargs > 1 && vtype(args[1]) == T_NUM ? (unsigned int)(js_getnum(args[1]) / 1000.0) : 0;
  if (!socket) return js->thrown_value;
  if (socket && socket->conn) ant_conn_set_keep_alive(socket->conn, enable, delay);
  return js_getthis(js);
}

static ant_value_t js_net_socket_ref(ant_t *js, ant_value_t *args, int nargs) {
  net_socket_t *socket = net_require_socket(js, js_getthis(js));
  if (!socket) return js->thrown_value;
  if (socket && socket->conn) ant_conn_ref(socket->conn);
  return js_getthis(js);
}

static ant_value_t js_net_socket_unref(ant_t *js, ant_value_t *args, int nargs) {
  net_socket_t *socket = net_require_socket(js, js_getthis(js));
  if (!socket) return js->thrown_value;
  if (socket && socket->conn) ant_conn_unref(socket->conn);
  return js_getthis(js);
}

static ant_value_t js_net_socket_write(ant_t *js, ant_value_t *args, int nargs) {
  net_socket_t *socket = net_require_socket(js, js_getthis(js));
  net_write_args_t parsed;
  char *copy = NULL;

  if (!socket) return js->thrown_value;
  if (!socket->conn) return js_false;
  if (!net_parse_write_args(js, args, nargs, &parsed)) return parsed.error;
  if (parsed.len == 0) return js_true;

  copy = malloc(parsed.len);
  if (!copy) return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
  
  memcpy(copy, parsed.bytes, parsed.len);
  if (ant_conn_write(socket->conn, copy, parsed.len, NULL, NULL) != 0) return js_false;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, parsed.callback);
  net_socket_sync_state(socket);
  
  if (is_callable(parsed.callback)) net_call_value(js, parsed.callback, js_mkundef(), NULL, 0);
  GC_ROOT_RESTORE(js, root_mark);
  
  return js_true;
}

static ant_value_t js_net_socket_end(ant_t *js, ant_value_t *args, int nargs) {
  net_socket_t *socket = net_require_socket(js, js_getthis(js));
  net_write_args_t parsed;
  ant_value_t result = js_getthis(js);

  if (!socket) return js->thrown_value;
  if (!socket->conn) return result;
  if (!net_parse_write_args(js, args, nargs, &parsed)) return parsed.error;
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, parsed.callback);
  
  if (parsed.len > 0) {
  ant_value_t write_result = js_net_socket_write(js, args, nargs);
  if (is_err(write_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return write_result;
  }}
  
  ant_conn_shutdown(socket->conn);
  if (is_callable(parsed.callback)) net_call_value(js, parsed.callback, js_mkundef(), NULL, 0);
  GC_ROOT_RESTORE(js, root_mark);
  
  return result;
}

static ant_value_t js_net_socket_destroy(ant_t *js, ant_value_t *args, int nargs) {
  net_socket_t *socket = net_require_socket(js, js_getthis(js));
  if (!socket) return js->thrown_value;

  if (nargs > 0 && vtype(args[0]) != T_UNDEF && vtype(args[0]) != T_NULL) {
    ant_value_t err = args[0];
    socket->had_error = true;
    net_emit(js, socket->obj, "error", &err, 1);
  }

  if (socket->conn) ant_conn_close(socket->conn);
  return js_getthis(js);
}

static ant_value_t net_socket_connect_parsed(ant_t *js, net_socket_t *socket, const net_connect_args_t *parsed) {
  ant_conn_t *conn = NULL;
  int rc = 0;

  if (!socket || !parsed) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid net.Socket");
  if (socket->conn || socket->connecting)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Socket is already connected or connecting");

  memset(&socket->client_listener, 0, sizeof(socket->client_listener));
  socket->client_listener.loop = uv_default_loop();
  socket->client_listener.user_data = socket;
  socket->client_listener.callbacks.on_read = net_socket_on_read;
  socket->client_listener.callbacks.on_end = net_socket_on_end;
  socket->client_listener.callbacks.on_error = net_socket_on_error;
  socket->client_listener.callbacks.on_timeout = net_socket_on_timeout;
  socket->client_listener.callbacks.on_conn_close = net_server_on_conn_close;
  socket->client_listener.idle_timeout_ms = parsed->timeout_ms;

  conn = parsed->path
    ? ant_conn_create_pipe(&socket->client_listener, parsed->timeout_ms)
    : ant_conn_create_tcp(&socket->client_listener, parsed->timeout_ms);
  if (!conn) return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");

  socket->conn = conn;
  socket->connecting = true;
  socket->destroyed = false;
  socket->had_error = false;
  ant_conn_set_user_data(conn, socket);
  net_add_active_socket(socket);
  net_socket_sync_state(socket);

  if (is_callable(parsed->callback))
    net_add_listener(js, socket->obj, "connect", parsed->callback, true);

  if (parsed->no_delay) ant_conn_set_no_delay(conn, true);
  if (parsed->keep_alive)
    ant_conn_set_keep_alive(conn, true, parsed->keep_alive_initial_delay_secs);

  if (parsed->path)
    rc = ant_conn_connect_pipe(conn, parsed->path, net_socket_on_connect, socket);
  else
    rc = ant_conn_connect_tcp(conn, parsed->host ? parsed->host : "localhost", parsed->port, net_socket_on_connect, socket);

  if (rc != 0) {
    socket->connecting = false;
    socket->had_error = true;
    net_socket_sync_state(socket);
    ant_value_t err = js_mkerr_typed(js, JS_ERR_TYPE, "%s", uv_strerror(rc));
    net_emit(js, socket->obj, "error", &err, 1);
    ant_conn_close(conn);
  }

  return socket->obj;
}

static ant_value_t js_net_socket_connect(ant_t *js, ant_value_t *args, int nargs) {
  net_socket_t *socket = net_require_socket(js, js_getthis(js));
  net_connect_args_t parsed;

  if (!socket) return js->thrown_value;
  if (!net_parse_connect_args(js, args, nargs, &parsed)) return parsed.error;
  return net_socket_connect_parsed(js, socket, &parsed);
}

static ant_value_t js_net_server_ctor(ant_t *js, ant_value_t *args, int nargs) {
  net_server_t *server = net_server_create(js);

  if (!server) return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
  if (nargs > 0 && vtype(args[0]) == T_OBJ) net_server_apply_options(js, server, args[0]);
  if (nargs > 0 && is_callable(args[0])) net_add_listener(js, server->obj, "connection", args[0], false);
  else if (nargs > 1 && is_callable(args[1])) net_add_listener(js, server->obj, "connection", args[1], false);

  return server->obj;
}

static ant_value_t net_server_bind_listener(
  ant_t *js,
  net_server_t *server,
  const net_listen_args_t *parsed,
  const ant_listener_callbacks_t *callbacks
) {
  uv_loop_t *loop = uv_default_loop();
  int rc = 0;

  if (parsed->path) {
    server->path = strdup(parsed->path);
    if (!server->path) return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
    
    rc = ant_listener_listen_pipe(
      &server->listener, loop, server->path, 
      server->backlog, 0, callbacks, server
    );
  } else {
    const char *host = parsed->host;
    net_server_parse_host(host, &host);
    
    free(server->host);
    server->host = strdup(host ? host : "0.0.0.0");
    if (!server->host) return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
    
    rc = ant_listener_listen_tcp(
      &server->listener, loop, server->host, parsed->port, 
      server->backlog, 0, callbacks, server
    );
  }

  if (rc != 0) return js_mkerr_typed(js, JS_ERR_TYPE, "%s", uv_strerror(rc));
  return js_mkundef();
}

static ant_value_t js_net_server_listen(ant_t *js, ant_value_t *args, int nargs) {
  net_server_t *server = net_require_server(js, js_getthis(js));
  ant_listener_callbacks_t callbacks = {0};
  net_listen_args_t parsed;

  if (!server) return js->thrown_value;
  if (server->listening) return js_mkerr_typed(js, JS_ERR_TYPE, "Server is already listening");
  if (!net_parse_listen_args(js, args, nargs, &parsed)) return parsed.error;

  free(server->path);
  server->path = NULL;
  server->port = parsed.port;
  server->backlog = parsed.backlog > 0 ? parsed.backlog : server->backlog;

  callbacks.on_accept = net_server_on_accept;
  callbacks.on_read = net_socket_on_read;
  callbacks.on_end = net_socket_on_end;
  callbacks.on_error = net_socket_on_error;
  callbacks.on_timeout = net_socket_on_timeout;
  callbacks.on_conn_close = net_server_on_conn_close;
  callbacks.on_listener_close = net_server_on_listener_close;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, parsed.callback);
  
  ant_value_t bind_result = net_server_bind_listener(js, server, &parsed, &callbacks);
  if (is_err(bind_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return bind_result;
  }

  server->port = ant_listener_port(&server->listener);
  server->listening = true;
  server->closing = false;
  net_server_sync_state(server);
  net_add_active_server(server);

  if (is_callable(parsed.callback)) net_call_value(js, parsed.callback, js_mkundef(), NULL, 0);
  net_emit(js, server->obj, "listening", NULL, 0);
  GC_ROOT_RESTORE(js, root_mark);
  
  return js_getthis(js);
}

static ant_value_t js_net_server_close(ant_t *js, ant_value_t *args, int nargs) {
  net_server_t *server = net_require_server(js, js_getthis(js));

  if (!server) return js->thrown_value;
  if (nargs > 0 && is_callable(args[0])) net_add_listener(js, server->obj, "close", args[0], true);

  if (!server->listening && !server->closing) {
    if (nargs > 0 && is_callable(args[0])) {
      ant_value_t err = js_mkerr_typed(js, JS_ERR_TYPE, "Server is not running");
      net_call_value(js, args[0], js_mkundef(), &err, 1);
    }
    return js_getthis(js);
  }

  server->closing = true;
  ant_listener_stop(&server->listener, false);
  net_server_maybe_finish_close(server);
  return js_getthis(js);
}

static ant_value_t js_net_server_address(ant_t *js, ant_value_t *args, int nargs) {
  net_server_t *server = net_require_server(js, js_getthis(js));
  ant_value_t out = js_mknull();
  
  if (!server) return js->thrown_value;
  if (!server || !server->listening) return out;
  if (server->path) return js_mkstr(js, server->path, strlen(server->path));

  struct sockaddr_storage saddr;
  int saddr_len = sizeof(saddr);
  
  if (uv_tcp_getsockname(&server->listener.handle.tcp, (struct sockaddr *)&saddr, &saddr_len) == 0) {
    char addr_str[INET6_ADDRSTRLEN] = {0};
    const char *family = "IPv4";
    int port = server->port;

    if (saddr.ss_family == AF_INET) {
      struct sockaddr_in *sa = (struct sockaddr_in *)&saddr;
      inet_ntop(AF_INET, &sa->sin_addr, addr_str, sizeof(addr_str));
      port = ntohs(sa->sin_port);
    } else if (saddr.ss_family == AF_INET6) {
      struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)&saddr;
      inet_ntop(AF_INET6, &sa6->sin6_addr, addr_str, sizeof(addr_str));
      port = ntohs(sa6->sin6_port);
      family = "IPv6";
    }

    out = js_mkobj(js);
    js_set(js, out, "port", js_mknum((double)port));
    js_set(js, out, "family", js_mkstr(js, family, strlen(family)));
    js_set(js, out, "address", js_mkstr(js, addr_str, strlen(addr_str)));
    
    return out;
  }

  struct in6_addr addr6;
  out = js_mkobj(js);
  js_set(js, out, "port", js_mknum(server->port));
  
  if (server->host && inet_pton(AF_INET6, server->host, &addr6) == 1) {
    char normalized[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &addr6, normalized, sizeof(normalized));
    js_set(js, out, "family", js_mkstr(js, "IPv6", 4));
    js_set(js, out, "address", js_mkstr(js, normalized, strlen(normalized)));
  } else {
    const char *h = server->host ? server->host : "0.0.0.0";
    js_set(js, out, "family", js_mkstr(js, "IPv4", 4));
    js_set(js, out, "address", js_mkstr(js, h, strlen(h)));
  }
  
  return out;
}

static ant_value_t js_net_server_getConnections(ant_t *js, ant_value_t *args, int nargs) {
  net_server_t *server = net_require_server(js, js_getthis(js));
  ant_value_t cb = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t argv[2] = { js_mknull(), js_mknum((double)net_server_socket_count(server)) };

  if (!server) return js->thrown_value;
  if (is_callable(cb)) net_call_value(js, cb, js_mkundef(), argv, 2);
  return js_getthis(js);
}

static ant_value_t js_net_server_ref(ant_t *js, ant_value_t *args, int nargs) {
  net_server_t *server = net_require_server(js, js_getthis(js));
  if (!server) return js->thrown_value;
  if (server) ant_listener_ref(&server->listener);
  return js_getthis(js);
}

static ant_value_t js_net_server_unref(ant_t *js, ant_value_t *args, int nargs) {
  net_server_t *server = net_require_server(js, js_getthis(js));
  if (!server) return js->thrown_value;
  if (server) ant_listener_unref(&server->listener);
  return js_getthis(js);
}

static ant_value_t js_net_createServer(ant_t *js, ant_value_t *args, int nargs) {
  return js_net_server_ctor(js, args, nargs);
}

static ant_value_t js_net_createConnection(ant_t *js, ant_value_t *args, int nargs) {
  net_connect_args_t parsed;
  net_socket_t *socket = NULL;

  if (!net_parse_connect_args(js, args, nargs, &parsed)) return parsed.error;
  socket = net_socket_create(js, parsed.allow_half_open);
  if (!socket) return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");

  return net_socket_connect_parsed(js, socket, &parsed);
}

static ant_value_t js_net_connect(ant_t *js, ant_value_t *args, int nargs) {
  return js_net_createConnection(js, args, nargs);
}

static ant_value_t js_net_getDefaultAutoSelectFamily(ant_t *js, ant_value_t *args, int nargs) {
  return js_bool(g_default_auto_select_family);
}

static ant_value_t js_net_setDefaultAutoSelectFamily(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs > 0) g_default_auto_select_family = js_truthy(js, args[0]);
  return js_mkundef();
}

static ant_value_t js_net_getDefaultAutoSelectFamilyAttemptTimeout(ant_t *js, ant_value_t *args, int nargs) {
  return js_mknum(g_default_auto_select_family_attempt_timeout);
}

static ant_value_t js_net_setDefaultAutoSelectFamilyAttemptTimeout(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs > 0 && vtype(args[0]) == T_NUM) {
    double value = js_getnum(args[0]);
    if (value > 0 && value < 10) value = 10;
    if (value > 0) g_default_auto_select_family_attempt_timeout = value;
  }
  return js_mkundef();
}

static void net_init_constructors(ant_t *js) {
  ant_value_t events = 0;
  ant_value_t ee_ctor = 0;
  ant_value_t ee_proto = 0;

  if (g_net_server_ctor && g_net_socket_ctor) return;

  events = events_library(js);
  ee_ctor = js_get(js, events, "EventEmitter");
  ee_proto = js_get(js, ee_ctor, "prototype");

  g_net_socket_proto = js_mkobj(js);
  js_set_proto_init(g_net_socket_proto, ee_proto);
  js_set(js, g_net_socket_proto, "address", js_mkfun(js_net_socket_address));
  js_set(js, g_net_socket_proto, "pause", js_mkfun(js_net_socket_pause));
  js_set(js, g_net_socket_proto, "resume", js_mkfun(js_net_socket_resume));
  js_set(js, g_net_socket_proto, "setEncoding", js_mkfun(js_net_socket_setEncoding));
  js_set(js, g_net_socket_proto, "setTimeout", js_mkfun(js_net_socket_setTimeout));
  js_set(js, g_net_socket_proto, "setNoDelay", js_mkfun(js_net_socket_setNoDelay));
  js_set(js, g_net_socket_proto, "setKeepAlive", js_mkfun(js_net_socket_setKeepAlive));
  js_set(js, g_net_socket_proto, "write", js_mkfun(js_net_socket_write));
  js_set(js, g_net_socket_proto, "end", js_mkfun(js_net_socket_end));
  js_set(js, g_net_socket_proto, "destroy", js_mkfun(js_net_socket_destroy));
  js_set(js, g_net_socket_proto, "connect", js_mkfun(js_net_socket_connect));
  js_set(js, g_net_socket_proto, "ref", js_mkfun(js_net_socket_ref));
  js_set(js, g_net_socket_proto, "unref", js_mkfun(js_net_socket_unref));
  js_set_sym(js, g_net_socket_proto, get_toStringTag_sym(), js_mkstr(js, "Socket", 6));
  g_net_socket_ctor = js_make_ctor(js, js_net_socket_ctor, g_net_socket_proto, "Socket", 6);

  g_net_server_proto = js_mkobj(js);
  js_set_proto_init(g_net_server_proto, ee_proto);
  js_set(js, g_net_server_proto, "listen", js_mkfun(js_net_server_listen));
  js_set(js, g_net_server_proto, "close", js_mkfun(js_net_server_close));
  js_set(js, g_net_server_proto, "address", js_mkfun(js_net_server_address));
  js_set(js, g_net_server_proto, "getConnections", js_mkfun(js_net_server_getConnections));
  js_set(js, g_net_server_proto, "ref", js_mkfun(js_net_server_ref));
  js_set(js, g_net_server_proto, "unref", js_mkfun(js_net_server_unref));
  js_set_sym(js, g_net_server_proto, get_toStringTag_sym(), js_mkstr(js, "Server", 6));
  g_net_server_ctor = js_make_ctor(js, js_net_server_ctor, g_net_server_proto, "Server", 6);
}

ant_value_t net_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);

  net_init_constructors(js);
  js_set(js, lib, "Server", g_net_server_ctor);
  js_set(js, lib, "Socket", g_net_socket_ctor);
  js_set(js, lib, "createServer", js_mkfun(js_net_createServer));
  js_set(js, lib, "createConnection", js_mkfun(js_net_createConnection));
  js_set(js, lib, "connect", js_mkfun(js_net_connect));
  js_set(js, lib, "isIP", js_mkfun(net_isIP));
  js_set(js, lib, "isIPv4", js_mkfun(net_isIPv4));
  js_set(js, lib, "isIPv6", js_mkfun(net_isIPv6));
  js_set(js, lib, "getDefaultAutoSelectFamily", js_mkfun(js_net_getDefaultAutoSelectFamily));
  js_set(js, lib, "setDefaultAutoSelectFamily", js_mkfun(js_net_setDefaultAutoSelectFamily));
  js_set(js, lib, "getDefaultAutoSelectFamilyAttemptTimeout", js_mkfun(js_net_getDefaultAutoSelectFamilyAttemptTimeout));
  js_set(js, lib, "setDefaultAutoSelectFamilyAttemptTimeout", js_mkfun(js_net_setDefaultAutoSelectFamilyAttemptTimeout));
  js_set(js, lib, "default", lib);
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "net", 3));
  
  return lib;
}

void gc_mark_net(ant_t *js, gc_mark_fn mark) {
  net_server_t *server = NULL;
  net_socket_t *socket = NULL;

  if (g_net_server_proto) mark(js, g_net_server_proto);
  if (g_net_socket_proto) mark(js, g_net_socket_proto);
  if (g_net_server_ctor) mark(js, g_net_server_ctor);
  if (g_net_socket_ctor) mark(js, g_net_socket_ctor);

  for (server = g_active_servers; server; server = server->next_active)
    mark(js, server->obj);
    
  for (socket = g_active_sockets; socket; socket = socket->next_active) {
    mark(js, socket->obj);
    if (vtype(socket->encoding) != T_UNDEF) mark(js, socket->encoding);
  }
}
