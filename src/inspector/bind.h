#ifndef ANT_INSPECTOR_BIND_H
#define ANT_INSPECTOR_BIND_H

#include "types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uv.h>
#include <yyjson.h>

typedef struct inspector_client inspector_client_t;
typedef struct inspector_script inspector_script_t;

typedef struct inspector_object_handle inspector_object_handle_t;
typedef struct inspector_network_entry inspector_network_entry_t;
typedef struct inspector_console_event inspector_console_event_t;

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} sbuf_t;

struct inspector_client {
  uv_tcp_t handle;
  ant_t *js;
  bool websocket;
  bool runtime_enabled;
  bool console_enabled;
  bool console_replayed;
  bool debugger_enabled;
  bool network_enabled;
  char *read_buf;
  size_t read_len;
  size_t read_cap;
  inspector_client_t *next;
};

struct inspector_object_handle {
  uint32_t id;
  ant_value_t value;
  inspector_object_handle_t *next;
};

struct inspector_network_entry {
  uint64_t id;
  uint8_t *request_body;
  size_t request_body_len;
  bool request_body_truncated;
  uint8_t *response_body;
  size_t response_body_len;
  bool response_body_truncated;
  inspector_network_entry_t *next;
};

struct inspector_console_event {
  char *json;
  inspector_console_event_t *next;
};

struct inspector_script {
  int id;
  char *url;
  char *source;
  size_t source_len;
  bool is_module;
  inspector_script_t *next;
};

typedef struct {
  ant_t *js;
  uv_tcp_t server;
  bool started;
  bool attached;
  bool waiting_for_debugger;
  char host[64];
  int port;
  char uuid[37];
  inspector_client_t *clients;
  inspector_object_handle_t *object_handles;
  inspector_network_entry_t *network_entries;
  inspector_console_event_t *console_events_head;
  inspector_console_event_t *console_events_tail;
  inspector_script_t *scripts;
  size_t console_event_count;
  size_t network_entry_count;
  uint32_t next_object_id;
  uint64_t next_network_request_id;
  int next_script_id;
  int entry_script_id;
} inspector_state_t;

typedef struct {
  uv_write_t req;
  uv_buf_t buf;
} inspector_write_t;

extern inspector_state_t g_inspector;

extern const size_t k_inspector_network_body_limit;
extern const size_t k_inspector_network_entry_limit;
extern const size_t k_inspector_network_write_queue_limit;
extern const size_t k_inspector_console_event_limit;

bool sbuf_append_len(sbuf_t *b, const char *s, size_t len);
bool sbuf_append(sbuf_t *b, const char *s);
bool sbuf_appendf(sbuf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

bool sbuf_json_string_len(sbuf_t *b, const char *s, size_t len);
bool sbuf_json_string(sbuf_t *b, const char *s);

void inspector_send_raw(inspector_client_t *client, const char *data, size_t len);
void inspector_send_ws(inspector_client_t *client, const char *json);
void inspector_send_response_obj(inspector_client_t *client, int id, const char *result_obj);
void inspector_send_empty_result(inspector_client_t *client, int id);
void inspector_send_error(inspector_client_t *client, int id, int code, const char *message);
bool inspector_param_bool(yyjson_val *params, const char *name);

double inspector_timestamp_seconds(void);
double inspector_wall_time_seconds(void);

bool inspector_is_remote_handle_value(ant_value_t value);
bool inspector_value_to_remote_object(ant_t *js, ant_value_t value, sbuf_t *out);
void inspector_send_execution_context(inspector_client_t *client);
void inspector_eval(inspector_client_t *client, int id, yyjson_val *params);
void inspector_get_heap_usage(inspector_client_t *client, int id);
void inspector_get_properties(inspector_client_t *client, int id, yyjson_val *params);
void inspector_global_lexical_scope_names(inspector_client_t *client, int id);
void inspector_clear_console_events(void);
void inspector_replay_console_events(inspector_client_t *client);
void inspector_clear_exception_state(ant_t *js);
void inspector_send_eval_result(inspector_client_t *client, int id, ant_value_t result);

bool inspector_is_url_like(const char *path);
char *inspector_make_script_url(const char *path);

inspector_script_t *inspector_script_for_id(int id);
inspector_script_t *inspector_script_for_url(const char *url);
inspector_script_t *inspector_entry_script(void);

void inspector_send_registered_scripts(inspector_client_t *client);
bool inspector_append_call_location(ant_t *js, sbuf_t *b);
void inspector_compile_script(inspector_client_t *client, int id, yyjson_val *params);
void inspector_run_script(inspector_client_t *client, int id, yyjson_val *params);
void inspector_get_script_source(inspector_client_t *client, int id, yyjson_val *params);

void inspector_get_response_body(inspector_client_t *client, int id, yyjson_val *params);
void inspector_get_request_post_data(inspector_client_t *client, int id, yyjson_val *params);
void inspector_handle_message(inspector_client_t *client, const char *payload, size_t len);

#endif
