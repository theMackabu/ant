#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "runtime.h"
#include "modules/fetch.h"
#include "modules/timer.h"

typedef struct {
  char *data;
  size_t size;
} fetch_buffer_t;

static CURL *global_curl = NULL;
static void cleanup_fetch_module(void);

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
  size_t realsize = size * nmemb;
  fetch_buffer_t *buf = (fetch_buffer_t *)userp;
  
  char *ptr = realloc(buf->data, buf->size + realsize + 1);
  if (!ptr) return 0;
  
  buf->data = ptr;
  memcpy(&(buf->data[buf->size]), contents, realsize);
  buf->size += realsize;
  buf->data[buf->size] = 0;
  
  return realsize;
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
  
  const char *method = "GET";
  char *body = NULL;
  
  if (js_type(options_val) != JS_UNDEF && js_type(options_val) != JS_NULL) {
    jsval_t method_val = js_get(js, options_val, "method");
    if (js_type(method_val) == JS_STR) {
      method = js_getstr(js, method_val, NULL);
    }
    
    jsval_t body_val = js_get(js, options_val, "body");
    if (js_type(body_val) == JS_STR) {
      body = js_getstr(js, body_val, NULL);
    }
  }
  
  CURL *curl = global_curl;
  if (!curl) {
    jsval_t err = js_mkstr(js, "CURL not initialized", 20);
    js_reject_promise(js, promise, err);
    return js_mkundef();
  }
  
  fetch_buffer_t response_buf = { .data = malloc(1), .size = 0 };
  if (!response_buf.data) {
    jsval_t err = js_mkstr(js, "Memory allocation failed", 24);
    js_reject_promise(js, promise, err);
    return js_mkundef();
  }
  
  curl_easy_reset(curl);
  
  curl_easy_setopt(curl, CURLOPT_URL, url_str);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  
  if (strcmp(method, "GET") != 0) {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
  }
  
  struct curl_slist *headers = NULL;
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
          char header_line[1024];
          snprintf(header_line, sizeof(header_line), "%.*s: %s", (int)key_len, key, value_str);
          headers = curl_slist_append(headers, header_line);
        }
      }
      js_prop_iter_end(&iter);
      
      if (headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      }
    }
  }
  
  if (body) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
  }
  
  CURLcode res = curl_easy_perform(curl);
  if (headers) curl_slist_free_all(headers);
  
  if (res != CURLE_OK) {
    free(response_buf.data);
    const char *err_msg = curl_easy_strerror(res);
    jsval_t err = js_mkstr(js, err_msg, strlen(err_msg));
    js_reject_promise(js, promise, err);
    return js_mkundef();
  }
  
  long status_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
  
  jsval_t response = create_response(js, (int)status_code, response_buf.data, response_buf.size);
  
  free(response_buf.data);
  js_resolve_promise(js, promise, response);
  
  return js_mkundef();
}

static jsval_t js_fetch(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "fetch requires at least 1 argument");
  }
  
  if (js_type(args[0]) != JS_STR) {
    return js_mkerr(js, "fetch URL must be a string");
  }
  
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
  static int curl_initialized = 0;
  if (!curl_initialized) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl_initialized = 1;
    
    global_curl = curl_easy_init();
    if (!global_curl) {
      fprintf(stderr, "Warning: Failed to initialize CURL handle\n");
    }
    
    atexit(cleanup_fetch_module);
  }
  
  struct js *js = rt->js;
  js_set(js, js_glob(js), "fetch", js_mkfun(js_fetch));
}

static void cleanup_fetch_module() {
  if (global_curl) {
    curl_easy_cleanup(global_curl);
    global_curl = NULL;
  }
  curl_global_cleanup();
}
