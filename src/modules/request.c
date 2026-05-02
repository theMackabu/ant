#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>

#include "ant.h"
#include "ptr.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "common.h"
#include "descriptors.h"

#include "modules/blob.h"
#include "modules/buffer.h"
#include "modules/assert.h"
#include "modules/abort.h"
#include "modules/formdata.h"
#include "modules/headers.h"
#include "modules/multipart.h"
#include "modules/request.h"
#include "modules/symbol.h"
#include "modules/url.h"
#include "modules/json.h"
#include "streams/pipes.h"
#include "streams/readable.h"

ant_value_t g_request_proto = 0;

enum { REQUEST_NATIVE_TAG = 0x52455153u }; // REQS

static request_data_t *get_data(ant_value_t obj) {
  return (request_data_t *)js_get_native(obj, REQUEST_NATIVE_TAG);
}

request_data_t *request_get_data(ant_value_t obj) {
  return get_data(obj);
}

ant_value_t request_get_headers(ant_value_t obj) {
  return js_get_slot(obj, SLOT_REQUEST_HEADERS);
}

ant_value_t request_get_signal(ant_t *js, ant_value_t obj) {
  ant_value_t signal = js_get_slot(obj, SLOT_REQUEST_SIGNAL);

  if (vtype(signal) != T_UNDEF) return signal;
  signal = abort_signal_create_dependent(js, js_mkundef());
  
  if (is_err(signal)) return signal;
  ant_value_t abort_reason = js_get_slot(obj, SLOT_REQUEST_ABORT_REASON);
  
  if (vtype(abort_reason) != T_UNDEF) signal_do_abort(js, signal, abort_reason);
  js_set_slot_wb(js, obj, SLOT_REQUEST_SIGNAL, signal);
  
  return signal;
}

static void data_free(request_data_t *d) {
  if (!d) return;
  free(d->method);
  url_state_clear(&d->url);
  free(d->referrer);
  free(d->referrer_policy);
  free(d->mode);
  free(d->credentials);
  free(d->cache);
  free(d->redirect);
  free(d->integrity);
  free(d->body_data);
  free(d->body_type);
  free(d);
}

static request_data_t *data_new_with(const char *method, const char *mode) {
  request_data_t *d = calloc(1, sizeof(request_data_t));
  if (!d) return NULL;

  d->method = strdup(method ? method : "GET");
  d->referrer = strdup("client");
  d->referrer_policy = strdup("");
  d->mode = strdup(mode);
  d->credentials = strdup("same-origin");
  d->cache = strdup("default");
  d->redirect = strdup("follow");
  d->integrity = strdup("");
  
  if (!d->method
    || !d->referrer
    || !d->referrer_policy
    || !d->mode
    || !d->credentials
    || !d->cache
    || !d->redirect
    || !d->integrity
  ) { data_free(d); return NULL; }

  return d;
}

static request_data_t *data_new(void) { return data_new_with("GET", "cors"); }
static request_data_t *data_new_server(const char *method) { return data_new_with(method, "same-origin"); }

static ant_value_t request_create_object(ant_t *js, request_data_t *req, ant_value_t headers_obj, bool create_signal) {
  ant_value_t obj = js_mkobj(js);
  ant_value_t hdrs = is_object_type(headers_obj)
    ? headers_obj
    : headers_create_empty(js);

  js_set_proto_init(obj, g_request_proto);
  js_set_slot(obj, SLOT_BRAND, js_mknum(BRAND_REQUEST));
  js_set_native(obj, req, REQUEST_NATIVE_TAG);

  headers_set_guard(hdrs,
    strcmp(req->mode, "no-cors") == 0
    ? HEADERS_GUARD_REQUEST_NO_CORS
    : HEADERS_GUARD_REQUEST
  );

  headers_apply_guard(hdrs);
  js_set_slot_wb(js, obj, SLOT_REQUEST_HEADERS, hdrs);
  js_set_slot(obj, SLOT_REQUEST_ABORT_REASON, js_mkundef());
  
  js_set_slot_wb(js, obj, SLOT_REQUEST_SIGNAL, create_signal 
    ? abort_signal_create_dependent(js, js_mkundef()) 
    : js_mkundef()
  );
  
  return obj;
}

static char *request_build_server_base_url(const char *host, const char *server_hostname, int server_port) {
  const char *authority = (host && host[0]) ? host : server_hostname;
  size_t authority_len = authority ? strlen(authority) : 0;
  bool include_port = (!host || !host[0]) && server_port > 0 && server_port != 80;
  size_t cap = sizeof("http://") - 1 + authority_len + 1 + 16 + 1;
  
  char *base = malloc(cap);
  int written = 0;

  if (!base) return NULL;
  if (include_port) written = snprintf(base, cap, "http://%s:%d/", authority ? authority : "", server_port);
  else written = snprintf(base, cap, "http://%s/", authority ? authority : "");
  
  if (written < 0 || (size_t)written >= cap) {
    free(base);
    return NULL;
  }

  return base;
}

static int request_parse_server_url(
  const char *target,
  bool absolute_target,
  const char *host,
  const char *server_hostname,
  int server_port,
  url_state_t *out
) {
  char *base = NULL;
  int rc = 0;

  if (absolute_target)
    return parse_url_to_state(target, NULL, out);

  base = request_build_server_base_url(host, server_hostname, server_port);
  if (!base) return -1;
  
  rc = parse_url_to_state(target, base, out);
  free(base);
  
  return rc;
}

static request_data_t *data_dup(const request_data_t *src) {
  request_data_t *d = calloc(1, sizeof(request_data_t));
  if (!d) return NULL;

#define DUP_STR(f) do { d->f = src->f ? strdup(src->f) : NULL; } while(0)
  DUP_STR(method);
  DUP_STR(referrer);
  DUP_STR(referrer_policy);
  DUP_STR(mode);
  DUP_STR(credentials);
  DUP_STR(cache);
  DUP_STR(redirect);
  DUP_STR(integrity);
  DUP_STR(body_type);
#undef DUP_STR
  url_state_t *su = (url_state_t *)&src->url;
  url_state_t *du = &d->url;
#define DUP_US(f) do { du->f = su->f ? strdup(su->f) : NULL; } while(0)
  DUP_US(protocol); DUP_US(username); DUP_US(password);
  DUP_US(hostname); DUP_US(port);     DUP_US(pathname);
  DUP_US(search);   DUP_US(hash);
#undef DUP_US
  d->keepalive         = src->keepalive;
  d->reload_navigation = src->reload_navigation;
  d->history_navigation = src->history_navigation;
  d->has_body          = src->has_body;
  d->body_is_stream    = src->body_is_stream;
  d->body_used         = src->body_used;
  d->body_size         = src->body_size;

  if (src->body_data && src->body_size > 0) {
    d->body_data = malloc(src->body_size);
    if (!d->body_data) { data_free(d); return NULL; }
    memcpy(d->body_data, src->body_data, src->body_size);
  }

  return d;
}

