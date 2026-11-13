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
#include "cfunc.h"
#include "list.h"
#include "watch.h"
#include "ptr.h"
#include "errors.h"
#include "internal.h"

#include "gc/roots.h"
#include "gc/modules.h"
#include "silver/call.h"
#include "net/connection.h"
#include "net/listener.h"
#include "net/listen_policy.h"

#include "modules/buffer.h"
#include "modules/events.h"
#include "modules/net.h"
#include "modules/stream.h"

typedef struct net_connect_args_s net_connect_args_t;
typedef struct net_write_args_s  net_write_args_t;

struct net_socket_s {
  ant_t *js;
  ant_value_t obj;
  ant_conn_t *conn;
  ant_listener_t client_listener;
  net_server_t *server;
  net_socket_t *next;
  net_socket_t **pprev;
  struct net_socket_s *next_in_server;
  bool destroyed;
  bool connecting;
  bool had_error;
  bool need_drain;
  bool synced_pending;
  bool synced_connecting;
  const char *synced_ready_state;
  double synced_bytes_read;
  double synced_bytes_written;
  double synced_timeout;
};

struct net_server_s {
  ant_t *js;
  ant_value_t obj;
  ant_listener_t listener;
  net_socket_t *sockets;
  net_server_t *next;
  net_server_t **pprev;
  char *host;
  char *path;
  int port;
  int backlog;
  int max_connections;
  uint32_t generation;
  bool listening;
  bool closing;
  bool allow_half_open;
  bool pause_on_connect;
  bool no_delay;
  bool keep_alive;
  unsigned int keep_alive_initial_delay_secs;
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

static constexpr uint32_t NET_SERVER_NATIVE_TAG = Ant_NativeTag("NETS");
static constexpr uint32_t NET_SOCKET_NATIVE_TAG = Ant_NativeTag("NETK");
static constexpr size_t NET_SOCKET_HIGH_WATER_MARK = 64 * 1024;

static net_server_t *net_server_data(ant_value_t value) {
  return (net_server_t *)js_get_native(value, NET_SERVER_NATIVE_TAG);
}

static net_socket_t *net_socket_data(ant_value_t value) {
  return (net_socket_t *)js_get_native(value, NET_SOCKET_NATIVE_TAG);
}

int net_socket_fileno(ant_value_t value) {
#ifdef _WIN32
  (void)value;
  return -1;
#else
  net_socket_t *socket = net_socket_data(value);
  uv_os_fd_t fd;

  if (!socket || !socket->conn) return -1;
  uv_handle_t *handle = (uv_handle_t *)ant_conn_stream(socket->conn);
  if (!handle || uv_is_closing(handle) || uv_fileno(handle, &fd) != 0) return -1;

  return (int)fd;
#endif
}

static void net_add_active_server(net_server_t *server) {
  Ant_List_Insert(&server->js->net_servers, server);
}

static void net_add_active_socket(net_socket_t *socket) {
  if (socket) Ant_List_Insert(&socket->js->net_sockets, socket);
}

static net_server_t *net_require_server(ant_t *js, ant_value_t this_val) {
  net_server_t *server = net_server_data(this_val);
  if (!server) {
    js_mkerr_typed(js, JS_ERR_TYPE, "Invalid net.Server");
    return NULL;
  }
  return server;
}

static net_socket_t *net_require_socket(ant_t *js, ant_value_t this_val) {
  net_socket_t *socket = net_socket_data(this_val);
  if (!socket) {
    js_mkerr_typed(js, JS_ERR_TYPE, "Invalid net.Socket");
    return NULL;
  }
  return socket;
}

static bool net_parse_write_args(ant_native_params_t, net_write_args_t *out) {
  ant_value_t value = 0;

  if (!out) return false;
  memset(out, 0, sizeof(*out));
  out->callback = Ant_Value_Undefined;
  out->error = Ant_Value_Undefined;

  if (nargs < 1 || vtype(args[0]) == kTypeUndefined || vtype(args[0]) == kTypeNull) return true;
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

static bool net_string_is_blank(ant_t *js, ant_value_t value) {
  size_t len = 0;
  const char *str = js_getstr(js, value, &len);

  for (size_t i = 0; str && i < len; i++) switch (str[i]) {
    case ' ': case '\t': case '\n': case '\v': case '\f': case '\r': break;
    default: return false;
  }

  return true;
}

bool net_value_is_port(ant_t *js, ant_value_t value) {
  return vtype(value) == kTypeNumber || (vtype(value) == kTypeString && js_to_number(js, value) >= 0);
}

static bool net_port_from_number(ant_t *js, ant_value_t value, double port, const char *range_name, int *out_port, ant_value_t *out_error) {
  if (port >= 0 && port <= 65535 && port == (double)(int)port) {
    *out_port = (int)port;
    return true;
  }

  char received[160], message[256];
  js_describe_received(js, value, received, sizeof(received));
  snprintf(message, sizeof(message), "%s should be >= 0 and < 65536. %s.", range_name, received);
  *out_error = js_mkerr_code(js, JS_ERR_RANGE, "ERR_SOCKET_BAD_PORT", message);
  
  return false;
}

static double net_port_number(ant_t *js, ant_value_t value) {
  if (vtype(value) == kTypeNumber) return Ant_Value_AsNumberUnchecked(value);
  return net_string_is_blank(js, value) ? -1 : js_to_number(js, value);
}

bool net_parse_port(ant_t *js, ant_value_t value, const char *range_name, int *out_port, ant_value_t *out_error) {
  uint8_t type = vtype(value);
  if (type != kTypeNumber && type != kTypeString) {
    *out_error = Ant_Error_CreateInvalidArgType(js, "options.port", "one of type number or string", value);
    return false;
  }

  return net_port_from_number(js, value, net_port_number(js, value), range_name, out_port, out_error);
}

static bool net_parse_listen_port(ant_t *js, ant_value_t value, ant_value_t options, int *out_port, ant_value_t *out_error) {
  uint8_t type = vtype(value);
  if (type == kTypeUndefined || type == kTypeNull) return true;

  if (type != kTypeNumber && type != kTypeString) {
    *out_error = Ant_Error_CreateInvalidArgValue(js, "options", NULL, options);
    return false;
  }

  return net_port_from_number(js, value, net_port_number(js, value), "options.port", out_port, out_error);
}

bool net_parse_listen_args(ant_native_params_t, net_listen_args_t *out) {
  if (!out) return false;
  
  memset(out, 0, sizeof(*out));
  out->backlog = 511;
  out->callback = Ant_Value_Undefined;
  out->error = Ant_Value_Undefined;

  if (nargs == 0) return true;

  if (net_value_is_port(js, args[0])) {
    if (!net_parse_listen_port(js, args[0], args[0], &out->port, &out->error)) return false;
    
    if (nargs > 1 && vtype(args[1]) == kTypeString) {
      size_t len = 0;
      out->host = js_getstr(js, args[1], &len);
    }
    
    if (nargs > 2 && vtype(args[2]) == kTypeNumber) out->backlog = (int)Ant_Value_AsNumberUnchecked(args[2]);
    if (nargs > 3 && is_callable(args[3])) out->callback = args[3];
    else if (nargs > 2 && is_callable(args[2])) out->callback = args[2];
    else if (nargs > 1 && is_callable(args[1])) out->callback = args[1];
    
    return true;
  }

  if (vtype(args[0]) == kTypeObject) {
    ant_value_t value = js_get(js, args[0], "path");
    
    if (vtype(value) == kTypeString) out->path = js_getstr(js, value, NULL);
    if (!net_parse_listen_port(js, js_get(js, args[0], "port"), args[0], &out->port, &out->error)) return false;
    
    value = js_get(js, args[0], "host");
    if (vtype(value) == kTypeString) {
      size_t len = 0;
      out->host = js_getstr(js, value, &len);
    }
    
    value = js_get(js, args[0], "backlog");
    if (vtype(value) == kTypeNumber) out->backlog = (int)Ant_Value_AsNumberUnchecked(value);
    if (nargs > 1 && is_callable(args[1])) out->callback = args[1];
    
    return true;
  }

  if (vtype(args[0]) == kTypeString) {
    out->path = js_getstr(js, args[0], NULL);
    if (nargs > 1 && vtype(args[1]) == kTypeNumber) out->backlog = (int)Ant_Value_AsNumberUnchecked(args[1]);
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

static bool net_take_string(ant_t *js, const char *name, ant_value_t value, const char **slot, net_connect_args_t *out) {
  ant_value_t nul = Ant_Error_CheckNoNullBytes(js, name, value);
  if (is_err(nul)) {
    out->error = nul;
    return false;
  }
  *slot = js_getstr(js, value, NULL);
  return true;
}

static bool net_parse_connect_args(ant_native_params_t, net_connect_args_t *out) {
  ant_value_t value = 0;

  if (!out) return false;
  memset(out, 0, sizeof(*out));
  out->host = "localhost";
  out->callback = Ant_Value_Undefined;
  out->error = Ant_Value_Undefined;
  out->no_delay = true;

  if (nargs == 0) {
    out->error = js_mkerr_typed(js, JS_ERR_TYPE, "net.connect requires options, port, or path");
    return false;
  }

  if (net_value_is_port(js, args[0])) {
    if (!net_parse_port(js, args[0], "Port", &out->port, &out->error)) return false;
    if (nargs > 1 && vtype(args[1]) == kTypeString && !net_take_string(js, "host", args[1], &out->host, out)) return false;
    if (nargs > 2 && is_callable(args[2])) out->callback = args[2];
    else if (nargs > 1 && is_callable(args[1])) out->callback = args[1];
    return true;
  }

  if (vtype(args[0]) == kTypeString) {
    if (!net_take_string(js, "path", args[0], &out->path, out)) return false;
    if (nargs > 1 && is_callable(args[1])) out->callback = args[1];
    return true;
  }

  if (vtype(args[0]) == kTypeObject) {
    value = js_get(js, args[0], "path");
    if (vtype(value) == kTypeString && !net_take_string(js, "path", value, &out->path, out)) return false;

    value = js_get(js, args[0], "socketPath");
    if (!out->path && vtype(value) == kTypeString && !net_take_string(js, "path", value, &out->path, out)) return false;

    value = js_get(js, args[0], "port");
    if (!out->path && vtype(value) != kTypeUndefined && !net_parse_port(js, value, "Port", &out->port, &out->error)) return false;

    value = js_get(js, args[0], "host");
    if (vtype(value) == kTypeString && !net_take_string(js, "host", value, &out->host, out)) return false;

    value = js_get(js, args[0], "hostname");
    if (vtype(value) == kTypeString && !net_take_string(js, "hostname", value, &out->host, out)) return false;

    value = js_get(js, args[0], "allowHalfOpen");
    if (vtype(value) != kTypeUndefined) out->allow_half_open = js_truthy(js, value);

    value = js_get(js, args[0], "timeout");
    if (vtype(value) == kTypeNumber && Ant_Value_AsNumberUnchecked(value) > 0) out->timeout_ms = (uint64_t)Ant_Value_AsNumberUnchecked(value);

    value = js_get(js, args[0], "noDelay");
    if (vtype(value) != kTypeUndefined) out->no_delay = js_truthy(js, value);

    value = js_get(js, args[0], "keepAlive");
    if (vtype(value) != kTypeUndefined) out->keep_alive = js_truthy(js, value);

    value = js_get(js, args[0], "keepAliveInitialDelay");
    if (vtype(value) == kTypeNumber && Ant_Value_AsNumberUnchecked(value) > 0)
      out->keep_alive_initial_delay_secs = (unsigned int)(Ant_Value_AsNumberUnchecked(value) / 1000.0);

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
  ArrayBufferData *ab = create_array_buffer_data(js, len);
  if (!ab) return Ant_Error_CreateOutOfMemory(js);
  if (len > 0 && data) memcpy(ab->data, data, len);
  return create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, len, js->builtins.buffer_proto);
}

static void net_socket_sync_state(net_socket_t *socket) {
  if (!socket) return;

  ant_t *js = socket->js;
  ant_value_t obj = socket->obj;
  if (!is_object_type(obj)) return;

  const char *ready_state = "open";
  if (socket->destroyed) ready_state = "closed";
  else if (socket->connecting) ready_state = "opening";
  stream_set_destroyed(js, obj, socket->destroyed);

  bool first_sync = socket->synced_ready_state == NULL;
  bool pending = socket->connecting || (socket->conn == NULL && !socket->destroyed);
  
  double bytes_read = (double)(socket->conn ? ant_conn_bytes_read(socket->conn) : 0);
  double bytes_written = (double)(socket->conn ? ant_conn_bytes_written(socket->conn) : 0);
  double timeout = (double)(socket->conn ? ant_conn_timeout_ms(socket->conn) : 0);

  if (first_sync || pending != socket->synced_pending) {
    socket->synced_pending = pending;
    js_set(js, obj, "pending", Ant_Value_FromBool(pending));
  }

  if (first_sync || socket->connecting != socket->synced_connecting) {
    socket->synced_connecting = socket->connecting;
    js_set(js, obj, "connecting", Ant_Value_FromBool(socket->connecting));
  }

  if (ready_state != socket->synced_ready_state) {
    socket->synced_ready_state = ready_state;
    js_set(js, obj, "readyState", js_mkstr(js, ready_state, strlen(ready_state)));
  }

  if (first_sync || bytes_read != socket->synced_bytes_read) {
    socket->synced_bytes_read = bytes_read;
    js_set(js, obj, "bytesRead", Ant_Value_FromNumber(bytes_read));
  }

  if (first_sync || bytes_written != socket->synced_bytes_written) {
    socket->synced_bytes_written = bytes_written;
    js_set(js, obj, "bytesWritten", Ant_Value_FromNumber(bytes_written));
  }

  if (first_sync || timeout != socket->synced_timeout) {
    socket->synced_timeout = timeout;
    js_set(js, obj, "timeout", Ant_Value_FromNumber(timeout));
  }
}

static void net_server_sync_state(net_server_t *server) {
  if (!server || !is_object_type(server->obj)) return;
  js_set(server->js, server->obj, "listening", Ant_Value_FromBool(server->listening));
  js_set(server->js, server->obj, "maxConnections", Ant_Value_FromNumber((double)server->max_connections));
  js_set(server->js, server->obj, "dropMaxConnection", Ant_Value_False);
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
  eventemitter_emit_args(server->js, server->obj, "close", NULL, 0);
  Ant_List_Remove(server);
}

static void net_socket_unlink(net_socket_t *socket) {
  net_server_t *server = socket->server;

  if (server) {
    net_socket_t **it = NULL;
    for (it = &socket->server->sockets; *it; it = &(*it)->next_in_server) if (*it == socket) {
      *it = socket->next_in_server;
      break;
    }
    socket->next_in_server = NULL;
    socket->server = NULL;
  }

  Ant_List_Remove(socket);
  net_server_maybe_finish_close(server);
}

static void net_server_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  net_server_t *server = net_server_data(value);
  if (!server) return;
  
  js_clear_native(value, NET_SERVER_NATIVE_TAG);
  server->obj = Ant_Value_Undefined;
  if (server->listener.started && !ant_listener_is_closed(&server->listener)) return;
  
  for (net_socket_t *socket = server->sockets; socket; socket = socket->next_in_server) socket->server = NULL;
  Ant_List_Remove(server);
  
  free(server->host);
  free(server->path);
  free(server);
}

static void net_socket_finalize(ant_t *js, ant_object_t *obj) {
  net_socket_t *socket = net_socket_data(js_obj_from_ptr(obj));
  if (!socket || socket->conn) return;
  net_socket_unlink(socket);
  js_clear_native(js_obj_from_ptr(obj), NET_SOCKET_NATIVE_TAG);
  free(socket);
}

static ant_value_t net_socket_emit_connect_error(ant_params_t) {
  ant_value_t state = js_func_data(call_func);
  ant_value_t socket_obj = is_object_type(state) ? js_get(js, state, "socket") : Ant_Value_Undefined;
  ant_value_t status_val = is_object_type(state) ? js_get(js, state, "status") : Ant_Value_FromNumber(UV_EIO);
  
  net_socket_t *socket = net_socket_data(socket_obj);
  int status = vtype(status_val) == kTypeNumber ? (int)Ant_Value_AsNumberUnchecked(status_val) : UV_EIO;
  
  ant_value_t err = 0;
  if (!socket) return Ant_Value_Undefined;

  socket->connecting = false;
  socket->had_error = true;
  net_socket_sync_state(socket);
  
  err = Ant_Error_Create(js, JS_ERR_TYPE, uv_strerror(status));
  eventemitter_emit_args(js, socket->obj, "error", &err, 1);
  if (socket->conn) ant_conn_close(socket->conn);

  return Ant_Value_Undefined;
}

static void net_socket_schedule_connect_error(ant_t *js, net_socket_t *socket, int status) {
  ant_value_t state = Ant_Object_CreateBare(js);
  js_set(js, state, "socket", socket->obj);
  js_set(js, state, "status", Ant_Value_FromNumber((double)status));
  queue_microtask(js, js_heavy_mkfun(js, net_socket_emit_connect_error, state));
}

static void net_socket_on_read_end(ant_t *js, ant_value_t stream_obj) {
  net_socket_t *socket = net_socket_data(stream_obj);
  if (socket && socket->conn) ant_conn_mark_read_done(socket->conn);
}

static net_socket_t *net_socket_create(ant_t *js, bool allow_half_open, ant_value_t new_target) {
  ant_value_t obj = Ant_Object_CreateBare(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, js->builtins.net_socket_proto, new_target);
  net_socket_t *socket = calloc(1, sizeof(*socket));

  if (!socket) return NULL;
  if (is_object_type(proto)) js_set_proto_init(obj, proto);

  socket->js = js;
  socket->obj = obj;

  js_set_native(obj, socket, NET_SOCKET_NATIVE_TAG);
  js_set_finalizer(obj, net_socket_finalize);
  stream_init_duplex_object(js, obj, Ant_Value_Undefined);
  stream_set_end_hook(obj, net_socket_on_read_end);
  
  js_set(js, obj, "allowHalfOpen", Ant_Value_FromBool(allow_half_open));
  js_set(js, obj, "remoteAddress", Ant_Value_Undefined);
  js_set(js, obj, "remotePort", Ant_Value_Undefined);
  js_set(js, obj, "remoteFamily", Ant_Value_Undefined);
  js_set(js, obj, "localAddress", Ant_Value_Undefined);
  js_set(js, obj, "localPort", Ant_Value_Undefined);
  js_set(js, obj, "localFamily", Ant_Value_Undefined);
  net_socket_sync_state(socket);
  
  return socket;
}

static void net_socket_attach_conn(net_socket_t *socket, ant_conn_t *conn) {
  if (!socket || !conn) return;

  socket->conn = conn;
  ant_conn_set_user_data(conn, socket);
  if (ant_conn_has_remote_addr(conn)) {
    js_set(socket->js, socket->obj, "remoteAddress", js_mkstr(socket->js, ant_conn_remote_addr(conn), strlen(ant_conn_remote_addr(conn))));
    js_set(socket->js, socket->obj, "remotePort", Ant_Value_FromNumber(ant_conn_remote_port(conn)));
    js_set(socket->js, socket->obj, "remoteFamily", js_mkstr(socket->js, ant_conn_remote_family(conn), strlen(ant_conn_remote_family(conn))));
  }
  if (ant_conn_has_local_addr(conn)) {
    js_set(socket->js, socket->obj, "localAddress", js_mkstr(socket->js, ant_conn_local_addr(conn), strlen(ant_conn_local_addr(conn))));
    js_set(socket->js, socket->obj, "localPort", Ant_Value_FromNumber(ant_conn_local_port(conn)));
    js_set(socket->js, socket->obj, "localFamily", js_mkstr(socket->js, ant_conn_local_family(conn), strlen(ant_conn_local_family(conn))));
  }
  net_socket_sync_state(socket);
}

static net_server_t *net_server_create(ant_t *js, ant_value_t new_target) {
  ant_value_t obj = Ant_Object_CreateBare(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, js->builtins.net_server_proto, new_target);
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
  js_set_finalizer(obj, net_server_finalize);
  net_server_sync_state(server);
  return server;
}

static ant_value_t net_isIP(ant_params_t) {
  size_t len = 0;
  const char *host = NULL;
  struct in_addr addr4;
  struct in6_addr addr6;

  if (nargs < 1) return Ant_Value_FromNumber(0);
  host = js_getstr(js, args[0], &len);
  if (!host) return Ant_Value_FromNumber(0);

  if (inet_pton(AF_INET, host, &addr4) == 1) return Ant_Value_FromNumber(4);
  if (inet_pton(AF_INET6, host, &addr6) == 1) return Ant_Value_FromNumber(6);
  return Ant_Value_FromNumber(0);
}

static ant_value_t net_isIPv4(ant_params_t) {
  if (Ant_Value_AsNumberUnchecked(net_isIP(js, args, nargs, Ant_Value_Undefined, call_this, call_func)) == 4.0) return Ant_Value_True;
  return Ant_Value_False;
}

static ant_value_t net_isIPv6(ant_params_t) {
  if (Ant_Value_AsNumberUnchecked(net_isIP(js, args, nargs, Ant_Value_Undefined, call_this, call_func)) == 6.0) return Ant_Value_True;
  return Ant_Value_False;
}

static void net_server_apply_options(ant_t *js, net_server_t *server, ant_value_t options) {
  ant_value_t value = 0;

  if (!server || vtype(options) != kTypeObject) return;

  value = js_get(js, options, "allowHalfOpen");
  if (vtype(value) != kTypeUndefined) server->allow_half_open = js_truthy(js, value);

  value = js_get(js, options, "pauseOnConnect");
  if (vtype(value) != kTypeUndefined) server->pause_on_connect = js_truthy(js, value);

  value = js_get(js, options, "noDelay");
  if (vtype(value) != kTypeUndefined) server->no_delay = js_truthy(js, value);

  value = js_get(js, options, "keepAlive");
  if (vtype(value) != kTypeUndefined) server->keep_alive = js_truthy(js, value);

  value = js_get(js, options, "keepAliveInitialDelay");
  if (vtype(value) == kTypeNumber && Ant_Value_AsNumberUnchecked(value) > 0)
    server->keep_alive_initial_delay_secs = (unsigned int)(Ant_Value_AsNumberUnchecked(value) / 1000.0);

  value = js_get(js, options, "backlog");
  if (vtype(value) == kTypeNumber && Ant_Value_AsNumberUnchecked(value) > 0)
    server->backlog = (int)Ant_Value_AsNumberUnchecked(value);
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

  chunk = net_make_buffer_chunk(socket->js, buffer, (size_t)nread);
  ant_conn_consume(conn, total);
  net_socket_sync_state(socket);
  
  if (!is_err(chunk)) chunk = stream_readable_push(socket->js, socket->obj, chunk, Ant_Value_Undefined);
  if (chunk == Ant_Value_False && socket->conn) ant_conn_pause_read(socket->conn);
  if (!is_err(chunk)) return;

  GC_ROOT_SAVE(root_mark, socket->js);
  chunk = js_take_thrown(socket->js, chunk);

  GC_ROOT_PIN(socket->js, chunk);
  socket->had_error = true;

  eventemitter_emit_args(socket->js, socket->obj, "error", &chunk, 1);
  GC_ROOT_RESTORE(socket->js, root_mark);
  ant_conn_close(conn);
}

static void net_socket_on_end(ant_conn_t *conn, void *user_data) {
  net_socket_t *socket = (net_socket_t *)ant_conn_get_user_data(conn);
  if (socket) stream_readable_end(socket->js, socket->obj);
}

static void net_socket_on_error(ant_conn_t *conn, int status, void *user_data) {
  net_socket_t *socket = (net_socket_t *)ant_conn_get_user_data(conn);

  if (!socket) return;
  socket->had_error = true; {
    ant_value_t err = Ant_Error_Create(socket->js, JS_ERR_TYPE, uv_strerror(status));
    eventemitter_emit_args(socket->js, socket->obj, "error", &err, 1);
  }
}

static void net_socket_on_timeout(ant_conn_t *conn, void *user_data) {
  net_socket_t *socket = (net_socket_t *)ant_conn_get_user_data(conn);
  if (!socket) return;
  eventemitter_emit_args(socket->js, socket->obj, "timeout", NULL, 0);
}

static void net_server_on_conn_close(ant_conn_t *conn, void *user_data) {
  net_socket_t *socket = (net_socket_t *)ant_conn_get_user_data(conn);
  ant_value_t arg = 0;

  if (!socket) return;
  arg = Ant_Value_FromBool(socket->had_error);
  socket->conn = NULL;
  socket->destroyed = true;
  
  net_socket_sync_state(socket);
  eventemitter_emit_args(socket->js, socket->obj, "close", &arg, 1);
  ant_conn_set_user_data(conn, NULL);
  net_socket_unlink(socket);
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
  eventemitter_emit_args(socket->js, socket->obj, "connect", NULL, 0);
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

  socket = net_socket_create(server->js, server->allow_half_open, Ant_Value_Undefined);
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
  eventemitter_emit_args(server->js, server->obj, "connection", &arg, 1);
}

static bool net_server_parse_host(const char *input, const char **out) {
  if (!input || !*input) {
    *out = "0.0.0.0";
    return true;
  }
  *out = input;
  return true;
}

static ant_value_t js_net_socket_ctor(ant_params_t) {
  net_socket_t *socket = NULL;
  bool allow_half_open = false;

  if (nargs > 0 && vtype(args[0]) == kTypeObject) {
    ant_value_t value = js_get(js, args[0], "allowHalfOpen");
    allow_half_open = js_truthy(js, value);
  }

  socket = net_socket_create(js, allow_half_open, call_new_target);
  if (!socket) return Ant_Error_CreateOutOfMemory(js);
  return socket->obj;
}

static ant_value_t js_net_socket_address(ant_params_t) {
  net_socket_t *socket = net_require_socket(js, call_this);
  ant_value_t out = Ant_Object_CreateBare(js);

  if (!socket) return Ant_Exception_Current(js);
  if (!socket || !socket->conn || !ant_conn_has_local_addr(socket->conn)) return out;

  js_set(js, out, "address", js_mkstr(js, ant_conn_local_addr(socket->conn), strlen(ant_conn_local_addr(socket->conn))));
  js_set(js, out, "port", Ant_Value_FromNumber(ant_conn_local_port(socket->conn)));
  js_set(js, out, "family", js_mkstr(js, ant_conn_local_family(socket->conn), strlen(ant_conn_local_family(socket->conn))));

  return out;
}

static ant_value_t js_net_socket_pause(ant_params_t) {
  net_socket_t *socket = net_require_socket(js, call_this);
  if (!socket) return Ant_Exception_Current(js);
  return stream_readable_pause(js, socket->obj);
}

static ant_value_t js_net_socket_resume(ant_params_t) {
  net_socket_t *socket = net_require_socket(js, call_this);
  if (!socket) return Ant_Exception_Current(js);
  if (socket->conn) ant_conn_resume_read(socket->conn);
  return stream_readable_resume(js, socket->obj);
}

static ant_value_t js_net_socket__read(ant_params_t) {
  net_socket_t *socket = net_require_socket(js, call_this);
  if (!socket) return Ant_Exception_Current(js);
  if (socket->conn) ant_conn_resume_read(socket->conn);
  return Ant_Value_Undefined;
}

static ant_value_t js_net_socket_setTimeout(ant_params_t) {
  net_socket_t *socket = net_require_socket(js, call_this);
  double timeout = 0;

  if (!socket) return Ant_Exception_Current(js);
  if (nargs > 0 && vtype(args[0]) == kTypeNumber) timeout = Ant_Value_AsNumberUnchecked(args[0]);
  if (socket->conn) ant_conn_set_timeout_ms(socket->conn, timeout > 0 ? (uint64_t)timeout : 0);
  net_socket_sync_state(socket);

  if (nargs > 1 && is_callable(args[1]))
    eventemitter_add_listener(js, socket->obj, "timeout", args[1], true);

  return call_this;
}

static ant_value_t js_net_socket_setNoDelay(ant_params_t) {
  net_socket_t *socket = net_require_socket(js, call_this);
  bool enable = nargs == 0 || js_truthy(js, args[0]);
  if (!socket) return Ant_Exception_Current(js);
  if (socket && socket->conn) ant_conn_set_no_delay(socket->conn, enable);
  return call_this;
}

static ant_value_t js_net_socket_setKeepAlive(ant_params_t) {
  net_socket_t *socket = net_require_socket(js, call_this);
  bool enable = nargs > 0 && js_truthy(js, args[0]);
  unsigned int delay = nargs > 1 && vtype(args[1]) == kTypeNumber ? (unsigned int)(Ant_Value_AsNumberUnchecked(args[1]) / 1000.0) : 0;
  if (!socket) return Ant_Exception_Current(js);
  if (socket && socket->conn) ant_conn_set_keep_alive(socket->conn, enable, delay);
  return call_this;
}

static ant_value_t js_net_socket_ref(ant_params_t) {
  net_socket_t *socket = net_require_socket(js, call_this);
  if (!socket) return Ant_Exception_Current(js);
  if (socket && socket->conn) ant_conn_ref(socket->conn);
  return call_this;
}

static ant_value_t js_net_socket_unref(ant_params_t) {
  net_socket_t *socket = net_require_socket(js, call_this);
  if (!socket) return Ant_Exception_Current(js);
  if (socket && socket->conn) ant_conn_unref(socket->conn);
  return call_this;
}

static void net_socket_on_write(ant_conn_t *conn, int status, void *user_data) {
  net_socket_t *socket = (net_socket_t *)ant_conn_get_user_data(conn);

  if (!socket || !socket->need_drain) return;
  if (status < 0 || ant_conn_write_queue_size(conn) > 0) return;
  
  socket->need_drain = false;
  eventemitter_emit_args(socket->js, socket->obj, "drain", NULL, 0);
}

static ant_value_t js_net_socket_write(ant_params_t) {
  net_socket_t *socket = net_require_socket(js, call_this);
  net_write_args_t parsed;

  if (!socket) return Ant_Exception_Current(js);
  if (!socket->conn) return Ant_Value_False;
  if (!net_parse_write_args(js, args, nargs, &parsed)) return parsed.error;
  if (parsed.len == 0) return Ant_Value_True;

  int rc = ant_conn_write_copy(socket->conn, (const char *)parsed.bytes, parsed.len, net_socket_on_write, NULL);
  if (rc == UV_ENOMEM) return Ant_Error_CreateOutOfMemory(js);
  if (rc != 0) return Ant_Value_False;

  bool ok = ant_conn_write_queue_size(socket->conn) < NET_SOCKET_HIGH_WATER_MARK;
  if (!ok) socket->need_drain = true;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, parsed.callback);
  net_socket_sync_state(socket);
  
  if (is_callable(parsed.callback)) sv_call_value(js, parsed.callback, Ant_Value_Undefined, NULL, 0);
  GC_ROOT_RESTORE(js, root_mark);
  
  return Ant_Value_FromBool(ok);
}

static ant_value_t js_net_socket_end(ant_params_t) {
  net_socket_t *socket = net_require_socket(js, call_this);
  net_write_args_t parsed;

  if (!socket) return Ant_Exception_Current(js);
  if (!socket->conn) return call_this;
  if (!net_parse_write_args(js, args, nargs, &parsed)) return parsed.error;
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, parsed.callback);
  
  if (parsed.len > 0) {
    ant_value_t write_result = js_net_socket_write(js, args, nargs, Ant_Value_Undefined, call_this, call_func);
    if (is_err(write_result)) { GC_ROOT_RESTORE(js, root_mark); return write_result; }
  }
  
  ant_conn_shutdown_write(socket->conn);
  if (is_callable(parsed.callback)) sv_call_value(js, parsed.callback, Ant_Value_Undefined, NULL, 0);
  GC_ROOT_RESTORE(js, root_mark);
  
  return call_this;
}

static ant_value_t js_net_socket_destroy(ant_params_t) {
  net_socket_t *socket = net_require_socket(js, call_this);
  if (!socket) return Ant_Exception_Current(js);
  if (socket->destroyed) return call_this;

  if (nargs > 0 && vtype(args[0]) != kTypeUndefined && vtype(args[0]) != kTypeNull) {
    ant_value_t err = args[0];
    socket->had_error = true;
    eventemitter_emit_args(js, socket->obj, "error", &err, 1);
  }

  socket->destroyed = true;
  socket->connecting = false;
  
  net_socket_sync_state(socket);
  if (socket->conn && !ant_conn_is_closing(socket->conn)) ant_conn_close(socket->conn);
  
  return call_this;
}

static ant_value_t net_socket_connect_parsed(ant_t *js, net_socket_t *socket, const net_connect_args_t *parsed) {
  ant_conn_t *conn = NULL;
  int rc = 0;

  if (!socket || !parsed) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid net.Socket");
  if (socket->conn || socket->connecting)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Socket is already connected or connecting");

  memset(&socket->client_listener, 0, sizeof(socket->client_listener));
  socket->client_listener.loop = js->loop;
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
  if (!conn) return Ant_Error_CreateOutOfMemory(js);

  socket->conn = conn;
  socket->connecting = true;
  socket->destroyed = false;
  socket->had_error = false;
  ant_conn_set_user_data(conn, socket);
  net_add_active_socket(socket);
  net_socket_sync_state(socket);

  if (is_callable(parsed->callback))
    eventemitter_add_listener(js, socket->obj, "connect", parsed->callback, true);

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
    net_socket_schedule_connect_error(js, socket, rc);
  }

