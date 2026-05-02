#include <compat.h> // IWYU pragma: keep

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <uv.h>
#include <utarray.h>

#include "ant.h"
#include "ptr.h"
#include "common.h"
#include "errors.h"
#include "internal.h"
#include "runtime.h"
#include "esm/remote.h"
#include "gc/modules.h"
#include "modules/abort.h"
#include "modules/assert.h"
#include "modules/buffer.h"
#include "modules/fetch.h"
#include "modules/headers.h"
#include "modules/http.h"
#include "modules/request.h"
#include "modules/response.h"
#include "modules/url.h"
#include "streams/readable.h"

typedef struct fetch_request_s {
  ant_t *js;
  
  ant_value_t promise;
  ant_value_t request_obj;
  ant_value_t response_obj;
  ant_value_t abort_listener;
  ant_value_t upload_reader;
  ant_value_t upload_read_promise;
  ant_http_request_t *http_req;
  
  int refs;
  int redirect_count;
  bool settled;
  bool aborted;
  bool restart_pending;
  bool response_started;
} fetch_request_t;

enum { FETCH_REQUEST_NATIVE_TAG = 0x46524551u }; // FREQ

static UT_array *pending_requests = NULL;
static const int k_fetch_max_redirects = 20;
static void fetch_start_http(fetch_request_t *req);

static void fetch_request_retain(fetch_request_t *req) {
  if (req) req->refs++;
}

static void remove_pending_request(fetch_request_t *req) {
  if (!req || !pending_requests) return;

  fetch_request_t **p = NULL;
  unsigned int i = 0;
  
  while ((p = (fetch_request_t **)utarray_next(pending_requests, p))) {
    if (*p == req) { utarray_erase(pending_requests, i, 1); return; } i++;
  }
}

static void destroy_fetch_request(fetch_request_t *req) {
  ant_value_t signal = 0;

  if (!req) return;
  signal = request_get_signal(req->js, req->request_obj);

  if (abort_signal_is_signal(signal) && is_callable(req->abort_listener))
    abort_signal_remove_listener(req->js, signal, req->abort_listener);

  remove_pending_request(req);
  free(req);
}

static void fetch_request_release(fetch_request_t *req) {
  if (!req) return;
  if (--req->refs == 0) destroy_fetch_request(req);
}

static ant_value_t fetch_type_error(ant_t *js, const char *message) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "%s", message ? message : "fetch failed");
}

static ant_value_t fetch_rejection_reason(ant_t *js, ant_value_t value) {
  if (!is_err(value)) return value;
  if (js->thrown_exists) {
    ant_value_t reason = js->thrown_value;
    js->thrown_exists = false;
    js->thrown_value = js_mkundef();
    js->thrown_stack = js_mkundef();
    return reason;
  }
  return value;
}

static bool fetch_is_redirect_status(int status) {
  return 
    status == 301 ||
    status == 302 ||
    status == 303 ||
    status == 307 ||
    status == 308;
}

static void fetch_cancel_request_body(fetch_request_t *req, ant_value_t reason) {
  request_data_t *data = request_get_data(req->request_obj);
  ant_value_t stream = js_get_slot(req->request_obj, SLOT_REQUEST_BODY_STREAM);

  if (!data || !data->body_is_stream || !rs_is_stream(stream)) return;
  readable_stream_cancel(req->js, stream, reason);
}

static void fetch_error_response_body(fetch_request_t *req, ant_value_t reason) {
  ant_value_t stream = js_get_slot(req->response_obj, SLOT_RESPONSE_BODY_STREAM);
  if (rs_is_stream(stream)) readable_stream_error(req->js, stream, reason);
}

static void fetch_reject(fetch_request_t *req, ant_value_t reason) {
  if (!req) return;

  if (!req->settled) {
    req->settled = true;
    js_reject_promise(req->js, req->promise, reason);
  }

  fetch_cancel_request_body(req, reason);
  if (is_object_type(req->response_obj)) fetch_error_response_body(req, reason);
}