static bool is_token_char(unsigned char c) {
  if (c == 0 || c > 127) return false;
  static const char *delimiters = "(),/:;<=>?@[\\]{}\"";
  return c > 32 && !strchr(delimiters, (char)c);
}

static bool is_valid_method(const char *m) {
  if (!m || !*m) return false;
  for (const unsigned char *p = (const unsigned char *)m; *p; p++)
    if (!is_token_char(*p)) return false;
  return true;
}

static bool is_forbidden_method(const char *m) {
  return 
    strcasecmp(m, "CONNECT") == 0 ||
    strcasecmp(m, "TRACE")   == 0 ||
    strcasecmp(m, "TRACK")   == 0;
}

static bool is_cors_safelisted_method(const char *m) {
  return 
    strcasecmp(m, "GET")  == 0 ||
    strcasecmp(m, "HEAD") == 0 ||
    strcasecmp(m, "POST") == 0;
}

static void normalize_method(char *m) {
  static const char *norm[] = { 
    "DELETE","GET","HEAD","OPTIONS","POST","PUT" 
  };
  
  for (int i = 0; i < 6; i++) {
  if (strcasecmp(m, norm[i]) == 0) {
    strcpy(m, norm[i]);
    return;
  }}
}

static ant_value_t request_rejection_reason(ant_t *js, ant_value_t value) {
  if (!is_err(value)) return value;
  ant_value_t reason = js->thrown_exists ? js->thrown_value : value;
  js->thrown_exists = false;
  js->thrown_value = js_mkundef();
  js->thrown_stack = js_mkundef();
  return reason;
}

static const char *request_effective_body_type(ant_t *js, ant_value_t req_obj, request_data_t *d) {
  ant_value_t headers = js_get_slot(req_obj, SLOT_REQUEST_HEADERS);
  if (!headers_is_headers(headers)) return d ? d->body_type : NULL;
  ant_value_t ct = headers_get_value(js, headers, "content-type");
  if (vtype(ct) == T_STR) return js_getstr(js, ct, NULL);
  return d ? d->body_type : NULL;
}

static bool copy_body_bytes(
  ant_t *js, const uint8_t *src, size_t src_len,
  uint8_t **out_data, size_t *out_size, ant_value_t *err_out
) {
  uint8_t *buf = NULL;

  *out_data = NULL;
  *out_size = 0;
  if (src_len == 0) return true;

  buf = malloc(src_len);
  if (!buf) {
    *err_out = js_mkerr(js, "out of memory");
    return false;
  }

  memcpy(buf, src, src_len);
  *out_data = buf;
  *out_size = src_len;
  return true;
}

static bool extract_buffer_source_body(
  ant_t *js, ant_value_t body_val,
  uint8_t **out_data, size_t *out_size, ant_value_t *err_out
) {
  const uint8_t *src = NULL;
  size_t src_len = 0;

  if (!((
    vtype(body_val) == T_TYPEDARRAY || vtype(body_val) == T_OBJ) &&
    buffer_source_get_bytes(js, body_val, &src, &src_len))
  ) return false;

  return copy_body_bytes(js, src, src_len, out_data, out_size, err_out);
}

static bool extract_stream_body(
  ant_t *js, ant_value_t body_val,
  ant_value_t *out_stream, ant_value_t *err_out
) {
  if (!rs_is_stream(body_val)) return false;
  if (rs_stream_unusable(body_val)) {
    *err_out = js_mkerr_typed(js, JS_ERR_TYPE, "body stream is disturbed or locked");
    return false;
  }

  *out_stream = body_val;
  return true;
}

static bool extract_blob_body(
  ant_t *js, ant_value_t body_val,
  uint8_t **out_data, size_t *out_size, char **out_type, ant_value_t *err_out
) {
  blob_data_t *bd = blob_is_blob(js, body_val) ? blob_get_data(body_val) : NULL;
  if (!bd) return false;

  if (!copy_body_bytes(js, bd->data, bd->size, out_data, out_size, err_out)) return false;
  if (bd->type && bd->type[0]) *out_type = strdup(bd->type);
  
  return true;
}

static bool extract_urlsearchparams_body(
  ant_t *js, ant_value_t body_val,
  uint8_t **out_data, size_t *out_size, char **out_type
) {
  char *serialized = NULL;
  if (!usp_is_urlsearchparams(js, body_val)) return false;

  serialized = usp_serialize(js, body_val);
  if (serialized) {
    *out_data = (uint8_t *)serialized;
    *out_size = strlen(serialized);
    *out_type = strdup("application/x-www-form-urlencoded;charset=UTF-8");
  }
  
  return true;
}

static bool extract_formdata_body(
  ant_t *js, ant_value_t body_val,
  uint8_t **out_data, size_t *out_size, char **out_type, ant_value_t *err_out
) {
  char *boundary = NULL;
  char *content_type = NULL;
  size_t mp_size = 0;
  uint8_t *mp = NULL;

  if (!formdata_is_formdata(js, body_val)) return false;
  mp = formdata_serialize_multipart(js, body_val, &mp_size, &boundary);
  
  if (!mp) {
    *err_out = js_mkerr(js, "out of memory");
    return false;
  }

  if (mp_size > 0) *out_data = mp;
  else free(mp);

  *out_size = mp_size;
  if (boundary) {
    size_t ct_len = snprintf(NULL, 0, "multipart/form-data; boundary=%s", boundary);
    content_type = malloc(ct_len + 1);
    if (!content_type) {
      free(boundary);
      if (mp_size > 0) free(mp);
      *out_data = NULL;
      *out_size = 0;
      *err_out = js_mkerr(js, "out of memory");
      return false;
    }
    snprintf(content_type, ct_len + 1, "multipart/form-data; boundary=%s", boundary);
    free(boundary);
    *out_type = content_type;
  }

  return true;
}

static bool extract_string_body(
  ant_t *js, ant_value_t body_val,
  uint8_t **out_data, size_t *out_size, char **out_type, ant_value_t *err_out
) {
  size_t len = 0;
  const char *s = NULL;

  if (vtype(body_val) != T_STR) {
  body_val = js_tostring_val(js, body_val);
  if (is_err(body_val)) {
    *err_out = body_val;
    return false;
  }}

  s = js_getstr(js, body_val, &len);
  if (!copy_body_bytes(js, (const uint8_t *)s, len, out_data, out_size, err_out)) return false;
  *out_type = strdup("text/plain;charset=UTF-8");
  
  return true;
}