  return socket->obj;
}

static ant_value_t js_net_socket_connect(ant_params_t) {
  net_socket_t *socket = net_require_socket(js, call_this);
  net_connect_args_t parsed;

  if (!socket) return Ant_Exception_Current(js);
  if (!net_parse_connect_args(js, args, nargs, &parsed)) return parsed.error;
  return net_socket_connect_parsed(js, socket, &parsed);
}

static ant_value_t js_net_server_ctor(ant_params_t) {
  net_server_t *server = net_server_create(js, call_new_target);

  if (!server) return Ant_Error_CreateOutOfMemory(js);
  if (nargs > 0 && vtype(args[0]) == kTypeObject) net_server_apply_options(js, server, args[0]);
  if (nargs > 0 && is_callable(args[0])) eventemitter_add_listener(js, server->obj, "connection", args[0], false);
  else if (nargs > 1 && is_callable(args[1])) eventemitter_add_listener(js, server->obj, "connection", args[1], false);

  return server->obj;
}

static ant_value_t net_server_bind_listener(
  ant_t *js,
  net_server_t *server,
  const net_listen_args_t *parsed,
  const ant_listener_callbacks_t *callbacks
) {
  uv_loop_t *loop = js->loop;
  int rc = 0;

  if (parsed->path) {
    server->path = strdup(parsed->path);
    if (!server->path) return Ant_Error_CreateOutOfMemory(js);
    
    rc = ant_listener_listen_pipe(
      &server->listener, loop, server->path, 
      server->backlog, 0, callbacks, server
    );
  } else {
    const char *host = parsed->host;
    net_server_parse_host(host, &host);
    
    free(server->host);
    server->host = strdup(host ? host : "0.0.0.0");
    if (!server->host) return Ant_Error_CreateOutOfMemory(js);
    
    rc = ant_listener_listen_tcp(
      &server->listener, loop, server->host, parsed->port, 
      server->backlog, 0, callbacks, server
    );
  }

  if (rc != 0) return js_mkerr_typed(js, JS_ERR_TYPE, "%s", uv_strerror(rc));
  return Ant_Value_Undefined;
}

