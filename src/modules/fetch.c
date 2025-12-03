#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlsuv/tlsuv.h>
#include <tlsuv/http.h>
#include <utarray.h>
#include <unistd.h>

#include "runtime.h"
#include "modules/fetch.h"
#include "modules/timer.h"

typedef struct {
  char *data;
  size_t size;
  size_t capacity;
} fetch_buffer_t;

typedef struct fetch_request_s {
  struct js *js;
  jsval_t promise;
  tlsuv_http_t http_client;
  tlsuv_http_req_t *http_req;
  fetch_buffer_t response_buffer;
  int status_code;
  int completed;
  int failed;
  char *error_msg;
  jsval_t headers_obj;
} fetch_request_t;

static uv_loop_t *fetch_loop = NULL;
static UT_array *pending_requests = NULL;

static void free_fetch_request(fetch_request_t *req) {
  if (!req) return;
  
  if (req->response_buffer.data) free(req->response_buffer.data);
  if (req->error_msg) free(req->error_msg);
  
  free(req);
}

static void remove_pending_request(fetch_request_t *req) {
  if (!req || !pending_requests) return;
  
  fetch_request_t **p = NULL;
  unsigned int i = 0;
  
  while ((p = (fetch_request_t**)utarray_next(pending_requests, p))) {
    if (*p == req) { utarray_erase(pending_requests, i, 1); break; }
    i++;
  }
}

static jsval_t create_response(struct js *js, int status, const char *body, size_t body_len) {
  jsval_t response_obj = js_mkobj(js);
  
  js_set(js, response_obj, "status", js_mknum(status));
  js_set(js, response_obj, "ok", status >= 200 && status < 300 ? js_mktrue() : js_mkfalse());
  js_set(js, response_obj, "__body", js_mkstr(js, body, body_len));
  
  const char *json_code = "(){return JSON.parse(this.__body)}";
  jsval_t json_str = js_mkstr(js, json_code, strlen(json_code));
  jsval_t json_obj = js_mkobj(js);
  js_set(js, json_obj, "__code", json_str);
  jsval_t json_func = js_mknum(0);
  memcpy(&json_func, &json_obj, sizeof(jsval_t));
  json_func = (json_func & 0xFFFFFFFFFFFFULL) | (0x7FF0000000000000ULL | ((uint64_t)7 << 48));
  js_set(js, response_obj, "json", json_func);
  
  const char *text_code = "(){return this.__body}";
  jsval_t text_str = js_mkstr(js, text_code, strlen(text_code));
  jsval_t text_obj = js_mkobj(js);
  js_set(js, text_obj, "__code", text_str);
  jsval_t text_func = js_mknum(0);
  memcpy(&text_func, &text_obj, sizeof(jsval_t));
  text_func = (text_func & 0xFFFFFFFFFFFFULL) | (0x7FF0000000000000ULL | ((uint64_t)7 << 48));
  js_set(js, response_obj, "text", text_func);
  
  return response_obj;
}

static void complete_request(fetch_request_t *req) {
  if (req->failed) {
    jsval_t err = js_mkstr(req->js, req->error_msg ? req->error_msg : "Unknown error", req->error_msg ? strlen(req->error_msg) : 13);
    js_reject_promise(req->js, req->promise, err);
  } else {
    jsval_t response = create_response(req->js, req->status_code, req->response_buffer.data, req->response_buffer.size);
    js_resolve_promise(req->js, req->promise, response);
  }
  
  remove_pending_request(req);
  free_fetch_request(req);
}

static void on_http_close(tlsuv_http_t *client) {
  fetch_request_t *req = (fetch_request_t *)client->data;
  if (req && req->completed) complete_request(req);
}