static bool extract_body(
  ant_t *js, ant_value_t body_val,
  uint8_t **out_data, size_t *out_size, char **out_type,
  ant_value_t *out_stream, ant_value_t *err_out
) {
  *out_data   = NULL;
  *out_size   = 0;
  *out_type   = NULL;
  *out_stream = js_mkundef();
  *err_out    = js_mkundef();

  if (vtype(body_val) == T_NULL || vtype(body_val) == T_UNDEF) return true;
  if (extract_buffer_source_body(js, body_val, out_data, out_size, err_out)) return true;
  if (vtype(body_val) == T_OBJ && rs_is_stream(body_val)) return extract_stream_body(js, body_val, out_stream, err_out);
  if (vtype(body_val) == T_OBJ && extract_blob_body(js, body_val, out_data, out_size, out_type, err_out)) return true;
  if (vtype(body_val) == T_OBJ && extract_urlsearchparams_body(js, body_val, out_data, out_size, out_type)) return true;
  if (vtype(body_val) == T_OBJ && extract_formdata_body(js, body_val, out_data, out_size, out_type, err_out)) return true;
  
  return extract_string_body(js, body_val, out_data, out_size, out_type, err_out);
}

enum { 
  BODY_TEXT = 0,
  BODY_JSON,
  BODY_ARRAYBUFFER,
  BODY_BLOB,
  BODY_BYTES,
  BODY_FORMDATA
};

static void resolve_body_promise(
  ant_t *js, ant_value_t promise,
  const uint8_t *data, size_t size,
  const char *body_type, int mode, bool has_body
) {
  switch (mode) {
  case BODY_TEXT: {
    ant_value_t str = (data && size > 0)
      ? js_mkstr(js, (const char *)data, size)
      : js_mkstr(js, "", 0);
    js_resolve_promise(js, promise, str);
    break;
  }
  case BODY_JSON: {
    ant_value_t str = (data && size > 0)
      ? js_mkstr(js, (const char *)data, size)
      : js_mkstr(js, "", 0);
    ant_value_t parsed = json_parse_value(js, str);
    if (is_err(parsed)) js_reject_promise(js, promise, request_rejection_reason(js, parsed));
    else js_resolve_promise(js, promise, parsed);
    break;
  }
  case BODY_ARRAYBUFFER: {
    ArrayBufferData *ab = create_array_buffer_data(size);
    if (!ab) { js_reject_promise(js, promise, js_mkerr(js, "out of memory")); break; }
    if (data && size > 0) memcpy(ab->data, data, size);
    js_resolve_promise(js, promise, create_arraybuffer_obj(js, ab));
    break;
  }
  case BODY_BLOB: {
    const char *type = body_type ? body_type : "";
    js_resolve_promise(js, promise, blob_create(js, data, size, type));
    break;
  }
  case BODY_BYTES: {
    ArrayBufferData *ab = create_array_buffer_data(size);
    if (!ab) { js_reject_promise(js, promise, js_mkerr(js, "out of memory")); break; }
    if (data && size > 0) memcpy(ab->data, data, size);
    js_resolve_promise(js, promise,
      create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, size, "Uint8Array"));
    break;
  }
  case BODY_FORMDATA: {
    ant_value_t fd = formdata_parse_body(js, data, size, body_type, has_body);
    if (is_err(fd)) js_reject_promise(js, promise, request_rejection_reason(js, fd));
    else js_resolve_promise(js, promise, fd);
    break;
  }}
}

static uint8_t *concat_chunks(ant_t *js, ant_value_t chunks, size_t *out_size) {
  ant_offset_t n = js_arr_len(js, chunks);
  size_t total = 0;

  for (ant_offset_t i = 0; i < n; i++) {
  ant_value_t chunk = js_arr_get(js, chunks, i);
  if (vtype(chunk) == T_TYPEDARRAY) {
    TypedArrayData *ta = (TypedArrayData *)js_gettypedarray(chunk);
    if (ta && ta->buffer && !ta->buffer->is_detached) total += ta->byte_length;
  }}

  uint8_t *buf = total > 0 ? malloc(total) : NULL;
  if (total > 0 && !buf) return NULL;

  size_t pos = 0;
  for (ant_offset_t i = 0; i < n; i++) {
  ant_value_t chunk = js_arr_get(js, chunks, i);
  
  if (vtype(chunk) == T_TYPEDARRAY) {
  TypedArrayData *ta = (TypedArrayData *)js_gettypedarray(chunk);
  if (ta && ta->buffer && !ta->buffer->is_detached && ta->byte_length > 0) {
    memcpy(buf + pos, ta->buffer->data + ta->byte_offset, ta->byte_length);
    pos += ta->byte_length;
  }}}

  *out_size = pos;
  return buf;
}

static ant_value_t stream_body_read(ant_t *js, ant_value_t *args, int nargs);

static ant_value_t stream_body_rejected(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state   = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t promise = js_get(js, state, "promise");
  ant_value_t reason  = (nargs > 0) ? args[0] : js_mkundef();
  js_reject_promise(js, promise, reason);
  return js_mkundef();
}

static void stream_schedule_next_read(ant_t *js, ant_value_t state, ant_value_t read_fn, ant_value_t reader) {
  ant_value_t next_p  = rs_default_reader_read(js, reader);
  ant_value_t fulfill = js_heavy_mkfun(js, stream_body_read, state);
  ant_value_t reject  = js_heavy_mkfun(js, stream_body_rejected, state);
  ant_value_t then_result = js_promise_then(js, next_p, fulfill, reject);
  promise_mark_handled(then_result);
}

static ant_value_t stream_body_read(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state   = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t result  = (nargs > 0) ? args[0] : js_mkundef();
  ant_value_t promise = js_get(js, state, "promise");
  ant_value_t reader  = js_get(js, state, "reader");
  ant_value_t chunks  = js_get(js, state, "chunks");
  int mode            = (int)js_getnum(js_get(js, state, "mode"));

  ant_value_t done_val = js_get(js, result, "done");
  ant_value_t value    = js_get(js, result, "value");

  if (vtype(done_val) == T_BOOL && done_val == js_true) {
    size_t size = 0;
    uint8_t *data = concat_chunks(js, chunks, &size);
    ant_value_t type_v = js_get(js, state, "type");
    const char *body_type = (vtype(type_v) == T_STR) ? js_getstr(js, type_v, NULL) : NULL;
    resolve_body_promise(js, promise, data, size, body_type, mode, true);
    free(data);
    return js_mkundef();
  }

  if (vtype(value) != T_UNDEF && vtype(value) != T_NULL)
    js_arr_push(js, chunks, value);

  stream_schedule_next_read(js, state, js_mkundef(), reader);
  return js_mkundef();
}

static ant_value_t consume_body_from_stream(
  ant_t *js, ant_value_t stream,
  ant_value_t promise, int mode,
  const char *body_type
) {
  ant_value_t reader_args[1] = { stream };
  ant_value_t saved = js->new_target;
  
  js->new_target = g_reader_proto;
  ant_value_t reader = js_rs_reader_ctor(js, reader_args, 1);
  js->new_target = saved;
  
  if (is_err(reader)) { 
    js_reject_promise(js, promise, reader);
    return promise;
  }

  ant_value_t state = js_mkobj(js);
  js_set(js, state, "promise", promise);
  js_set(js, state, "reader",  reader);
  js_set(js, state, "chunks",  js_mkarr(js));
  js_set(js, state, "mode",    js_mknum(mode));
  js_set(js, state, "type",    body_type ? js_mkstr(js, body_type, strlen(body_type)) : js_mkundef());

  stream_schedule_next_read(js, state, js_mkundef(), reader);
  return promise;
}

