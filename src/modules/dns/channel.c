#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "errors.h"
#include "list.h"
#include "gc/uv.h"
#include "gc/roots.h"
#include "dns_internal.h"

static void dns_channel_handle_closed(dns_channel_t *channel) {
  if (--channel->open_handles == 0 && channel->destroyed) free(channel);
}

static void dns_poll_close_cb(uv_handle_t *handle) {
  dns_poll_t *poll = handle->data;
  dns_channel_t *channel = poll->channel;
  free(poll);
  dns_channel_handle_closed(channel);
}

static void dns_timer_close_cb(uv_handle_t *handle) {
  dns_channel_handle_closed(handle->data);
}

static void dns_channel_set_ref(uv_handle_t *handle, bool ref) {
  if (ref) uv_ref(handle);
  else uv_unref(handle);
}

static void dns_channel_refresh_refs(dns_channel_t *channel) {
  bool ref = channel->queries > 0;
  dns_channel_set_ref((uv_handle_t *)&channel->timer, ref);
  for (dns_poll_t *poll = channel->polls; poll; poll = poll->next) dns_channel_set_ref((uv_handle_t *)&poll->handle, ref);
}

static void dns_timer_cb(uv_timer_t *timer) {
  GC_UV_CALLBACK();
  dns_channel_t *channel = timer->data;
  if (!channel->ares) return;
  ares_process_fd(channel->ares, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
  dns_channel_schedule(channel);
}

void dns_channel_schedule(dns_channel_t *channel) {
  if (!channel->ares || channel->destroyed) return;
  struct timeval tv;

  if (!ares_timeout(channel->ares, NULL, &tv)) {
    uv_timer_stop(&channel->timer);
    return;
  }

  uint64_t ms = (uint64_t)tv.tv_sec * 1000 + ((uint64_t)tv.tv_usec + 999) / 1000;
  uv_timer_start(&channel->timer, dns_timer_cb, ms, 0);
}

static void dns_poll_cb(uv_poll_t *handle, int status, int events) {
  GC_UV_CALLBACK();
  dns_poll_t *poll = handle->data;
  dns_channel_t *channel = poll->channel;
  if (!channel->ares) return;

  ares_socket_t read_fd = status < 0 || (events & UV_READABLE) ? poll->fd : ARES_SOCKET_BAD;
  ares_socket_t write_fd = status < 0 || (events & UV_WRITABLE) ? poll->fd : ARES_SOCKET_BAD;
  ares_process_fd(channel->ares, read_fd, write_fd);
  dns_channel_schedule(channel);
}

static void dns_sock_state_cb(void *data, ares_socket_t fd, int readable, int writable) {
  dns_channel_t *channel = data;
  dns_poll_t **link = &channel->polls;
  dns_poll_t *poll = NULL;

  for (; *link; link = &(*link)->next) if ((*link)->fd == fd) {
    poll = *link;
    break;
  }

  if (!readable && !writable) {
    if (!poll) return;
    *link = poll->next;
    uv_poll_stop(&poll->handle);
    uv_close((uv_handle_t *)&poll->handle, dns_poll_close_cb);
    return;
  }

  if (!poll) {
    poll = calloc(1, sizeof(*poll));
    if (!poll) return;

    poll->channel = channel;
    poll->fd = fd;

    if (uv_poll_init_socket(channel->js->loop, &poll->handle, (uv_os_sock_t)fd) != 0) {
      free(poll);
      return;
    }

    poll->handle.data = poll;
    poll->next = channel->polls;
    channel->polls = poll;
    channel->open_handles++;
    dns_channel_set_ref((uv_handle_t *)&poll->handle, channel->queries > 0);
  }

  uv_poll_start(&poll->handle, (readable ? UV_READABLE : 0) | (writable ? UV_WRITABLE : 0), dns_poll_cb);
}

dns_channel_t *dns_channel_create(ant_t *js, int timeout_ms, int tries) {
  dns_channel_t *channel = calloc(1, sizeof(*channel));
  if (!channel) return NULL;
  channel->js = js;

  struct ares_options options = {
    .flags = ARES_FLAG_NOCHECKRESP,
    .sock_state_cb = dns_sock_state_cb,
    .sock_state_cb_data = channel,
    .qcache_max_ttl = 0,
  };

  int mask = ARES_OPT_FLAGS | ARES_OPT_SOCK_STATE_CB | ARES_OPT_QUERY_CACHE;
  if (timeout_ms >= 0) { options.timeout = timeout_ms; mask |= ARES_OPT_TIMEOUTMS; }
  if (tries > 0) { options.tries = tries; mask |= ARES_OPT_TRIES; }

  if (ares_library_init(ARES_LIB_INIT_ALL) != ARES_SUCCESS) {
    free(channel);
    return NULL;
  }

  if (ares_init_options(&channel->ares, &options, mask) != ARES_SUCCESS) {
    ares_library_cleanup();
    free(channel);
    return NULL;
  }

  uv_timer_init(js->loop, &channel->timer);
  channel->timer.data = channel;
  channel->open_handles = 1;
  uv_unref((uv_handle_t *)&channel->timer);

  Ant_List_Insert(&js->dns_channels, channel);
  return channel;
}

dns_channel_t *dns_default_channel(ant_t *js) {
  if (!js->dns_default_channel) js->dns_default_channel = dns_channel_create(js, -1, 0);
  return js->dns_default_channel;
}

void dns_channel_query_started(dns_channel_t *channel) {
  if (channel->queries++ == 0) dns_channel_refresh_refs(channel);
}

void dns_channel_query_finished(dns_channel_t *channel) {
  if (--channel->queries == 0 && !channel->destroyed) dns_channel_refresh_refs(channel);
}

void dns_channel_destroy(dns_channel_t *channel) {
  if (!channel || channel->destroyed) return;
  ant_t *js = channel->js;

  channel->destroyed = true;
  if (channel->owner_ref) *channel->owner_ref = NULL;
  Ant_List_Remove(channel);
  if (js->dns_default_channel == channel) js->dns_default_channel = NULL;

  if (channel->ares) {
    ares_destroy(channel->ares);
    channel->ares = NULL;
    ares_library_cleanup();
  }

  while (channel->polls) {
    dns_poll_t *poll = channel->polls;
    channel->polls = poll->next;
    uv_poll_stop(&poll->handle);
    uv_close((uv_handle_t *)&poll->handle, dns_poll_close_cb);
  }

  uv_timer_stop(&channel->timer);
  uv_close((uv_handle_t *)&channel->timer, dns_timer_close_cb);
}

ant_value_t dns_channel_get_servers(ant_t *js, dns_channel_t *channel) {
  ant_value_t list = Ant_Array_Create(js);
  if (!channel || !channel->ares) return list;

  char *csv = ares_get_servers_csv(channel->ares);
  if (!csv) return list;

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, list);

  for (char *entry = csv, *next; entry && *entry; entry = next) {
    next = strchr(entry, ',');
    if (next) *next++ = '\0';

    char *host = entry, *port = NULL;
    bool bracketed = entry[0] == '[';

    if (bracketed) {
      char *close = strchr(entry, ']');
      if (!close) continue;
      *close = '\0';
      host = entry + 1;
      if (close[1] == ':') port = close + 2;
    } else if ((port = strrchr(entry, ':'))) *port++ = '\0';

    char out[INET6_ADDRSTRLEN + 32];
    bool default_port = !port || strcmp(port, "53") == 0;
    if (default_port) snprintf(out, sizeof(out), "%s", host);
    else snprintf(out, sizeof(out), bracketed ? "[%s]:%s" : "%s:%s", host, port);
    js_arr_push(js, list, js_mkstr(js, out, strlen(out)));
  }

  ares_free_string(csv);
  GC_ROOT_RESTORE(js, mark);
  return list;
}