static void net_server_take_over(ant_t *js, net_server_t *server, const net_listen_args_t *parsed) {
  for (net_server_t *old = js->net_servers, *next; old; old = next) {
    next = old->next;
    if (old == server || !old->listening || old->closing || old->generation >= server->generation) continue;

    bool same = parsed->path
      ? old->path && strcmp(old->path, parsed->path) == 0
      : !old->path && parsed->port != 0 && old->port == parsed->port;
    if (!same) continue;

    old->closing = true;
    ant_listener_stop(&old->listener, false);
    net_server_maybe_finish_close(old);
  }
}

static ant_value_t net_server_emit_listening(ant_params_t) {
  ant_value_t obj = js_get_slot(call_func, SLOT_DATA);
  net_server_t *server = net_server_data(obj);
  if (server && server->listening) eventemitter_emit_args(js, obj, "listening", NULL, 0);
  return Ant_Value_Undefined;
}

static ant_value_t js_net_server_listen(ant_params_t) {
  net_server_t *server = net_require_server(js, call_this);
  ant_listener_callbacks_t callbacks = {0};
  net_listen_args_t parsed;

  if (!server) return Ant_Exception_Current(js);
  if (server->listening) return js_mkerr_typed(js, JS_ERR_TYPE, "Server is already listening");
  if (!net_parse_listen_args(js, args, nargs, &parsed)) return parsed.error;
  
  if (!parsed.path && parsed.port == 0 && Ant_Net_ListenPolicy_Restricted(js)) return js_mkerr_typed(
    js, JS_ERR_TYPE | JS_ERR_NO_STACK,
    "sandbox listen on port 0 is not supported; use a fixed port with --forward <port>"
  );
  
  if (!parsed.path && !Ant_Net_ListenPolicy_PortAllowed(js, parsed.port)) return js_mkerr_typed(
    js, JS_ERR_TYPE | JS_ERR_NO_STACK,
    "sandbox listen on port %d requires --forward %d or --forward <host>:%d",
    parsed.port, parsed.port, parsed.port
  );

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
  
  server->generation = ant_watch_generation(js);
  net_server_take_over(js, server, &parsed);
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

  if (is_callable(parsed.callback)) eventemitter_add_listener(js, server->obj, "listening", parsed.callback, true);
  ant_value_t emit = js_make_data_cfunc(js, server->obj, net_server_emit_listening);
  if (!is_err(emit)) queue_next_tick(js, emit);
  GC_ROOT_RESTORE(js, root_mark);
  
  return call_this;
}