static void fetch_resolve(fetch_request_t *req, ant_value_t response_obj) {
  if (!req || req->settled) return;
  req->settled = true;
  req->response_started = true;
  req->response_obj = response_obj;
  js_resolve_promise(req->js, req->promise, response_obj);
}

static bool fetch_is_http_url(const char *url) {
  return strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0;
}

static char *fetch_build_request_url(request_data_t *request) {
  if (!request) return NULL;
  return build_href(&request->url);
}

static const char *fetch_find_header_value(const ant_http_header_t *headers, const char *name) {
  for (const ant_http_header_t *entry = headers; entry; entry = entry->next) {
    if (entry->name && strcasecmp(entry->name, name) == 0) return entry->value;
  }
  return NULL;
}

static bool fetch_redirect_rewrites_to_get(int status, const char *method) {
  if (!method) return false;
  if (status == 303) return strcasecmp(method, "HEAD") != 0;
  return (status == 301 || status == 302) && strcasecmp(method, "POST") == 0;
}

typedef struct {
  ant_t *js;
  ant_value_t headers;
  bool drop_body_headers;
  bool failed;
} fetch_redirect_headers_ctx_t;

static void fetch_copy_redirect_header(const char *name, const char *value, void *ctx) {
  fetch_redirect_headers_ctx_t *copy = (fetch_redirect_headers_ctx_t *)ctx;
  ant_value_t step = 0;

  if (!copy || copy->failed) return;
  if (copy->drop_body_headers && name && strcasecmp(name, "content-type") == 0) return;

  step = headers_append_literal(copy->js, copy->headers, name, value);
  if (is_err(step)) copy->failed = true;
}

static ant_value_t fetch_replace_request_headers(fetch_request_t *req, bool drop_body_headers) {
  ant_t *js = req->js;
  
  request_data_t *request = request_get_data(req->request_obj);
  ant_value_t current = request_get_headers(req->request_obj);
  ant_value_t headers = headers_create_empty(js);
  
  fetch_redirect_headers_ctx_t ctx = {
    .js = js,
    .headers = headers,
    .drop_body_headers = drop_body_headers,
    .failed = false,
  };

  if (is_err(headers)) return headers;
  headers_for_each(current, fetch_copy_redirect_header, &ctx);
  if (ctx.failed) return js_mkerr(js, "out of memory");

  headers_set_guard(headers,
    strcmp(request->mode, "no-cors") == 0
    ? HEADERS_GUARD_REQUEST_NO_CORS
    : HEADERS_GUARD_REQUEST
  );
  
  headers_apply_guard(headers);
  js_set_slot_wb(js, req->request_obj, SLOT_REQUEST_HEADERS, headers);
  
  return js_mkundef();
}

static ant_value_t fetch_clear_redirect_request_body(fetch_request_t *req) {
  request_data_t *request = request_get_data(req->request_obj);
  ant_value_t headers_step = 0;
  
  if (!request)
    return fetch_type_error(req->js, "Invalid Request object");
  
  free(request->body_data);
  free(request->body_type);
  request->body_data = NULL;
  request->body_size = 0;
  request->body_type = NULL;
  request->body_is_stream = false;
  request->has_body = false;
  request->body_used = false;
  js_set_slot_wb(req->js, req->request_obj, SLOT_REQUEST_BODY_STREAM, js_mkundef());

  headers_step = fetch_replace_request_headers(req, true);
  if (is_err(headers_step)) return headers_step;
  
  return js_mkundef();
}

static ant_value_t fetch_set_redirect_method(fetch_request_t *req, const char *method) {
  request_data_t *request = request_get_data(req->request_obj);
  char *dup = NULL;

  if (!request) return fetch_type_error(req->js, "Invalid Request object");
  dup = strdup(method);
  if (!dup) return js_mkerr(req->js, "out of memory");
  free(request->method);
  request->method = dup;
  return js_mkundef();
}