static void body_cb(tlsuv_http_req_t *http_req, char *body, ssize_t len) {
  fetch_request_t *req = (fetch_request_t *)http_req->data;
  
  if (len == UV_EOF) {
    req->completed = 1;
    tlsuv_http_close(&req->http_client, on_http_close);
    return;
  }
  
  if (len < 0) {
    req->failed = 1;
    req->error_msg = strdup(uv_strerror(len));
    req->completed = 1;
    tlsuv_http_close(&req->http_client, on_http_close);
    return;
  }
  
  if (req->response_buffer.size + len > req->response_buffer.capacity) {
    size_t new_capacity = req->response_buffer.capacity * 2;
    while (new_capacity < req->response_buffer.size + len) new_capacity *= 2;
    char *new_data = realloc(req->response_buffer.data, new_capacity);
    
    if (!new_data) {
      req->failed = 1;
      req->error_msg = strdup("Out of memory");
      req->completed = 1;
      tlsuv_http_close(&req->http_client, on_http_close);
      return;
    }
    
    req->response_buffer.data = new_data;
    req->response_buffer.capacity = new_capacity;
  }
  
  memcpy(req->response_buffer.data + req->response_buffer.size, body, len);
  req->response_buffer.size += len;
}

static void resp_cb(tlsuv_http_resp_t *resp, void *data) {
  (void)data;
  fetch_request_t *req = (fetch_request_t *)resp->req->data;
  
  if (resp->code < 0) {
    req->failed = 1;
    req->error_msg = strdup(uv_strerror(resp->code));
    req->completed = 1;
    tlsuv_http_close(&req->http_client, on_http_close);
    return;
  }
  
  req->status_code = resp->code;
  resp->body_cb = body_cb;
}

static jsval_t do_fetch_microtask(struct js *js, jsval_t *args, int nargs) {
  (void)args;
  (void)nargs;
  
  jsval_t current_func = js_getcurrentfunc(js);
  jsval_t url_val = js_get(js, current_func, "url");
  jsval_t options_val = js_get(js, current_func, "options");
  jsval_t promise = js_get(js, current_func, "promise");
  
  char *url_str = js_getstr(js, url_val, NULL);
  if (!url_str) {
    jsval_t err = js_mkstr(js, "Invalid URL", 11);
    js_reject_promise(js, promise, err);
    return js_mkundef();
  }
  
  fetch_request_t *req = calloc(1, sizeof(fetch_request_t));
  if (!req) {
    jsval_t err = js_mkstr(js, "Out of memory", 13);
    js_reject_promise(js, promise, err);
    return js_mkundef();
  }
  
  req->js = js;
  req->promise = promise;
  req->headers_obj = options_val;
  
  req->response_buffer.capacity = 16384;
  req->response_buffer.data = malloc(req->response_buffer.capacity);
  req->response_buffer.size = 0;
  
  if (!req->response_buffer.data) {
    jsval_t err = js_mkstr(js, "Out of memory", 13);
    js_reject_promise(js, promise, err);
    free(req);
    return js_mkundef();
  }
  
  if (!pending_requests) utarray_new(pending_requests, &ut_ptr_icd);
  utarray_push_back(pending_requests, &req);
  
  struct tlsuv_url_s parsed_url;
  if (tlsuv_parse_url(&parsed_url, url_str) != 0) {
    jsval_t err = js_mkstr(js, "Failed to parse URL", 19);
    js_reject_promise(js, promise, err);
    remove_pending_request(req);
    free_fetch_request(req);
    return js_mkundef();
  }
  
  size_t host_len = (parsed_url.path - parsed_url.hostname) + parsed_url.hostname_len;
  char *host_url = calloc(1, host_len + parsed_url.scheme_len + 4);
  
  snprintf(
    host_url, host_len + parsed_url.scheme_len + 4, "%.*s://%.*s", 
    (int)parsed_url.scheme_len, parsed_url.scheme, (int)parsed_url.hostname_len, parsed_url.hostname)
  ;
  
  if (parsed_url.port > 0 && !((parsed_url.port == 80 && strncmp(parsed_url.scheme, "http", 4) == 0) || (parsed_url.port == 443 && strncmp(parsed_url.scheme, "https", 5) == 0))) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), ":%d", parsed_url.port);
    strcat(host_url, port_str);
  }
  
  int rc = tlsuv_http_init(fetch_loop, &req->http_client, host_url);
  free(host_url);
  
  if (rc != 0) {
    jsval_t err = js_mkstr(js, "Failed to initialize HTTP client", 33);
    js_reject_promise(js, promise, err);
    remove_pending_request(req);
    free_fetch_request(req);
    return js_mkundef();
  }
  
  req->http_client.data = req;
  
  char *method = "GET";
  char *body = NULL;
  size_t body_len = 0;
  
  if (js_type(options_val) != JS_UNDEF && js_type(options_val) != JS_NULL) {
    jsval_t method_val = js_get(js, options_val, "method");
    if (js_type(method_val) == JS_STR) {
      char *method_str = js_getstr(js, method_val, NULL);
      if (method_str) method = method_str;
    }
    
    jsval_t body_val = js_get(js, options_val, "body");
    if (js_type(body_val) == JS_STR) {
      body = js_getstr(js, body_val, NULL);
      if (body) body_len = strlen(body);
    }
  }
  
  char *path = parsed_url.path_len > 0 ? strndup(parsed_url.path, parsed_url.path_len) : strdup("/");
  req->http_req = tlsuv_http_req(&req->http_client, method, path, resp_cb, req);
  free(path);
  
  if (!req->http_req) {
    jsval_t err = js_mkstr(js, "Failed to create HTTP request", 30);
    js_reject_promise(js, promise, err);
    tlsuv_http_close(&req->http_client, NULL);
    remove_pending_request(req);
    free_fetch_request(req);
    return js_mkundef();
  }
  
  req->http_req->data = req;
  
  if (js_type(options_val) != JS_UNDEF && js_type(options_val) != JS_NULL) {
    jsval_t headers_val = js_get(js, options_val, "headers");
    if (js_type(headers_val) != JS_UNDEF && js_type(headers_val) != JS_NULL) {
      js_prop_iter_t iter = js_prop_iter_begin(js, headers_val);
      const char *key;
      size_t key_len;
      jsval_t value;
      
      while (js_prop_iter_next(&iter, &key, &key_len, &value)) {
        char *value_str = js_getstr(js, value, NULL);
        if (value_str) {
          char *key_str = strndup(key, key_len);
          tlsuv_http_req_header(req->http_req, key_str, value_str);
          free(key_str);
        }
      }
      js_prop_iter_end(&iter);
    }
  }
  
  if (body && body_len > 0) {
    tlsuv_http_req_data(req->http_req, body, body_len, body_cb);
  }
  
  return js_mkundef();
}