static ant_value_t js_net_server_close(ant_params_t) {
  net_server_t *server = net_require_server(js, call_this);

  if (!server) return Ant_Exception_Current(js);
  if (nargs > 0 && is_callable(args[0])) eventemitter_add_listener(js, server->obj, "close", args[0], true);

  if (!server->listening && !server->closing) {
    if (nargs > 0 && is_callable(args[0])) {
      ant_value_t err = Ant_Error_Create(js, JS_ERR_TYPE, "Server is not running");
      sv_call_value(js, args[0], Ant_Value_Undefined, &err, 1);
    }
    return call_this;
  }

  server->closing = true;
  ant_listener_stop(&server->listener, false);
  net_server_maybe_finish_close(server);
  return call_this;
}

static ant_value_t net_server_async_dispose_done(ant_params_t) {
  ant_value_t promise = js_func_data(call_func);
  if (nargs > 0 && is_object_type(args[0])) js_reject_promise(js, promise, args[0]);
  else js_resolve_promise(js, promise, Ant_Value_Undefined);
  return Ant_Value_Undefined;
}

static ant_value_t js_net_server_async_dispose(ant_params_t) {
  ant_value_t promise = js_mkpromise(js);

  net_server_t *server = net_server_data(call_this);
  if (!server || !server->listening) {
    js_resolve_promise(js, promise, Ant_Value_Undefined);
    return promise;
  }

  ant_value_t done = js_heavy_mkfun(js, net_server_async_dispose_done, promise);
  ant_value_t result = sv_call_value(js, js_get(js, call_this, "close"), call_this, &done, 1);
  if (is_err(result)) js_reject_promise(js, promise, result);

  return promise;
}