static ant_value_t fetch_update_request_url(fetch_request_t *req, const char *location) {
  request_data_t *request = request_get_data(req->request_obj);
  url_state_t next = {0};
  char *base = NULL;

  if (!request || !location) return fetch_type_error(req->js, "Invalid redirect URL");
  base = fetch_build_request_url(request);
  if (!base) return fetch_type_error(req->js, "Invalid request URL");

  if (parse_url_to_state(location, base, &next) != 0) {
    free(base);
    url_state_clear(&next);
    return fetch_type_error(req->js, "Invalid redirect URL");
  }

  free(base);
  url_state_clear(&request->url);
  request->url = next;
  return js_mkundef();
}

static ant_value_t fetch_prepare_redirect(fetch_request_t *req, const ant_http_response_t *resp) {
  request_data_t *request = request_get_data(req->request_obj);
  const char *location = fetch_find_header_value(resp->headers, "location");
  ant_value_t step = 0;
  bool rewrite_to_get = false;

  if (!request || !location || location[0] == '\0') return js_mkundef();
  if (req->redirect_count >= k_fetch_max_redirects) {
    return fetch_type_error(req->js, "fetch failed: too many redirects");
  }

  rewrite_to_get = fetch_redirect_rewrites_to_get(resp->status, request->method);
  if (!rewrite_to_get && request->body_is_stream) {
    return fetch_type_error(req->js, "fetch failed: cannot follow redirect with a streamed request body");
  }

  if (rewrite_to_get) {
    step = fetch_set_redirect_method(req, strcasecmp(request->method, "HEAD") == 0 ? "HEAD" : "GET");
    if (is_err(step)) return step;
    step = fetch_clear_redirect_request_body(req);
    if (is_err(step)) return step;
  }

  step = fetch_update_request_url(req, location);
  if (is_err(step)) return step;

  req->redirect_count++;
  req->restart_pending = true;
  return js_mkundef();
}

typedef struct {
  ant_http_header_t *head;
  ant_http_header_t **tail;
  bool failed;
  bool has_user_agent;
  bool has_accept;
  bool has_accept_language;
  bool has_sec_fetch_mode;
  bool has_accept_encoding;
} fetch_header_builder_t;

static void fetch_collect_header(const char *name, const char *value, void *ctx) {
  fetch_header_builder_t *builder = (fetch_header_builder_t *)ctx;
  ant_http_header_t *header = NULL;

  if (!builder || builder->failed) return;
  if (name && strcasecmp(name, "user-agent") == 0) builder->has_user_agent = true;
  if (name && strcasecmp(name, "accept") == 0) builder->has_accept = true;
  if (name && strcasecmp(name, "accept-language") == 0) builder->has_accept_language = true;
  if (name && strcasecmp(name, "sec-fetch-mode") == 0) builder->has_sec_fetch_mode = true;
  if (name && strcasecmp(name, "accept-encoding") == 0) builder->has_accept_encoding = true;
  header = calloc(1, sizeof(ant_http_header_t));
  if (!header) {
    builder->failed = true;
    return;
  }

  header->name = strdup(name ? name : "");
  header->value = strdup(value ? value : "");
  if (!header->name || !header->value) {
    free(header->name);
    free(header->value);
    free(header);
    builder->failed = true;
    return;
  }

  *builder->tail = header;
  builder->tail = &header->next;
}

static bool fetch_append_header(fetch_header_builder_t *builder, const char *name, const char *value) {
  ant_http_header_t *header = NULL;

  if (!builder || builder->failed) return false;
  header = calloc(1, sizeof(ant_http_header_t));
  if (!header) {
    builder->failed = true;
    return false;
  }

  header->name = strdup(name);
  header->value = strdup(value);
  if (!header->name || !header->value) {
    free(header->name);
    free(header->value);
    free(header);
    builder->failed = true;
    return false;
  }

  *builder->tail = header;
  builder->tail = &header->next;
  return true;
}