static jsval_t js_fetch(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "fetch requires at least 1 argument");
  if (js_type(args[0]) != JS_STR) return js_mkerr(js, "fetch URL must be a string");
  
  jsval_t url_val = args[0];
  jsval_t options_val = nargs > 1 ? args[1] : js_mkundef();
  
  jsval_t promise = js_mkpromise(js);
  jsval_t wrapper_obj = js_mkobj(js);
  
  js_set(js, wrapper_obj, "__native_func", js_mkfun(do_fetch_microtask));
  js_set(js, wrapper_obj, "url", url_val);
  js_set(js, wrapper_obj, "options", options_val);
  js_set(js, wrapper_obj, "promise", promise);
  
  jsval_t wrapper_func = js_mknum(0);
  memcpy(&wrapper_func, &wrapper_obj, sizeof(jsval_t));
  wrapper_func = (wrapper_func & 0xFFFFFFFFFFFFULL) | (0x7FF0000000000000ULL | ((uint64_t)7 << 48));
  queue_microtask(js, wrapper_func);
  
  return promise;
}

void init_fetch_module() {
  if (!fetch_loop) fetch_loop = uv_default_loop();
  struct js *js = rt->js;
  js_set(js, js_glob(js), "fetch", js_mkfun(js_fetch));
}

int has_pending_fetches(void) {
  return (pending_requests && utarray_len(pending_requests) > 0) || (fetch_loop && uv_loop_alive(fetch_loop));
}

void fetch_poll_events(void) {
  if (fetch_loop && fetch_loop == uv_default_loop() && rt->external_event_loop_active) return;
  if (fetch_loop && uv_loop_alive(fetch_loop)) {
    uv_run(fetch_loop, UV_RUN_ONCE);
    if (pending_requests && utarray_len(pending_requests) > 0) usleep(1000);
  }
}