static ant_value_t js_net_server_address(ant_params_t) {
  net_server_t *server = net_require_server(js, call_this);
  ant_value_t out = Ant_Value_Null;
  
  if (!server) return Ant_Exception_Current(js);
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

    out = Ant_Object_CreateBare(js);
    js_set(js, out, "port", Ant_Value_FromNumber((double)port));
    js_set(js, out, "family", js_mkstr(js, family, strlen(family)));
    js_set(js, out, "address", js_mkstr(js, addr_str, strlen(addr_str)));
    
    return out;
  }

  struct in6_addr addr6;
  out = Ant_Object_CreateBare(js);
  js_set(js, out, "port", Ant_Value_FromNumber(server->port));
  
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

static ant_value_t js_net_server_getConnections(ant_params_t) {
  net_server_t *server = net_require_server(js, call_this);
  ant_value_t cb = nargs > 0 ? args[0] : Ant_Value_Undefined;
  ant_value_t argv[2] = { Ant_Value_Null, Ant_Value_FromNumber((double)net_server_socket_count(server)) };

  if (!server) return Ant_Exception_Current(js);
  if (is_callable(cb)) sv_call_value(js, cb, Ant_Value_Undefined, argv, 2);
  return call_this;
}

static ant_value_t js_net_server_ref(ant_params_t) {
  net_server_t *server = net_require_server(js, call_this);
  if (!server) return Ant_Exception_Current(js);
  if (server) ant_listener_ref(&server->listener);
  return call_this;
}