static ant_http_header_t *fetch_build_http_headers(ant_value_t request_obj) {
  fetch_header_builder_t builder = {0};
  char user_agent[256] = {0};

  builder.tail = &builder.head;
  headers_for_each(request_get_headers(request_obj), fetch_collect_header, &builder);
  
  if (builder.failed) {
    ant_http_headers_free(builder.head);
    return NULL;
  }

  if (!builder.has_accept && !fetch_append_header(&builder, "accept", "*/*")) {
    ant_http_headers_free(builder.head);
    return NULL;
  }
  if (!builder.has_accept_language && !fetch_append_header(&builder, "accept-language", "*")) {
    ant_http_headers_free(builder.head);
    return NULL;
  }
  if (!builder.has_sec_fetch_mode && !fetch_append_header(&builder, "sec-fetch-mode", "cors")) {
    ant_http_headers_free(builder.head);
    return NULL;
  }
  if (!builder.has_accept_encoding && !fetch_append_header(&builder, "accept-encoding", "br, gzip, deflate")) {
    ant_http_headers_free(builder.head);
    return NULL;
  }
  if (builder.has_user_agent) return builder.head;

  snprintf(user_agent, sizeof(user_agent), "ant/%s", ANT_VERSION);
  if (!fetch_append_header(&builder, "user-agent", user_agent)) {
    ant_http_headers_free(builder.head);
    return NULL;
  }
  
  return builder.head;
}

static ant_value_t fetch_headers_from_http(ant_t *js, const ant_http_header_t *headers) {
  ant_value_t hdrs = headers_create_empty(js);
  if (is_err(hdrs)) return hdrs;

  for (const ant_http_header_t *entry = headers; entry; entry = entry->next) {
    ant_value_t step = headers_append_value(
      js, hdrs,
      js_mkstr(js, entry->name, strlen(entry->name)),
      js_mkstr(js, entry->value, strlen(entry->value))
    );
    if (is_err(step)) return step;
  }

  return hdrs;
}

static ant_value_t fetch_create_chunk(ant_t *js, const uint8_t *data, size_t len) {
  ArrayBufferData *ab = create_array_buffer_data(len);
  if (!ab) return js_mkerr(js, "out of memory");
  if (len > 0) memcpy(ab->data, data, len);
  return create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, len, "Uint8Array");
}

static bool fetch_get_upload_chunk(ant_value_t value, const uint8_t **out, size_t *len) {
  TypedArrayData *ta = buffer_get_typedarray_data(value);

  if (!ta || ta->type != TYPED_ARRAY_UINT8) return false;
  if (!ta->buffer || ta->buffer->is_detached) {
    *out = NULL;
    *len = 0;
    return true;
  }

  *out = ta->buffer->data + ta->byte_offset;
  *len = ta->byte_length;
  return true;
}