static ant_value_t consume_body(ant_t *js, int mode) {
  ant_value_t this = js_getthis(js);
  request_data_t *d = get_data(this);
  ant_value_t promise = js_mkpromise(js);

  if (!d) {
    js_reject_promise(js, promise, request_rejection_reason(js,
      js_mkerr_typed(js, JS_ERR_TYPE, "Invalid Request object")));
    return promise;
  }
  
  if (!d->has_body) {
    resolve_body_promise(js, promise, NULL, 0, request_effective_body_type(js, this, d), mode, false);
    return promise;
  }
  
  if (d->body_used) {
    js_reject_promise(js, promise, request_rejection_reason(js,
      js_mkerr_typed(js, JS_ERR_TYPE, "body stream is disturbed or locked")));
    return promise;
  }
  
  d->body_used = true;
  ant_value_t stream = js_get_slot(this, SLOT_REQUEST_BODY_STREAM);
  if (rs_is_stream(stream) && d->body_is_stream)
    return consume_body_from_stream(js, stream, promise, mode, request_effective_body_type(js, this, d));
  resolve_body_promise(js, promise, d->body_data, d->body_size, request_effective_body_type(js, this, d), mode, true);
  
  return promise;
}

static ant_value_t js_req_text(ant_t *js, ant_value_t *args, int nargs) {
  return consume_body(js, BODY_TEXT);
}

static ant_value_t js_req_json(ant_t *js, ant_value_t *args, int nargs) {
  return consume_body(js, BODY_JSON);
}

static ant_value_t js_req_array_buffer(ant_t *js, ant_value_t *args, int nargs) {
  return consume_body(js, BODY_ARRAYBUFFER);
}

static ant_value_t js_req_blob(ant_t *js, ant_value_t *args, int nargs) {
  return consume_body(js, BODY_BLOB);
}

static ant_value_t js_req_bytes(ant_t *js, ant_value_t *args, int nargs) {
  return consume_body(js, BODY_BYTES);
}

static ant_value_t js_req_form_data(ant_t *js, ant_value_t *args, int nargs) {
  return consume_body(js, BODY_FORMDATA);
}

static ant_value_t request_set_extracted_body(
  ant_t *js, ant_value_t req_obj, ant_value_t headers, request_data_t *req,
  uint8_t *body_data, size_t body_size, char *body_type,
  ant_value_t body_stream, bool duplex_provided
) {
  free(req->body_data);
  free(req->body_type);

  req->body_data = body_data;
  req->body_size = body_size;
  req->body_type = body_type;
  req->body_is_stream = rs_is_stream(body_stream);
  req->has_body = true;

  if (!req->body_is_stream) {
    if (body_type && body_type[0]) headers_append_if_missing(headers, "content-type", body_type);
    return js_mkundef();
  }

  if (req->keepalive) {
    return js_mkerr_typed(js, JS_ERR_TYPE,
    "Failed to construct 'Request': keepalive cannot be used with a ReadableStream body");
  }
  if (!duplex_provided) {
    return js_mkerr_typed(js, JS_ERR_TYPE,
    "Failed to construct 'Request': duplex must be provided for a ReadableStream body");
  }

  js_set_slot_wb(js, req_obj, SLOT_REQUEST_BODY_STREAM, body_stream);
  if (body_type && body_type[0]) headers_append_if_missing(headers, "content-type", body_type);
  return js_mkundef();
}

static void request_clear_body(ant_t *js, ant_value_t req_obj, request_data_t *req) {
  free(req->body_data);
  free(req->body_type);
  req->body_data = NULL;
  req->body_size = 0;
  req->body_type = NULL;
  req->body_is_stream = false;
  req->has_body = false;
  js_set_slot_wb(js, req_obj, SLOT_REQUEST_BODY_STREAM, js_mkundef());
}

static ant_value_t request_copy_source_body(ant_t *js, ant_value_t req_obj, ant_value_t input, request_data_t *req, request_data_t *src) {
  ant_value_t src_stream = js_get_slot(input, SLOT_REQUEST_BODY_STREAM);

  if (src->body_used) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot construct Request with unusable body");
  }

  if (rs_is_stream(src_stream) && rs_stream_unusable(src_stream)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "body stream is disturbed or locked");
  }

  if (!src->body_is_stream) {
    req->body_data = malloc(src->body_size);
    if (src->body_size > 0 && !req->body_data) return js_mkerr(js, "out of memory");
    if (src->body_size > 0) memcpy(req->body_data, src->body_data, src->body_size);
    req->body_size = src->body_size;
    req->body_type = src->body_type ? strdup(src->body_type) : NULL;
    req->body_is_stream = false;
    req->has_body = true;
    return js_mkundef();
  }

  if (!rs_is_stream(src_stream)) return js_mkundef();
  ant_value_t branches = readable_stream_tee(js, src_stream);
  
  if (is_err(branches)) return branches;
  if (vtype(branches) != T_ARR) {
    return js_mkerr_typed(js, JS_ERR_TYPE,
    "Failed to construct 'Request': tee() did not return branches");
  }

  js_set_slot_wb(js, req_obj, SLOT_REQUEST_BODY_STREAM, js_arr_get(js, branches, 1));
  req->body_is_stream = true;
  req->has_body = true;
  
  return js_mkundef();
}

#define REQ_GETTER_START(name)                                                    \
  static ant_value_t js_req_get_##name(ant_t *js, ant_value_t *args, int nargs) { \
    ant_value_t this = js_getthis(js);                                            \
    request_data_t *d = get_data(this);                                           \
    if (!d) return js_mkundef();

#define REQ_GETTER_END }

REQ_GETTER_START(method)
  return js_mkstr(js, d->method, strlen(d->method));
REQ_GETTER_END

REQ_GETTER_START(url)
  char *href = build_href(&d->url);
  if (!href) return js_mkstr(js, "", 0);
  ant_value_t ret = js_mkstr(js, href, strlen(href));
  free(href);
  return ret;
REQ_GETTER_END

REQ_GETTER_START(headers)
  return js_get_slot(this, SLOT_REQUEST_HEADERS);
REQ_GETTER_END

REQ_GETTER_START(destination)
  (void)d;
  return js_mkstr(js, "", 0);
REQ_GETTER_END

REQ_GETTER_START(referrer)
  if (!d->referrer || strcmp(d->referrer, "no-referrer") == 0)
    return js_mkstr(js, "", 0);
  if (strcmp(d->referrer, "client") == 0)
    return js_mkstr(js, "about:client", 12);
  return js_mkstr(js, d->referrer, strlen(d->referrer));
REQ_GETTER_END

REQ_GETTER_START(referrer_policy)
  const char *p = d->referrer_policy ? d->referrer_policy : "";
  return js_mkstr(js, p, strlen(p));
REQ_GETTER_END

REQ_GETTER_START(mode)
  return js_mkstr(js, d->mode, strlen(d->mode));