static bool dns_server_to_csv(const char *text, size_t len, char *out, size_t out_len) {
  char host[INET6_ADDRSTRLEN + 16], port[8] = "53";
  const char *colon = memchr(text, ':', len);
  const char *last_colon = NULL;
  for (size_t i = 0; i < len; i++) if (text[i] == ':') last_colon = text + i;

  if (len > 0 && text[0] == '[') {
    const char *close = memchr(text, ']', len);
    if (!close || (size_t)(close - text - 1) >= sizeof(host)) return false;
    memcpy(host, text + 1, (size_t)(close - text - 1));
    host[close - text - 1] = '\0';

    size_t rest = len - (size_t)(close - text) - 1;
    if (rest) {
      if (close[1] != ':' || rest - 1 == 0 || rest - 1 >= sizeof(port)) return false;
      memcpy(port, close + 2, rest - 1);
      port[rest - 1] = '\0';
    }

    if (dns_ip_family(host) != 6) return false;
  } else if (colon && colon == last_colon) {
    size_t host_len = (size_t)(colon - text), port_len = len - host_len - 1;
    if (host_len >= sizeof(host) || port_len == 0 || port_len >= sizeof(port)) return false;
    memcpy(host, text, host_len);
    host[host_len] = '\0';
    memcpy(port, colon + 1, port_len);
    port[port_len] = '\0';
    if (dns_ip_family(host) != 4) return false;
  } else {
    if (len >= sizeof(host)) return false;
    memcpy(host, text, len);
    host[len] = '\0';
    if (!dns_ip_family(host)) return false;
  }

  for (const char *p = port; *p; p++) if (*p < '0' || *p > '9') return false;
  int family = dns_ip_family(host);
  return snprintf(out, out_len, family == 6 ? "[%s]:%s" : "%s:%s", host, port) < (int)out_len;
}