static void fetch_http_on_response(ant_http_request_t *http_req, const ant_http_response_t *resp, void *user_data) {
  fetch_request_t *req = (fetch_request_t *)user_data;
  
  ant_t *js = req->js;
  request_data_t *request = request_get_data(req->request_obj);
  
  ant_value_t headers = 0;
  ant_value_t step = 0;
  ant_value_t stream = 0;
  ant_value_t response = 0;
  
  char *url = NULL;
  if (req->aborted) return;
  if (!request) {
    fetch_reject(req, fetch_type_error(js, "Invalid Request object"));
    ant_http_request_cancel(http_req);
    return;
  }

  if (fetch_is_redirect_status(resp->status)) {
    const char *location = fetch_find_header_value(resp->headers, "location");
    const char *redirect_mode = request->redirect ? request->redirect : "follow";
    
    if (location && location[0] != '\0' && strcmp(redirect_mode, "error") == 0) {
      fetch_reject(req, fetch_type_error(js, "fetch failed: redirect mode is set to error"));
      ant_http_request_cancel(http_req);
      return;
    }
    
    if (strcmp(redirect_mode, "follow") == 0) {
    step = fetch_prepare_redirect(req, resp);
    
    if (is_err(step)) {
      fetch_reject(req, fetch_rejection_reason(js, step));
      ant_http_request_cancel(http_req);
      return;
    }
    
    if (req->restart_pending) {
      ant_http_request_cancel(http_req);
      return;
    }}
  }

  headers = fetch_headers_from_http(js, resp->headers);
  if (is_err(headers)) {
    fetch_reject(req, fetch_rejection_reason(js, headers));
    ant_http_request_cancel(http_req);
    return;
  }

  stream = rs_create_stream(js, js_mkundef(), js_mkundef(), 1.0);
  if (is_err(stream)) {
    fetch_reject(req, fetch_rejection_reason(js, stream));
    ant_http_request_cancel(http_req);
    return;
  }

  url = fetch_build_request_url(request_get_data(req->request_obj));
  response = response_create_fetched(
    js, resp->status, resp->status_text, url, 
    req->redirect_count + 1, headers, NULL, 0, stream, NULL
  );
  
  free(url);

  if (is_err(response)) {
    fetch_reject(req, fetch_rejection_reason(js, response));
    ant_http_request_cancel(http_req);
    return;
  }

  fetch_resolve(req, response);
}

static void fetch_http_on_body(ant_http_request_t *http_req, const uint8_t *chunk, size_t len, void *user_data) {
  fetch_request_t *req = (fetch_request_t *)user_data;
  ant_t *js = req->js;
  ant_value_t stream = 0;
  ant_value_t controller = 0;
  ant_value_t value = 0;
  ant_value_t step = 0;

  (void)http_req;
  if (req->aborted || !is_object_type(req->response_obj)) return;

  stream = js_get_slot(req->response_obj, SLOT_RESPONSE_BODY_STREAM);
  if (!rs_is_stream(stream)) return;

  controller = rs_stream_controller(js, stream);
  value = fetch_create_chunk(js, chunk, len);
  if (is_err(value)) {
    fetch_error_response_body(req, fetch_rejection_reason(js, value));
    ant_http_request_cancel(http_req);
    return;
  }

  step = rs_controller_enqueue(js, controller, value);
  if (is_err(step)) {
    fetch_error_response_body(req, fetch_rejection_reason(js, step));
    ant_http_request_cancel(http_req);
  }
}

static ant_value_t fetch_transport_reason(fetch_request_t *req, ant_http_result_t result, const char *error_message) {
  if (result == ANT_HTTP_RESULT_ABORTED && req->aborted) {
    ant_value_t signal = request_get_signal(req->js, req->request_obj);
    return abort_signal_get_reason(signal);
  }

  return fetch_type_error(req->js, error_message ? error_message : "fetch failed");
}

static void fetch_http_on_complete(
  ant_http_request_t *http_req,
  ant_http_result_t result,
  int error_code, const char *error_message, void *user_data
) {
  fetch_request_t *req = (fetch_request_t *)user_data;
  
  ant_t *js = req->js;
  ant_value_t stream = 0;
  ant_value_t controller = 0;
  ant_value_t reason = 0;
  req->http_req = NULL;

  if (req->restart_pending) {
    req->restart_pending = false;
    fetch_start_http(req);
    return;
  }

  if (result != ANT_HTTP_RESULT_OK || error_code != 0) {
    reason = fetch_transport_reason(req, result, error_message);
    if (is_object_type(req->response_obj)) fetch_error_response_body(req, reason);
    else fetch_reject(req, reason);
    fetch_request_release(req);
    return;
  }

  if (is_object_type(req->response_obj)) {
    stream = js_get_slot(req->response_obj, SLOT_RESPONSE_BODY_STREAM);
    if (rs_is_stream(stream)) {
      controller = rs_stream_controller(js, stream);
      rs_controller_close(js, controller);
    }
  } else fetch_reject(req, fetch_type_error(js, "fetch completed without a response"));

  fetch_request_release(req);
}