REQ_GETTER_END

REQ_GETTER_START(credentials)
  return js_mkstr(js, d->credentials, strlen(d->credentials));
REQ_GETTER_END

REQ_GETTER_START(cache)
  return js_mkstr(js, d->cache, strlen(d->cache));
REQ_GETTER_END

REQ_GETTER_START(redirect)
  return js_mkstr(js, d->redirect, strlen(d->redirect));
REQ_GETTER_END

REQ_GETTER_START(integrity)
  const char *ig = d->integrity ? d->integrity : "";
  return js_mkstr(js, ig, strlen(ig));
REQ_GETTER_END

REQ_GETTER_START(keepalive)
  return js_bool(d->keepalive);
REQ_GETTER_END

REQ_GETTER_START(is_reload_navigation)
  return js_bool(d->reload_navigation);
REQ_GETTER_END

REQ_GETTER_START(is_history_navigation)
  return js_bool(d->history_navigation);
REQ_GETTER_END

REQ_GETTER_START(signal)
  return request_get_signal(js, this);
REQ_GETTER_END

REQ_GETTER_START(duplex)
  return js_mkstr(js, "half", 4);
REQ_GETTER_END

static ant_value_t req_body_pull(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t req_obj = js_get_slot(js->current_func, SLOT_DATA);
  request_data_t *d = get_data(req_obj);
  ant_value_t ctrl = (nargs > 0) ? args[0] : js_mkundef();

  if (d && d->body_data && d->body_size > 0) {
  ArrayBufferData *ab = create_array_buffer_data(d->body_size);
  if (ab) {
    memcpy(ab->data, d->body_data, d->body_size);
    rs_controller_enqueue(js, ctrl,
    create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, d->body_size, "Uint8Array"));
  }}
  
  rs_controller_close(js, ctrl);
  return js_mkundef();
}

REQ_GETTER_START(body)
  if (!d->has_body) return js_mknull();
  ant_value_t stored_stream = js_get_slot(this, SLOT_REQUEST_BODY_STREAM);
  if (rs_is_stream(stored_stream)) return stored_stream;
  if (d->body_used) return js_mknull();
  ant_value_t pull = js_heavy_mkfun(js, req_body_pull, this);
  ant_value_t stream = rs_create_stream(js, pull, js_mkundef(), 1.0);
  if (!is_err(stream)) js_set_slot_wb(js, this, SLOT_REQUEST_BODY_STREAM, stream);
  return stream;
REQ_GETTER_END

REQ_GETTER_START(body_used)
  return js_bool(d->body_used);
REQ_GETTER_END

#undef REQ_GETTER_START
#undef REQ_GETTER_END

static ant_value_t request_inspect_finish(ant_t *js, ant_value_t this_obj, ant_value_t body_obj) {
  ant_value_t tag_val = js_get_sym(js, this_obj, get_toStringTag_sym());
  const char *tag = vtype(tag_val) == T_STR ? js_getstr(js, tag_val, NULL) : "Request";

  js_inspect_builder_t builder;
  if (!js_inspect_builder_init_dynamic(&builder, js, 128)) {
    return js_mkerr(js, "out of memory");
  }

  bool ok = js_inspect_header_for(&builder, body_obj, "%s", tag);
  if (ok) ok = js_inspect_object_body(&builder, body_obj);
  if (ok) ok = js_inspect_close(&builder);

  if (!ok) {
    js_inspect_builder_dispose(&builder);
    return js_mkerr(js, "out of memory");
  }

  return js_inspect_builder_result(&builder);
}

// TODO: make dry
static bool request_inspect_set(
  ant_t *js, ant_value_t obj, const char *key, 
  ant_value_t value, ant_value_t *err_out
) {
  if (is_err(value)) {
    *err_out = value;
    return false;
  }

  js_set(js, obj, key, value);
  return true;
}

static ant_value_t request_inspect(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  ant_value_t out = js_mkobj(js);
  ant_value_t err = 0;

  if (!request_inspect_set(js, out, "method", js_req_get_method(js, NULL, 0), &err)) return err;
  if (!request_inspect_set(js, out, "url", js_req_get_url(js, NULL, 0), &err)) return err;
  if (!request_inspect_set(js, out, "headers", js_req_get_headers(js, NULL, 0), &err)) return err;
  if (!request_inspect_set(js, out, "destination", js_req_get_destination(js, NULL, 0), &err)) return err;
  if (!request_inspect_set(js, out, "referrer", js_req_get_referrer(js, NULL, 0), &err)) return err;
  if (!request_inspect_set(js, out, "referrerPolicy", js_req_get_referrer_policy(js, NULL, 0), &err)) return err;
  if (!request_inspect_set(js, out, "mode", js_req_get_mode(js, NULL, 0), &err)) return err;
  if (!request_inspect_set(js, out, "credentials", js_req_get_credentials(js, NULL, 0), &err)) return err;
  if (!request_inspect_set(js, out, "cache", js_req_get_cache(js, NULL, 0), &err)) return err;
  if (!request_inspect_set(js, out, "redirect", js_req_get_redirect(js, NULL, 0), &err)) return err;
  if (!request_inspect_set(js, out, "integrity", js_req_get_integrity(js, NULL, 0), &err)) return err;
  if (!request_inspect_set(js, out, "keepalive", js_req_get_keepalive(js, NULL, 0), &err)) return err;
  if (!request_inspect_set(js, out, "isReloadNavigation", js_req_get_is_reload_navigation(js, NULL, 0), &err)) return err;
  if (!request_inspect_set(js, out, "isHistoryNavigation", js_req_get_is_history_navigation(js, NULL, 0), &err)) return err;
  if (!request_inspect_set(js, out, "signal", js_req_get_signal(js, NULL, 0), &err)) return err;

  return request_inspect_finish(js, this_obj, out);
}

static ant_value_t js_request_clone(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this = js_getthis(js);
  request_data_t *d = get_data(this);
  
  if (!d) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid Request object");
  if (d->body_used)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot clone a Request whose body is unusable");

  request_data_t *nd = data_dup(d);
  if (!nd) return js_mkerr(js, "out of memory");

  ant_value_t src_headers = js_get_slot(this, SLOT_REQUEST_HEADERS);
  ant_value_t src_signal  = request_get_signal(js, this);

  ant_value_t new_headers = headers_create_empty(js);
  if (is_err(new_headers)) { data_free(nd); return new_headers; }
  
  headers_copy_from(js, new_headers, src_headers);
  headers_set_guard(new_headers,
    strcmp(nd->mode, "no-cors") == 0 
    ? HEADERS_GUARD_REQUEST_NO_CORS 
    : HEADERS_GUARD_REQUEST
  );
  headers_apply_guard(new_headers);

  ant_value_t new_signal = abort_signal_create_dependent(js, src_signal);
  if (is_err(new_signal)) { data_free(nd); return new_signal; }

  ant_value_t obj = js_mkobj(js);
  js_set_proto_init(obj, g_request_proto);
  js_set_slot(obj, SLOT_BRAND, js_mknum(BRAND_REQUEST));
  js_set_native(obj, nd, REQUEST_NATIVE_TAG);
  
  js_set_slot_wb(js, obj, SLOT_REQUEST_HEADERS, new_headers);
  js_set_slot(obj, SLOT_REQUEST_ABORT_REASON, js_mkundef());
  js_set_slot_wb(js, obj, SLOT_REQUEST_SIGNAL,  new_signal);

  ant_value_t src_stream = js_get_slot(this, SLOT_REQUEST_BODY_STREAM);
  if (rs_is_stream(src_stream)) {
  ant_value_t branches = readable_stream_tee(js, src_stream);
  if (!is_err(branches) && vtype(branches) == T_ARR) {
    ant_value_t b1 = js_arr_get(js, branches, 0);
    ant_value_t b2 = js_arr_get(js, branches, 1);
    js_set_slot_wb(js, this, SLOT_REQUEST_BODY_STREAM, b1);
    js_set_slot_wb(js, obj,  SLOT_REQUEST_BODY_STREAM, b2);
  }}

  return obj;
}