ant_value_t dns_channel_set_servers(ant_t *js, dns_channel_t *channel, ant_value_t servers) {
  if (vtype(servers) != kTypeArray) return Ant_Error_CreateInvalidArgType(js, "servers", "an instance of Array", servers);
  if (!channel || !channel->ares) return js_mkerr(js, "DNS channel is not available");

  ant_offset_t count = js_arr_len(js, servers);
  size_t cap = (size_t)count * (INET6_ADDRSTRLEN + 16) + 1, used = 0;
  char *csv = malloc(cap);
  if (!csv) return Ant_Error_CreateOutOfMemory(js);
  csv[0] = '\0';

  for (ant_offset_t i = 0; i < count; i++) {
    ant_value_t entry = js_arr_get(js, servers, i);
    if (vtype(entry) != kTypeString) {
      free(csv);
      char name[32];
      snprintf(name, sizeof(name), "servers[%u]", (unsigned)i);
      return Ant_Error_CreateInvalidArgType(js, name, "of type string", entry);
    }

    size_t len = 0;
    const char *text = js_getstr(js, entry, &len);
    char server[INET6_ADDRSTRLEN + 16];

    if (!dns_server_to_csv(text, len, server, sizeof(server))) {
      free(csv);
      char message[320];
      snprintf(message, sizeof(message), "Invalid IP address: %.*s", (int)len, text);
      return js_mkerr_code(js, JS_ERR_TYPE, "ERR_INVALID_IP_ADDRESS", message);
    }

    used += (size_t)snprintf(csv + used, cap - used, "%s%s", used ? "," : "", server);
  }

  int status = ares_set_servers_ports_csv(channel->ares, csv);
  free(csv);

  if (status != ARES_SUCCESS) {
    char message[256];
    snprintf(message, sizeof(message), "c-ares failed to set servers: \"%s\"", ares_strerror(status));
    return js_mkerr_code(js, JS_ERR_GENERIC, "ERR_DNS_SET_SERVERS_FAILED", message);
  }

  return Ant_Value_Undefined;
}