static char *fetch_data_url_content_type(const char *url) {
  const char *header = url + 5;
  const char *comma = strchr(header, ',');
  const char *base64 = NULL;
  size_t len = 0;

  if (!comma) return strdup("text/plain;charset=US-ASCII");
  base64 = strstr(header, ";base64");
  len = base64 && base64 < comma ? (size_t)(base64 - header) : (size_t)(comma - header);
  if (len == 0) return strdup("text/plain;charset=US-ASCII");
  
  return strndup(header, len);
}

static bool fetch_handle_data_url(fetch_request_t *req) {
  ant_t *js = req->js;
  request_data_t *request = request_get_data(req->request_obj);
  
  char *url = fetch_build_request_url(request);
  size_t len = 0;
  char *body = NULL;
  char *content_type = NULL;
  
  ant_value_t headers = 0;
  ant_value_t response = 0;

  if (!url || !esm_is_data_url(url)) {
    free(url);
    return false;
  }

  body = esm_parse_data_url(url, &len);
  content_type = fetch_data_url_content_type(url);
  headers = headers_create_empty(js);

  if (!body || !content_type || is_err(headers)) {
    free(url);
    free(body);
    free(content_type);
    fetch_reject(req, fetch_type_error(js, "Failed to decode data URL"));
    fetch_request_release(req);
    return true;
  }

  headers_set_literal(js, headers, "content-type", content_type);
  response = response_create_fetched(
    js, 200, "OK", url, 1, headers, 
    (const uint8_t *)body, len, js_mkundef(), content_type
  );

  free(url);
  free(body);
  free(content_type);

  if (is_err(response)) {
    fetch_reject(req, fetch_rejection_reason(js, response));
  } else fetch_resolve(req, response);

  fetch_request_release(req);
  return true;
}

static ant_value_t fetch_upload_on_reject(ant_t *js, ant_value_t *args, int nargs) {
  fetch_request_t *req = (fetch_request_t *)js_get_native(js->current_func, FETCH_REQUEST_NATIVE_TAG);
  ant_value_t reason = (nargs > 0) ? args[0] : js_mkundef();

  if (!req) return js_mkundef();
  req->upload_read_promise = js_mkundef();

  if (!req->aborted) {
    if (req->http_req) ant_http_request_cancel(req->http_req);
    fetch_reject(req, reason);
    if (!req->http_req) fetch_request_release(req);
  }

  fetch_request_release(req);
  return js_mkundef();
}

static void fetch_upload_schedule_next_read(fetch_request_t *req);
static ant_value_t fetch_upload_on_read(ant_t *js, ant_value_t *args, int nargs) {
  fetch_request_t *req = (fetch_request_t *)js_get_native(js->current_func, FETCH_REQUEST_NATIVE_TAG);
  
  ant_value_t result = (nargs > 0) ? args[0] : js_mkundef();
  ant_value_t done = 0;
  ant_value_t value = 0;
  
  const uint8_t *chunk = NULL;
  size_t chunk_len = 0;
  int rc = 0;

  if (!req) return js_mkundef();
  req->upload_read_promise = js_mkundef();

  if (req->aborted || !req->http_req) {
    fetch_request_release(req);
    return js_mkundef();
  }

  done = js_get(js, result, "done");
  value = js_get(js, result, "value");
  if (done == js_true) {
    ant_http_request_end(req->http_req);
    fetch_request_release(req);
    return js_mkundef();
  }

  if (!fetch_get_upload_chunk(value, &chunk, &chunk_len)) {
    ant_value_t reason = js_mkerr_typed(js, JS_ERR_TYPE, "fetch request body stream chunk must be a Uint8Array");
    ant_http_request_cancel(req->http_req);
    fetch_reject(req, fetch_rejection_reason(js, reason));
    fetch_request_release(req);
    return js_mkundef();
  }

  rc = ant_http_request_write(req->http_req, chunk, chunk_len);
  if (rc != 0) {
    ant_value_t reason = fetch_type_error(js, uv_strerror(rc));
    ant_http_request_cancel(req->http_req);
    fetch_reject(req, reason);
    fetch_request_release(req);
    return js_mkundef();
  }

  fetch_upload_schedule_next_read(req);
  fetch_request_release(req);
  return js_mkundef();
}