static const char *init_str(ant_t *js, ant_value_t init, const char *key, size_t klen, ant_value_t *err_out) {
  ant_value_t v = js_get(js, init, key);
  if (vtype(v) == T_UNDEF) return NULL;
  if (vtype(v) != T_STR) {
    v = js_tostring_val(js, v);
    if (is_err(v)) { *err_out = v; return NULL; }
  }
  return js_getstr(js, v, NULL);
}

static ant_value_t request_new_from_input(
  ant_t *js, ant_value_t input,
  request_data_t **out_req, request_data_t **out_src,
  ant_value_t *out_input_signal
) {
  request_data_t *req = NULL;
  request_data_t *src = NULL;

  *out_req = NULL;
  *out_src = NULL;
  *out_input_signal = js_mkundef();

  if (
    vtype(input) == T_OBJ && 
    js_check_brand(input, BRAND_REQUEST)
  ) src = get_data(input);

  if (!src) {
    size_t ulen = 0;
    const char *url_str = NULL;
    url_state_t parsed = {0};

    if (vtype(input) != T_STR) {
      input = js_tostring_val(js, input);
      if (is_err(input)) return input;
    }

    url_str = js_getstr(js, input, &ulen);
    if (parse_url_to_state(url_str, NULL, &parsed) != 0) {
      url_state_clear(&parsed);
      return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to construct 'Request': Invalid URL");
    }
    
    if ((parsed.username && parsed.username[0]) || (parsed.password && parsed.password[0])) {
      url_state_clear(&parsed);
      return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to construct 'Request': URL includes credentials");
    }

    req = data_new();
    if (!req) {
      url_state_clear(&parsed);
      return js_mkerr(js, "out of memory");
    }
    
    req->url = parsed;
  } else {
    req = data_dup(src);
    if (!req) return js_mkerr(js, "out of memory");
    req->body_used = false;
    *out_input_signal = request_get_signal(js, input);
  }

  *out_req = req;
  *out_src = src;
  return js_mkundef();
}

static ant_value_t request_apply_init_options(
  ant_t *js, ant_value_t init, request_data_t *req, ant_value_t *input_signal
) {
  ant_value_t err = js_mkundef();
  ant_value_t win = js_get(js, init, "window");
  
  const char *ref = NULL;
  const char *rp = NULL;
  const char *mode_val = NULL;
  const char *cred = NULL;
  const char *cache_val = NULL;
  const char *redir = NULL;
  const char *integ = NULL;
  const char *method_val = NULL;

  if (vtype(win) != T_UNDEF && vtype(win) != T_NULL) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to construct 'Request': 'window' must be null");
  }

  if (strcmp(req->mode, "navigate") == 0) {
    free(req->mode);
    req->mode = strdup("same-origin");
  }
  
  req->reload_navigation = false;
  req->history_navigation = false;
  free(req->referrer);
  req->referrer = strdup("client");
  free(req->referrer_policy);
  req->referrer_policy = strdup("");

  ref = init_str(js, init, "referrer", 8, &err);
  if (is_err(err)) return err;
  
  if (ref) {
  if (ref[0] == '\0') {
    free(req->referrer);
    req->referrer = strdup("no-referrer");
  } else {
    url_state_t rs = {0};
    if (parse_url_to_state(ref, NULL, &rs) != 0) {
      url_state_clear(&rs);
      return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to construct 'Request': Invalid referrer URL");
    }
    free(req->referrer);
    req->referrer = build_href(&rs);
    url_state_clear(&rs);
    if (!req->referrer) req->referrer = strdup("client");
  }}

  rp = init_str(js, init, "referrerPolicy", 14, &err);
  if (is_err(err)) return err;
  if (rp) {
    free(req->referrer_policy);
    req->referrer_policy = strdup(rp);
  }

  mode_val = init_str(js, init, "mode", 4, &err);
  if (is_err(err)) return err;
  if (mode_val) {
    if (strcmp(mode_val, "navigate") == 0) {
      return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to construct 'Request': mode 'navigate' is not allowed");
    }
    free(req->mode);
    req->mode = strdup(mode_val);
  }

  cred = init_str(js, init, "credentials", 11, &err);
  if (is_err(err)) return err;
  if (cred) {
    free(req->credentials);
    req->credentials = strdup(cred);
  }

  cache_val = init_str(js, init, "cache", 5, &err);
  if (is_err(err)) return err;
  
  if (cache_val) {
    free(req->cache);
    req->cache = strdup(cache_val);
    if (
      strcmp(req->cache, "only-if-cached") == 0 &&
      strcmp(req->mode, "same-origin") != 0
    ) return js_mkerr_typed(js, JS_ERR_TYPE,
      "Failed to construct 'Request': cache mode 'only-if-cached' requires mode 'same-origin'");
  }

  redir = init_str(js, init, "redirect", 8, &err);
  if (is_err(err)) return err;
  
  if (redir) {
    free(req->redirect);
    req->redirect = strdup(redir);
  }

  integ = init_str(js, init, "integrity", 9, &err);
  if (is_err(err)) return err;
  
  if (integ) {
    free(req->integrity);
    req->integrity = strdup(integ);
  }

  ant_value_t ka = js_get(js, init, "keepalive");
  if (vtype(ka) != T_UNDEF) req->keepalive = js_truthy(js, ka);

  method_val = init_str(js, init, "method", 6, &err);
  if (is_err(err)) return err;
  
  if (method_val) {
    if (!is_valid_method(method_val)) {
      return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to construct 'Request': Invalid method");
    }
    if (is_forbidden_method(method_val)) {
      return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to construct 'Request': Forbidden method");
    }
    
    free(req->method);
    req->method = strdup(method_val);
    normalize_method(req->method);
  }

  ant_value_t sig_val = js_get(js, init, "signal");
  if (vtype(sig_val) == T_UNDEF) return js_mkundef();
  
  if (vtype(sig_val) == T_NULL) {
    *input_signal = js_mkundef();
    return js_mkundef();
  }
  
  if (abort_signal_is_signal(sig_val)) {
    *input_signal = sig_val;
    return js_mkundef();
  }

  return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to construct 'Request': signal must be an AbortSignal");
}