static ant_value_t js_net_server_unref(ant_params_t) {
  net_server_t *server = net_require_server(js, call_this);
  if (!server) return Ant_Exception_Current(js);
  if (server) ant_listener_unref(&server->listener);
  return call_this;
}

static ant_value_t js_net_createServer(ant_params_t) {
  return js_net_server_ctor(js, args, nargs, Ant_Value_Undefined, call_this, call_func);
}

static ant_value_t js_net_createConnection(ant_params_t) {
  net_connect_args_t parsed;
  net_socket_t *socket = NULL;

  if (!net_parse_connect_args(js, args, nargs, &parsed)) return parsed.error;
  socket = net_socket_create(js, parsed.allow_half_open, Ant_Value_Undefined);
  if (!socket) return Ant_Error_CreateOutOfMemory(js);

  return net_socket_connect_parsed(js, socket, &parsed);
}

static ant_value_t js_net_connect(ant_params_t) {
  return js_net_createConnection(js, args, nargs, Ant_Value_Undefined, call_this, call_func);
}

static ant_value_t js_net_getDefaultAutoSelectFamily(ant_params_t) {
  return Ant_Value_FromBool(js->defaults.net_auto_select_family);
}

static ant_value_t js_net_setDefaultAutoSelectFamily(ant_params_t) {
  if (nargs > 0) js->defaults.net_auto_select_family = js_truthy(js, args[0]);
  return Ant_Value_Undefined;
}