static void fetch_upload_schedule_next_read(fetch_request_t *req) {
  ant_t *js = req->js;
  
  ant_value_t next_p = 0;
  ant_value_t fulfill = 0;
  ant_value_t reject = 0;
  ant_value_t then_result = 0;

  if (!req || !is_object_type(req->upload_reader)) return;
  next_p = rs_default_reader_read(js, req->upload_reader);
  req->upload_read_promise = next_p;
  
  fulfill = js_heavy_mkfun_native(js, fetch_upload_on_read, req, FETCH_REQUEST_NATIVE_TAG);
  reject = js_heavy_mkfun_native(js, fetch_upload_on_reject, req, FETCH_REQUEST_NATIVE_TAG);
  
  fetch_request_retain(req);
  then_result = js_promise_then(js, next_p, fulfill, reject);
  promise_mark_handled(then_result);
}

static void fetch_start_upload(fetch_request_t *req) {
  ant_t *js = req->js;
  
  ant_value_t stream = js_get_slot(req->request_obj, SLOT_REQUEST_BODY_STREAM);
  ant_value_t reader_args[1] = { stream };
  ant_value_t saved = js->new_target;
  ant_value_t reader = 0;

  if (!rs_is_stream(stream)) return;

  js->new_target = g_reader_proto;
  reader = js_rs_reader_ctor(js, reader_args, 1);
  js->new_target = saved;

  if (is_err(reader)) {
    if (req->http_req) ant_http_request_cancel(req->http_req);
    fetch_reject(req, fetch_rejection_reason(js, reader));
    if (!req->http_req) fetch_request_release(req);
    return;
  }

  req->upload_reader = reader;
  fetch_upload_schedule_next_read(req);
}

static void fetch_start_http(fetch_request_t *req) {
  request_data_t *request = request_get_data(req->request_obj);
  ant_http_request_options_t options = {0};
  ant_http_header_t *headers = NULL;
  char *url = NULL;
  int rc = 0;

  if (!request) {
    fetch_reject(req, fetch_type_error(req->js, "Invalid Request object"));
    fetch_request_release(req);
    return;
  }

  url = fetch_build_request_url(request);
  if (!url) {
    fetch_reject(req, fetch_type_error(req->js, "Invalid request URL"));
    fetch_request_release(req);
    return;
  }

  if (esm_is_data_url(url)) {
    free(url);
    fetch_handle_data_url(req);
    return;
  }
  if (!fetch_is_http_url(url)) {
    free(url);
    fetch_reject(req, fetch_type_error(req->js, "fetch only supports http:, https:, and data: URLs"));
    fetch_request_release(req);
    return;
  }

  headers = fetch_build_http_headers(req->request_obj);
  if (!headers) {
    free(url);
    fetch_reject(req, fetch_type_error(req->js, "out of memory"));
    fetch_request_release(req);
    return;
  }

  options.method = request->method;
  options.url = url;
  options.headers = headers;
  options.body = request->body_data;
  options.body_len = request->body_size;
  options.chunked_body = request->body_is_stream;

  rc = ant_http_request_start(
    uv_default_loop(), &options,
    fetch_http_on_response, fetch_http_on_body, fetch_http_on_complete,
    req, &req->http_req
  );

  ant_http_headers_free(headers);
  free(url);

  if (rc != 0) {
    fetch_reject(req, fetch_type_error(req->js, uv_strerror(rc)));
    fetch_request_release(req);
    return;
  }

  if (request->body_is_stream) fetch_start_upload(req);
}