static ant_value_t request_create_ctor_headers(ant_t *js, ant_value_t input) {
  ant_value_t headers = headers_create_empty(js);
  if (is_err(headers)) return headers;
  if (vtype(input) != T_OBJ) return headers;

  ant_value_t src_hdrs = js_get_slot(input, SLOT_REQUEST_HEADERS);
  headers_copy_from(js, headers, src_hdrs);
  return headers;
}

static ant_value_t request_apply_init_headers(ant_t *js, ant_value_t init, ant_value_t headers) {
  ant_value_t init_headers = js_get(js, init, "headers");
  if (vtype(init_headers) == T_UNDEF) return headers;
  return headers_create_from_init(js, init_headers);
}

static ant_value_t request_parse_duplex(ant_t *js, ant_value_t init, bool *out_duplex_provided) {
  ant_value_t duplex_val = js_get(js, init, "duplex");
  ant_value_t duplex_str_v = duplex_val;
  const char *duplex_str = NULL;

  *out_duplex_provided = vtype(duplex_val) != T_UNDEF;
  if (!*out_duplex_provided) return js_mkundef();

  if (vtype(duplex_str_v) != T_STR) {
    duplex_str_v = js_tostring_val(js, duplex_str_v);
    if (is_err(duplex_str_v)) return duplex_str_v;
  }

  duplex_str = js_getstr(js, duplex_str_v, NULL);
  if (duplex_str && strcmp(duplex_str, "half") == 0) return js_mkundef();

  return js_mkerr_typed(js, JS_ERR_TYPE,
    "Failed to construct 'Request': duplex must be 'half'");
}

static ant_value_t request_apply_ctor_body(
  ant_t *js, ant_value_t req_obj, ant_value_t input, ant_value_t init,
  bool init_provided, bool duplex_provided,
  request_data_t *req, request_data_t *src, ant_value_t headers
) {
  if (init_provided) {
    ant_value_t body_val = js_get(js, init, "body");
    bool init_body_present = vtype(body_val) != T_UNDEF;
    bool input_body_present = src && src->has_body;
    bool effective_body_present =
      (init_body_present && vtype(body_val) != T_NULL) ||
      (input_body_present && (!init_body_present || vtype(body_val) == T_NULL));

    if ((strcmp(req->method, "GET") == 0 || strcmp(req->method, "HEAD") == 0) && effective_body_present) {
      return js_mkerr_typed(js, JS_ERR_TYPE,
      "Failed to construct 'Request': Request with GET/HEAD method cannot have body");
    }

    if (vtype(body_val) == T_UNDEF) return js_mkundef();
    if (vtype(body_val) == T_NULL) {
      request_clear_body(js, req_obj, req);
      return js_mkundef();
    }

    request_data_t *init_req = get_data(init);
    ant_value_t body_err = js_mkundef();
    ant_value_t body_stream = js_mkundef();
    
    uint8_t *bd = NULL;
    size_t bs = 0;
    char *bt = NULL;
    
    if (init_req && !init_req->body_used && !init_req->body_is_stream && init_req->has_body) {
      bd = malloc(init_req->body_size);
      if (init_req->body_size > 0 && !bd) return js_mkerr(js, "out of memory");
      if (init_req->body_size > 0) memcpy(bd, init_req->body_data, init_req->body_size);
      bs = init_req->body_size;
      bt = init_req->body_type ? strdup(init_req->body_type) : NULL;
    } else if (!extract_body(js, body_val, &bd, &bs, &bt, &body_stream, &body_err)) 
      return is_err(body_err) ? body_err : js_mkerr(js, "Failed to extract body");
    
    return request_set_extracted_body(
      js, req_obj, headers, req, bd, 
      bs, bt, body_stream, duplex_provided
    );
  }

  if (!src) return js_mkundef();
  return request_copy_source_body(js, req_obj, input, req, src);
}

static ant_value_t js_request_ctor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t init = (nargs >= 2 && vtype(args[1]) != T_UNDEF) ? args[1] : js_mkundef();

  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Request constructor requires 'new'");
  if (nargs < 1)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Request constructor requires at least 1 argument");

  ant_value_t input  = args[0];
  ant_value_t obj = 0;
  ant_value_t proto = 0;
  bool init_provided = false;
  
  request_data_t *req = NULL;
  request_data_t *src = NULL;
  
  ant_value_t input_signal = js_mkundef();
  ant_value_t step = js_mkundef();
  ant_value_t signal = 0;
  ant_value_t headers = 0;
  
  bool duplex_provided = false;
  init_provided = (vtype(init) == T_OBJ || vtype(init) == T_ARR);
  
  step = request_new_from_input(js, input, &req, &src, &input_signal);
  if (is_err(step)) return step;

  if (init_provided) {
    step = request_apply_init_options(js, init, req, &input_signal);
    if (is_err(step)) { data_free(req); return step; }
  }

  if (
    strcmp(req->mode, "no-cors") == 0 &&
    !is_cors_safelisted_method(req->method)
  ) {
    data_free(req);
    return js_mkerr_typed(js, JS_ERR_TYPE,
    "Failed to construct 'Request': method must be one of GET, HEAD, POST for no-cors mode");
  }

  obj = js_mkobj(js);
  proto = js_instance_proto_from_new_target(js, g_request_proto);
  
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  else js_set_proto_init(obj, g_request_proto);
  
  js_set_slot(obj, SLOT_BRAND, js_mknum(BRAND_REQUEST));
  js_set_native(obj, req, REQUEST_NATIVE_TAG);
  js_set_slot(obj, SLOT_REQUEST_ABORT_REASON, js_mkundef());

  signal = abort_signal_create_dependent(js, input_signal);
  if (is_err(signal)) { data_free(req); return signal; }
  js_set_slot_wb(js, obj, SLOT_REQUEST_SIGNAL, signal);

  headers = request_create_ctor_headers(js, input);
  if (is_err(headers)) { data_free(req); return headers; }
  
  if (init_provided) {
    headers = request_apply_init_headers(js, init, headers);
    if (is_err(headers)) { data_free(req); return headers; }
  }

  headers_set_guard(headers,
    strcmp(req->mode, "no-cors") == 0
    ? HEADERS_GUARD_REQUEST_NO_CORS
    : HEADERS_GUARD_REQUEST
  );
  
  headers_apply_guard(headers);
  js_set_slot_wb(js, obj, SLOT_REQUEST_HEADERS, headers);

  if (init_provided) {
    step = request_parse_duplex(js, init, &duplex_provided);
    if (is_err(step)) { data_free(req); return step; }
  }

  step = request_apply_ctor_body(
    js, obj, input, init, init_provided,
    duplex_provided, req, src, headers
  );
  
  if (is_err(step)) {
    data_free(req);
    return step;
  }

  if (src && src->has_body && !src->body_used)
    src->body_used = true;

  return obj;
}

