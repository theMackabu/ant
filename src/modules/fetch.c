#include <compat.h> // IWYU pragma: keep

#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlsuv/tlsuv.h>
#include <tlsuv/http.h>
#include <utarray.h>

#include "ant.h"
#include "errors.h"
#include "config.h"
#include "common.h"
#include "internal.h"
#include "runtime.h"
#include "modules/fetch.h"
#include "modules/json.h"
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
  char *method;
  char *body;
  size_t body_len;
} fetch_request_t;

static uv_loop_t *fetch_loop = NULL;
static UT_array *pending_requests = NULL;

static void free_fetch_request(fetch_request_t *req) {
  if (!req) return;
  
  if (req->response_buffer.data) free(req->response_buffer.data);
  if (req->error_msg) free(req->error_msg);
  if (req->method) free(req->method);
  if (req->body) free(req->body);
  
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

static jsval_t fetch_fail_oom(struct js *js, jsval_t promise, fetch_request_t *req, bool close_http, const char *msg, size_t len) {
  jsval_t err = js_mkstr(js, msg, len);
  js_reject_promise(js, promise, err);
  if (close_http) tlsuv_http_close(&req->http_client, NULL);
  remove_pending_request(req); free_fetch_request(req);
  return js_mkundef();
}

static jsval_t response_text(struct js *js, jsval_t *args, int nargs) {  
  jsval_t this = js_getthis(js);
  jsval_t body = js_get_slot(js, this, SLOT_DATA);
  
  jsval_t promise = js_mkpromise(js);
  js_resolve_promise(js, promise, body);
  
  return promise;
}

static jsval_t response_json(struct js *js, jsval_t *args, int nargs) {  
  jsval_t this = js_getthis(js);
  jsval_t body = js_get_slot(js, this, SLOT_DATA);
  jsval_t parsed = js_json_parse(js, &body, 1);
  jsval_t promise = js_mkpromise(js);
  
  if (vtype(parsed) == T_ERR) {
    js_reject_promise(js, promise, parsed);
  } else js_resolve_promise(js, promise, parsed);
  
  return promise;
}

static jsval_t create_response(struct js *js, int status, const char *body, size_t body_len) {
  jsval_t response_obj = js_mkobj(js);
  jsval_t body_str = js_mkstr(js, body, body_len);

  js_set(js, response_obj, "ok", status >= 200 && status < 300 ? js_mktrue() : js_mkfalse());
  js_set(js, response_obj, "status", js_mknum(status));
  
  js_set_slot(js, response_obj, SLOT_DATA, body_str);
  
  js_set(js, response_obj, "text", js_mkfun(response_text));
  js_set(js, response_obj, "json", js_mkfun(response_json));
  
  return response_obj;
}

static void complete_request(fetch_request_t *req) {
  if (req->failed) {
    jsval_t err = js_mkstr(req->js, req->error_msg ? req->error_msg : "Unknown error", req->error_msg ? strlen(req->error_msg) : 13);
    js_reject_promise(req->js, req->promise, err);
  } else {
    const char *body = req->response_buffer.data ? req->response_buffer.data : "";
    size_t body_len = req->response_buffer.data ? req->response_buffer.size : 0;
    jsval_t response = create_response(req->js, req->status_code, body, body_len);
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
    req->error_msg = strdup(uv_strerror((int)len));
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
  jsval_t promise = js_get_slot(js, current_func, SLOT_DATA);
  
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
  
  const char *scheme_end = strstr(url_str, "://");
  if (!scheme_end) return fetch_fail_oom(js, promise, req, false, "Invalid URL: no scheme", 22);
  
  const char *host_start = scheme_end + 3;
  const char *path_start = strchr(host_start, '/');
  const char *at_in_host = NULL;
  
  for (const char *p = host_start; p < (path_start ? path_start : host_start + strlen(host_start)); p++) {
    if (*p == '@') at_in_host = p;
  } if (at_in_host) host_start = at_in_host + 1;
  
  size_t scheme_len = scheme_end - url_str;
  size_t host_len = path_start ? (size_t)(path_start - host_start) : strlen(host_start);
  
  const char *path = path_start ? path_start : "/";
  if (host_len == 0) return fetch_fail_oom(js, promise, req, false, "Invalid URL: no host", 20);
  
  char *host_url = calloc(1, scheme_len + 3 + host_len + 1);
  if (!host_url) return fetch_fail_oom(js, promise, req, false, "Out of memory", 13);
  snprintf(host_url, scheme_len + 3 + host_len + 1, "%.*s://%.*s", (int)scheme_len, url_str, (int)host_len, host_start);
  
  int rc = tlsuv_http_init(fetch_loop, &req->http_client, host_url); free(host_url);
  if (rc != 0) return fetch_fail_oom(js, promise, req, false, "Failed to initialize HTTP client", 33);
  
  req->http_client.data = req;
  req->method = NULL;
  req->body = NULL;
  req->body_len = 0;
  
  if (is_special_object(options_val)) {
    jsval_t method_val = js_get(js, options_val, "method");
    if (vtype(method_val) == T_STR) {
      char *str = js_getstr(js, method_val, NULL);
      if (str) {
        req->method = strdup(str);
        if (!req->method) return fetch_fail_oom(js, promise, req, true, "Out of memory", 13);
      }
    }
    
    jsval_t body_val = js_get(js, options_val, "body");
    if (vtype(body_val) == T_STR) {
      size_t len;
      char *str = js_getstr(js, body_val, &len);
      if (str && len > 0) {
        req->body = malloc(len);
        if (!req->body) return fetch_fail_oom(js, promise, req, true, "Out of memory", 13);
        memcpy(req->body, str, len);
        req->body_len = len;
      }
    }
  }
  
  if (!req->method) {
    req->method = strdup("GET");
    if (!req->method) return fetch_fail_oom(js, promise, req, true, "Out of memory", 13);
  }
  
  req->http_req = tlsuv_http_req(&req->http_client, req->method, path, resp_cb, req);
  if (!req->http_req) return fetch_fail_oom(js, promise, req, true, "Failed to create HTTP request", 30);
  req->http_req->data = req;
  
  char user_agent[256];
  snprintf(user_agent, sizeof(user_agent), "ant/%s", ANT_VERSION);
  tlsuv_http_req_header(req->http_req, "User-Agent", user_agent);
  
  if (is_special_object(options_val)) {
    jsval_t headers_val = js_get(js, options_val, "headers");
    if (is_special_object(headers_val)) {
      ant_iter_t iter = js_prop_iter_begin(js, headers_val);
      const char *key;
      size_t key_len;
      jsval_t value;
      
      while (js_prop_iter_next(&iter, &key, &key_len, &value)) {
        char *value_str = js_getstr(js, value, NULL);
        if (value_str) {
          char *key_str = strndup(key, key_len);
          if (key_str) { tlsuv_http_req_header(req->http_req, key_str, value_str); free(key_str); }
        }
      }
      js_prop_iter_end(&iter);
    }
  }
  
  if (req->body && req->body_len > 0) {
    tlsuv_http_req_data(req->http_req, req->body, req->body_len, body_cb);
  }
  
  return js_mkundef();
}

static jsval_t js_fetch(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "fetch requires at least 1 argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "fetch URL must be a string");
  
  jsval_t url_val = args[0];
  jsval_t options_val = nargs > 1 ? args[1] : js_mkundef();
  
  jsval_t promise = js_mkpromise(js);
  jsval_t wrapper_obj = js_mkobj(js);
  
  js_set_slot(js, wrapper_obj, SLOT_CFUNC, js_mkfun(do_fetch_microtask));
  js_set_slot(js, wrapper_obj, SLOT_DATA, promise);
  
  js_set(js, wrapper_obj, "url", url_val);
  js_set(js, wrapper_obj, "options", options_val);
  
  queue_microtask(js, js_obj_to_func(wrapper_obj));
  
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
  if (fetch_loop && fetch_loop == uv_default_loop() && (rt->flags & ANT_RUNTIME_EXT_EVENT_LOOP)) return;
  if (fetch_loop && uv_loop_alive(fetch_loop)) {
    uv_run(fetch_loop, fetch_loop == uv_default_loop() ? UV_RUN_NOWAIT : UV_RUN_ONCE);
    if (pending_requests && utarray_len(pending_requests) > 0) usleep(1000);
  }
}

void fetch_gc_update_roots(GC_OP_VAL_ARGS) {
  if (!pending_requests) return;
  unsigned int len = utarray_len(pending_requests);
  for (unsigned int i = 0; i < len; i++) {
    fetch_request_t **reqp = (fetch_request_t **)utarray_eltptr(pending_requests, i);
    if (reqp && *reqp) {
      op_val(ctx, &(*reqp)->promise);
      op_val(ctx, &(*reqp)->headers_obj);
    }
  }
}