static ant_value_t js_net_getDefaultAutoSelectFamilyAttemptTimeout(ant_params_t) {
  return Ant_Value_FromNumber(js->defaults.net_auto_select_family_timeout);
}

static ant_value_t js_net_setDefaultAutoSelectFamilyAttemptTimeout(ant_params_t) {
  if (nargs > 0 && vtype(args[0]) == kTypeNumber) {
    double value = Ant_Value_AsNumberUnchecked(args[0]);
    if (value > 0 && value < 10) value = 10;
    if (value > 0) js->defaults.net_auto_select_family_timeout = value;
  }
  return Ant_Value_Undefined;
}

static void net_init_constructors(ant_t *js) {
  ant_value_t events = 0;
  ant_value_t ee_ctor = 0;
  ant_value_t ee_proto = 0;
  ant_value_t duplex_proto = 0;

  if (js->builtins.net_server_ctor && js->builtins.net_socket_ctor) return;

  events = events_library(js);
  ee_ctor = js_get(js, events, "EventEmitter");
  ee_proto = js_get(js, ee_ctor, "prototype");
  duplex_proto = stream_duplex_prototype(js);

  js->builtins.net_socket_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.net_socket_proto, is_object_type(duplex_proto) ? duplex_proto : ee_proto);
  js_set(js, js->builtins.net_socket_proto, "address", Ant_Function_Create(js_net_socket_address));
  js_set(js, js->builtins.net_socket_proto, "pause", Ant_Function_Create(js_net_socket_pause));
  js_set(js, js->builtins.net_socket_proto, "resume", Ant_Function_Create(js_net_socket_resume));
  js_set(js, js->builtins.net_socket_proto, "_read", Ant_Function_CreateWithArity(js_net_socket__read, 1));
  js_set(js, js->builtins.net_socket_proto, "setTimeout", Ant_Function_CreateWithArity(js_net_socket_setTimeout, 2));
  js_set(js, js->builtins.net_socket_proto, "setNoDelay", Ant_Function_CreateWithArity(js_net_socket_setNoDelay, 1));
  js_set(js, js->builtins.net_socket_proto, "setKeepAlive", Ant_Function_CreateWithArity(js_net_socket_setKeepAlive, 2));
  js_set(js, js->builtins.net_socket_proto, "write", Ant_Function_Create(js_net_socket_write));
  js_set(js, js->builtins.net_socket_proto, "end", Ant_Function_CreateWithArity(js_net_socket_end, 3));
  js_set(js, js->builtins.net_socket_proto, "destroy", Ant_Function_Create(js_net_socket_destroy));
  js_set(js, js->builtins.net_socket_proto, "connect", Ant_Function_Create(js_net_socket_connect));
  js_set(js, js->builtins.net_socket_proto, "ref", Ant_Function_Create(js_net_socket_ref));
  js_set(js, js->builtins.net_socket_proto, "unref", Ant_Function_Create(js_net_socket_unref));
  js_set_tostringtag(js, js->builtins.net_socket_proto, js_mkstr(js, "Socket", 6));
  js->builtins.net_socket_ctor = Ant_Function_CreateConstructor(js, js_net_socket_ctor, js->builtins.net_socket_proto, "Socket", 1);

  js->builtins.net_server_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.net_server_proto, ee_proto);
  js_set(js, js->builtins.net_server_proto, "listen", Ant_Function_Create(js_net_server_listen));
  js_set(js, js->builtins.net_server_proto, "close", Ant_Function_CreateWithArity(js_net_server_close, 1));
  js_set(js, js->builtins.net_server_proto, "address", Ant_Function_Create(js_net_server_address));
  js_set(js, js->builtins.net_server_proto, "getConnections", Ant_Function_CreateWithArity(js_net_server_getConnections, 1));
  js_set(js, js->builtins.net_server_proto, "ref", Ant_Function_Create(js_net_server_ref));
  js_set(js, js->builtins.net_server_proto, "unref", Ant_Function_Create(js_net_server_unref));
  js_set_sym(js, js->builtins.net_server_proto, js->sym.asyncDispose_sym, Ant_Function_Create(js_net_server_async_dispose));
  js_set_tostringtag(js, js->builtins.net_server_proto, js_mkstr(js, "Server", 6));
  js->builtins.net_server_ctor = Ant_Function_CreateConstructor(js, js_net_server_ctor, js->builtins.net_server_proto, "Server", 2);
}