ant_value_t request_create_from_input_init(ant_t *js, ant_value_t input, ant_value_t init) {
  bool init_provided = (vtype(init) == T_OBJ || vtype(init) == T_ARR);
  
  request_data_t *req = NULL;
  request_data_t *src = NULL;
  
  ant_value_t input_signal = js_mkundef();
  ant_value_t step = js_mkundef();
  ant_value_t obj = 0;
  ant_value_t signal = 0;
  ant_value_t headers = 0;
  
  bool duplex_provided = false;
  step = request_new_from_input(js, input, &req, &src, &input_signal);
  if (is_err(step)) return step;

  if (init_provided) {
    step = request_apply_init_options(js, init, req, &input_signal);
    if (is_err(step)) { data_free(req); return step; }
  }

  if (
    strcmp(req->mode, "no-cors") == 0 &&
    !is_cors_safelisted_method(req->method)
  ) {
    data_free(req);
    return js_mkerr_typed(js, JS_ERR_TYPE,
    "Failed to construct 'Request': method must be one of GET, HEAD, POST for no-cors mode");
  }

  obj = js_mkobj(js);
  js_set_proto_init(obj, g_request_proto);
  js_set_slot(obj, SLOT_BRAND, js_mknum(BRAND_REQUEST));
  js_set_native(obj, req, REQUEST_NATIVE_TAG);
  js_set_slot(obj, SLOT_REQUEST_ABORT_REASON, js_mkundef());

  signal = abort_signal_create_dependent(js, input_signal);
  if (is_err(signal)) { data_free(req); return signal; }
  js_set_slot_wb(js, obj, SLOT_REQUEST_SIGNAL, signal);

  headers = request_create_ctor_headers(js, input);
  if (is_err(headers)) { data_free(req); return headers; }
  
  if (init_provided) {
    headers = request_apply_init_headers(js, init, headers);
    if (is_err(headers)) { data_free(req); return headers; }
  }

  headers_set_guard(headers,
    strcmp(req->mode, "no-cors") == 0 
    ? HEADERS_GUARD_REQUEST_NO_CORS 
    : HEADERS_GUARD_REQUEST
  );
  headers_apply_guard(headers);
  js_set_slot_wb(js, obj, SLOT_REQUEST_HEADERS, headers);

  if (init_provided) {
    step = request_parse_duplex(js, init, &duplex_provided);
    if (is_err(step)) { data_free(req); return step; }
  }

  step = request_apply_ctor_body(js, obj, input, init, init_provided, duplex_provided, req, src, headers);
  if (is_err(step)) {
    data_free(req);
    return step;
  }

  if (src && src->has_body && !src->body_used)
    src->body_used = true;

  return obj;
}

ant_value_t request_create(ant_t *js,
    const char *method, const char *url,
    ant_value_t headers_obj, const uint8_t *body, size_t body_len,
    const char *body_type) {
  request_data_t *req = data_new();
  if (!req) return js_mkerr(js, "out of memory");

  free(req->method);
  req->method = strdup(method ? method : "GET");
  free(req->mode);
  req->mode = strdup("same-origin");

  url_state_t parsed = {0};
  if (url && parse_url_to_state(url, NULL, &parsed) == 0) req->url = parsed;
  else url_state_clear(&parsed);

  if (body) req->has_body = true;
  
  if (body && body_len > 0) {
    req->body_data = malloc(body_len);
    if (!req->body_data) { data_free(req); return js_mkerr(js, "out of memory"); }
    memcpy(req->body_data, body, body_len);
    req->body_size = body_len;
    req->body_type = body_type ? strdup(body_type) : NULL;
  }
  req->body_is_stream = false;
  return request_create_object(js, req, headers_obj, true);
}

ant_value_t request_create_server(
  ant_t *js,
  const char *method,
  const char *target,
  bool absolute_target,
  const char *host,
  const char *server_hostname,
  int server_port,
  ant_value_t headers_obj,
  const uint8_t *body,
  size_t body_len,
  const char *body_type
) {
  request_data_t *req = data_new_server(method);
  if (!req) return js_mkerr(js, "out of memory");

  if (target && request_parse_server_url(target, absolute_target, host, server_hostname, server_port, &req->url) != 0)
    url_state_clear(&req->url);

  if (body) req->has_body = true;

  if (body && body_len > 0) {
    req->body_data = malloc(body_len);
    if (!req->body_data) { data_free(req); return js_mkerr(js, "out of memory"); }
    memcpy(req->body_data, body, body_len);
    req->body_size = body_len;
    req->body_type = body_type ? strdup(body_type) : NULL;
  }
  req->body_is_stream = false;

  return request_create_object(js, req, headers_obj, false);
}

void init_request_module(void) {
  ant_t *js = rt->js;
  ant_value_t g = js_glob(js);
  g_request_proto = js_mkobj(js);

  js_set(js, g_request_proto, "text", js_mkfun(js_req_text));
  js_set(js, g_request_proto, "json", js_mkfun(js_req_json));
  js_set(js, g_request_proto, "arrayBuffer", js_mkfun(js_req_array_buffer));
  js_set(js, g_request_proto, "blob", js_mkfun(js_req_blob));
  js_set(js, g_request_proto, "formData", js_mkfun(js_req_form_data));
  js_set(js, g_request_proto, "bytes", js_mkfun(js_req_bytes));
  js_set(js, g_request_proto, "clone", js_mkfun(js_request_clone));

#define GETTER(prop, fn) \
  js_set_getter_desc(js, g_request_proto, prop, sizeof(prop)-1, js_mkfun(js_req_get_##fn), JS_DESC_C)
  GETTER("method",            method);
  GETTER("url",               url);
  GETTER("headers",           headers);
  GETTER("destination",       destination);
  GETTER("referrer",          referrer);
  GETTER("referrerPolicy",    referrer_policy);
  GETTER("mode",              mode);
  GETTER("credentials",       credentials);
  GETTER("cache",             cache);
  GETTER("redirect",          redirect);
  GETTER("integrity",         integrity);
  GETTER("keepalive",         keepalive);
  GETTER("isReloadNavigation",is_reload_navigation);
  GETTER("isHistoryNavigation",is_history_navigation);
  GETTER("signal",            signal);
  GETTER("duplex",            duplex);
  GETTER("body",              body);
  GETTER("bodyUsed",          body_used);
#undef GETTER

  js_set_sym(js, g_request_proto, get_inspect_sym(), js_mkfun(request_inspect));
  js_set_sym(js, g_request_proto, get_toStringTag_sym(), js_mkstr(js, "Request", 7));
  ant_value_t ctor = js_make_ctor(js, js_request_ctor, g_request_proto, "Request", 7);
  
  js_set(js, g, "Request", ctor);
  js_set_descriptor(js, g, "Request", 7, JS_DESC_W | JS_DESC_C);
}