static ant_value_t fetch_abort_listener(ant_t *js, ant_value_t *args, int nargs) {
  fetch_request_t *req = (fetch_request_t *)js_get_native(js->current_func, FETCH_REQUEST_NATIVE_TAG);
  ant_value_t signal = 0;
  ant_value_t reason = 0;

  if (!req || req->aborted) return js_mkundef();
  req->aborted = true;
  signal = request_get_signal(js, req->request_obj);
  reason = abort_signal_get_reason(signal);

  if (req->http_req) ant_http_request_cancel(req->http_req);
  fetch_reject(req, reason);
  if (!req->http_req) fetch_request_release(req);

  return js_mkundef();
}

ant_value_t ant_fetch(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t input = (nargs >= 1) ? args[0] : js_mkundef();
  ant_value_t init = (nargs >= 2) ? args[1] : js_mkundef();
  
  ant_value_t promise = js_mkpromise(js);
  ant_value_t request_obj = 0;
  
  request_data_t *request = NULL;
  fetch_request_t *req = NULL;
  ant_value_t signal = 0;

  request_obj = request_create_from_input_init(js, input, init);
  if (is_err(request_obj)) {
    js_reject_promise(js, promise, fetch_rejection_reason(js, request_obj));
    return promise;
  }

  request = request_get_data(request_obj);
  if (!request) {
    js_reject_promise(js, promise, fetch_type_error(js, "Invalid Request object"));
    return promise;
  }

  req = calloc(1, sizeof(fetch_request_t));
  if (!req) {
    js_reject_promise(js, promise, fetch_type_error(js, "out of memory"));
    return promise;
  }

  req->js = js;
  req->promise = promise;
  req->request_obj = request_obj;
  req->response_obj = js_mkundef();
  req->abort_listener = js_mkundef();
  req->upload_reader = js_mkundef();
  req->upload_read_promise = js_mkundef();
  req->refs = 1;
  utarray_push_back(pending_requests, &req);

  signal = request_get_signal(js, request_obj);
  if (abort_signal_is_signal(signal)) {
    if (abort_signal_is_aborted(signal)) {
      fetch_reject(req, abort_signal_get_reason(signal));
      fetch_request_release(req);
      return promise;
    }
    
    req->abort_listener = js_heavy_mkfun_native(js, fetch_abort_listener, req, FETCH_REQUEST_NATIVE_TAG);
    abort_signal_add_listener(js, signal, req->abort_listener);
  }

  if (request->has_body) request->body_used = true;
  fetch_start_http(req);
  return promise;
}

void init_fetch_module() {
  utarray_new(pending_requests, &ut_ptr_icd);
  js_set(rt->js, rt->js->global, "fetch", js_mkfun_flags(ant_fetch, CFUNC_HAS_PROTOTYPE));
}

int has_pending_fetches(void) {
  return pending_requests && utarray_len(pending_requests) > 0;
}

void gc_mark_fetch(ant_t *js, gc_mark_fn mark) {
  if (!pending_requests) return;
  unsigned int len = utarray_len(pending_requests);
  
  for (unsigned int i = 0; i < len; i++) {
    fetch_request_t **reqp = (fetch_request_t **)utarray_eltptr(pending_requests, i);
    if (!reqp || !*reqp) continue;
    mark(js, (*reqp)->promise);
    mark(js, (*reqp)->request_obj);
    mark(js, (*reqp)->response_obj);
    mark(js, (*reqp)->abort_listener);
    mark(js, (*reqp)->upload_reader);
    mark(js, (*reqp)->upload_read_promise);
  }
}