ant_value_t net_library(ant_t *js) {
  ant_value_t lib = Ant_Object_CreateBare(js);

  net_init_constructors(js);
  js_set(js, lib, "Server", js->builtins.net_server_ctor);
  js_set(js, lib, "Socket", js->builtins.net_socket_ctor);
  js_set(js, lib, "createServer", Ant_Function_CreateWithArity(js_net_createServer, 2));
  js_set(js, lib, "createConnection", Ant_Function_Create(js_net_createConnection));
  js_set(js, lib, "connect", Ant_Function_Create(js_net_connect));
  js_set(js, lib, "isIP", Ant_Function_CreateWithArity(net_isIP, 1));
  js_set(js, lib, "isIPv4", Ant_Function_CreateWithArity(net_isIPv4, 1));
  js_set(js, lib, "isIPv6", Ant_Function_CreateWithArity(net_isIPv6, 1));
  js_set(js, lib, "getDefaultAutoSelectFamily", Ant_Function_Create(js_net_getDefaultAutoSelectFamily));
  js_set(js, lib, "setDefaultAutoSelectFamily", Ant_Function_CreateWithArity(js_net_setDefaultAutoSelectFamily, 1));
  js_set(js, lib, "getDefaultAutoSelectFamilyAttemptTimeout", Ant_Function_Create(js_net_getDefaultAutoSelectFamilyAttemptTimeout));
  js_set(js, lib, "setDefaultAutoSelectFamilyAttemptTimeout", Ant_Function_CreateWithArity(js_net_setDefaultAutoSelectFamilyAttemptTimeout, 1));
  js_set(js, lib, "default", lib);
  js_set_tostringtag(js, lib, js_mkstr(js, "net", 3));
  
  return lib;
}

void cleanup_net_module(ant_t *js) {
  for (net_socket_t *socket = js->net_sockets; socket; socket = socket->next)
    if (socket->conn && !ant_conn_is_closing(socket->conn)) ant_conn_close(socket->conn);

  for (net_server_t *server = js->net_servers; server; server = server->next) {
    server->closing = true;
    ant_listener_stop(&server->listener, true);
  }
}

void gc_mark_net(ant_t *js, gc_mark_fn mark) {
  net_server_t *server = NULL;
  net_socket_t *socket = NULL;

  for (server = js->net_servers; server; server = server->next) mark(js, server->obj);
  for (socket = js->net_sockets; socket; socket = socket->next) mark(js, socket->obj);
}
