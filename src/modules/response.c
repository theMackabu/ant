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
#include "utf8.h"

#include "modules/assert.h"
#include "modules/blob.h"
#include "modules/buffer.h"
#include "modules/formdata.h"
#include "modules/headers.h"
#include "modules/multipart.h"
#include "modules/response.h"
#include "modules/symbol.h"
#include "modules/url.h"
#include "modules/json.h"
#include "streams/pipes.h"
#include "streams/readable.h"

ant_value_t g_response_proto = 0;

enum { RESPONSE_NATIVE_TAG = 0x52455350u }; // RESP

static response_data_t *get_data(ant_value_t obj) {
  if (!js_check_native_tag(obj, RESPONSE_NATIVE_TAG)) return NULL;
  return (response_data_t *)js_get_native_ptr(obj);
}

response_data_t *response_get_data(ant_value_t obj) {
  return get_data(obj);
}

ant_value_t response_get_headers(ant_value_t obj) {
  return js_get_slot(obj, SLOT_RESPONSE_HEADERS);
}

static void data_free(response_data_t *d) {
  if (!d) return;
  free(d->type);
  url_state_clear(&d->url);
  free(d->status_text);
  free(d->body_data);
  free(d->body_type);
  free(d);
}

static response_data_t *data_new(void) {
  response_data_t *d = calloc(1, sizeof(response_data_t));
  if (!d) return NULL;
  d->type = strdup("default");
  d->status = 200;
  d->status_text = strdup("");
  d->url_list_size = 0;
  if (!d->type || !d->status_text) {
    data_free(d);
    return NULL;
  }
  return d;
}

static response_data_t *data_dup(const response_data_t *src) {
  response_data_t *d = calloc(1, sizeof(response_data_t));
  url_state_t *su = NULL;
  url_state_t *du = NULL;

  if (!d) return NULL;
  d->type = src->type ? strdup(src->type) : NULL;
  d->status_text = src->status_text ? strdup(src->status_text) : NULL;
  d->has_url = src->has_url;
  d->url_list_size = src->url_list_size;
  d->status = src->status;
  d->body_is_stream = src->body_is_stream;
  d->has_body = src->has_body;
  d->body_used = src->body_used;
  d->body_size = src->body_size;
  d->body_type = src->body_type ? strdup(src->body_type) : NULL;

  su = (url_state_t *)&src->url;
  du = &d->url;
#define DUP_US(f) do { du->f = su->f ? strdup(su->f) : NULL; } while (0)
  DUP_US(protocol);
  DUP_US(username);
  DUP_US(password);
  DUP_US(hostname);
  DUP_US(port);
  DUP_US(pathname);
  DUP_US(search);
  DUP_US(hash);
#undef DUP_US

  if (src->body_data && src->body_size > 0) {
    d->body_data = malloc(src->body_size);
    if (!d->body_data) {
      data_free(d);
      return NULL;
    }
    memcpy(d->body_data, src->body_data, src->body_size);
  }

  return d;
}

static ant_value_t response_rejection_reason(ant_t *js, ant_value_t value) {
  if (!is_err(value)) return value;
  ant_value_t reason = js->thrown_exists ? js->thrown_value : value;
  js->thrown_exists = false;
  js->thrown_value = js_mkundef();
  js->thrown_stack = js_mkundef();
  return reason;
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
  if (!serialized) return true;
  
  *out_data = (uint8_t *)serialized;
  *out_size = strlen(serialized);
  *out_type = strdup("application/x-www-form-urlencoded;charset=UTF-8");
  
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

  if (!boundary) return true;

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
  *out_data = NULL;
  *out_size = 0;
  *out_type = NULL;
  *out_stream = js_mkundef();
  *err_out = js_mkundef();

  if (vtype(body_val) == T_NULL || vtype(body_val) == T_UNDEF) return true;
  if (extract_buffer_source_body(js, body_val, out_data, out_size, err_out)) return true;
  if (vtype(body_val) == T_OBJ && rs_is_stream(body_val)) return extract_stream_body(js, body_val, out_stream, err_out);
  if (vtype(body_val) == T_OBJ && extract_blob_body(js, body_val, out_data, out_size, out_type, err_out)) return true;
  if (vtype(body_val) == T_OBJ && extract_urlsearchparams_body(js, body_val, out_data, out_size, out_type)) return true;
  if (vtype(body_val) == T_OBJ && extract_formdata_body(js, body_val, out_data, out_size, out_type, err_out)) return true;
  
  return extract_string_body(js, body_val, out_data, out_size, out_type, err_out);
}

static bool response_content_type_has_charset(const char *value) {
  const char *p = NULL;

  if (!value) return false;
  p = strchr(value, ';');
  
  while (p) {
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (strncasecmp(p, "charset", 7) == 0) {
      p += 7;
      while (*p == ' ' || *p == '\t') p++;
      if (*p == '=') return true;
    }
    p = strchr(p, ';');
  }

  return false;
}

static void response_maybe_normalize_text_content_type(
  ant_t *js, ant_value_t headers, ant_value_t current_type, const char *body_type
) {
  const char *current = NULL;

  if (!body_type || !headers_is_headers(headers)) return;
  if (vtype(current_type) != T_STR) return;

  current = js_getstr(js, current_type, NULL);
  if (!current) return;
  if (strncasecmp(current, "text/", 5) != 0) return;
  if (response_content_type_has_charset(current)) return;
  if (!response_content_type_has_charset(body_type)) return;

  headers_set_literal(js, headers, "content-type", body_type);
}

enum {
  BODY_TEXT = 0,
  BODY_JSON,
  BODY_ARRAYBUFFER,
  BODY_BLOB,
  BODY_BYTES,
  BODY_FORMDATA
};

static const char *response_effective_body_type(ant_t *js, ant_value_t resp_obj, response_data_t *d) {
  ant_value_t headers = js_get_slot(resp_obj, SLOT_RESPONSE_HEADERS);
  if (!headers_is_headers(headers)) return d ? d->body_type : NULL;
  ant_value_t ct = headers_get_value(js, headers, "content-type");
  if (vtype(ct) == T_STR) return js_getstr(js, ct, NULL);
  return d ? d->body_type : NULL;
}

static void strip_utf8_bom(const uint8_t **data, size_t *size) {
  if (!data || !*data || !size || *size < 3) return;
  if ((*data)[0] == 0xEF && (*data)[1] == 0xBB && (*data)[2] == 0xBF) { *data += 3; *size -= 3; }
}

static void resolve_body_promise(
  ant_t *js, ant_value_t promise,
  const uint8_t *data, size_t size,
  const char *body_type, int mode, bool has_body
) {
  switch (mode) {
  case BODY_TEXT: {
    ant_value_t str = (data && size > 0) ? js_mkstr(js, (const char *)data, size) : js_mkstr(js, "", 0);
    js_resolve_promise(js, promise, str);
    break;
  }
  case BODY_JSON: {
    const uint8_t *json_data = data;
    size_t json_size = size;
    strip_utf8_bom(&json_data, &json_size);
    
    ant_value_t str = (json_data && json_size > 0)
      ? js_mkstr(js, (const char *)json_data, json_size)
      : js_mkstr(js, "", 0);
      
    ant_value_t parsed = json_parse_value(js, str);
    if (is_err(parsed)) js_reject_promise(js, promise, response_rejection_reason(js, parsed));
    else js_resolve_promise(js, promise, parsed);
    
    break;
  }
  case BODY_ARRAYBUFFER: {
    ArrayBufferData *ab = create_array_buffer_data(size);
    if (!ab) {
      js_reject_promise(js, promise, js_mkerr(js, "out of memory"));
      break;
    }
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
    if (!ab) {
      js_reject_promise(js, promise, js_mkerr(js, "out of memory"));
      break;
    }
    if (data && size > 0) memcpy(ab->data, data, size);
    js_resolve_promise(js, promise,
      create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, size, "Uint8Array"));
    break;
  }
  case BODY_FORMDATA: {
    ant_value_t fd = formdata_parse_body(js, data, size, body_type, has_body);
    if (is_err(fd)) js_reject_promise(js, promise, response_rejection_reason(js, fd));
    else js_resolve_promise(js, promise, fd);
    break;
  }}
}

static bool response_chunk_is_uint8_array(ant_value_t chunk, TypedArrayData **out_ta) {
  if (!is_object_type(chunk)) return false;
  TypedArrayData *ta = buffer_get_typedarray_data(chunk);
  if (!ta || !ta->buffer || ta->buffer->is_detached) return false;
  if (ta->type != TYPED_ARRAY_UINT8) return false;
  *out_ta = ta;
  return true;
}

static uint8_t *concat_uint8_chunks(
  ant_t *js, ant_value_t chunks,
  size_t *out_size, ant_value_t *err_out
) {
  ant_offset_t n = js_arr_len(js, chunks);
  size_t total = 0;
  size_t pos = 0;
  uint8_t *buf = NULL;
  TypedArrayData *ta = NULL;

  *out_size = 0;
  *err_out = js_mkundef();

  for (ant_offset_t i = 0; i < n; i++) {
    ant_value_t chunk = js_arr_get(js, chunks, i);
    if (!response_chunk_is_uint8_array(chunk, &ta)) {
      *err_out = js_mkerr_typed(js, JS_ERR_TYPE, "Response body stream chunk must be a Uint8Array");
      return NULL;
    }
    total += ta->byte_length;
  }

  buf = total > 0 ? malloc(total) : NULL;
  if (total > 0 && !buf) {
    *err_out = js_mkerr(js, "out of memory");
    return NULL;
  }

  for (ant_offset_t i = 0; i < n; i++) {
    ant_value_t chunk = js_arr_get(js, chunks, i);
    if (!response_chunk_is_uint8_array(chunk, &ta)) {
      free(buf);
      *err_out = js_mkerr_typed(js, JS_ERR_TYPE, "Response body stream chunk must be a Uint8Array");
      return NULL;
    }
    if (ta->byte_length == 0) continue;
    memcpy(buf + pos, ta->buffer->data + ta->byte_offset, ta->byte_length);
    pos += ta->byte_length;
  }

  *out_size = pos;
  return buf;
}

static ant_value_t stream_body_read(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t stream_body_rejected(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t promise = js_get(js, state, "promise");
  ant_value_t reason = (nargs > 0) ? args[0] : js_mkundef();
  js_reject_promise(js, promise, reason);
  return js_mkundef();
}

static void stream_schedule_next_read(ant_t *js, ant_value_t state, ant_value_t reader) {
  ant_value_t next_p = rs_default_reader_read(js, reader);
  ant_value_t fulfill = js_heavy_mkfun(js, stream_body_read, state);
  ant_value_t reject = js_heavy_mkfun(js, stream_body_rejected, state);
  ant_value_t then_result = js_promise_then(js, next_p, fulfill, reject);
  promise_mark_handled(then_result);
}

static ant_value_t stream_body_read(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t result = (nargs > 0) ? args[0] : js_mkundef();
  ant_value_t promise = js_get(js, state, "promise");
  ant_value_t reader = js_get(js, state, "reader");
  ant_value_t chunks = js_get(js, state, "chunks");
  int mode = (int)js_getnum(js_get(js, state, "mode"));

  ant_value_t done_val = js_get(js, result, "done");
  ant_value_t value = js_get(js, result, "value");

  if (vtype(done_val) == T_BOOL && done_val == js_true) {
    size_t size = 0;
    ant_value_t chunk_err = js_mkundef();
    uint8_t *data = concat_uint8_chunks(js, chunks, &size, &chunk_err);
    if (is_err(chunk_err)) {
      js_reject_promise(js, promise, response_rejection_reason(js, chunk_err));
      return js_mkundef();
    }
    ant_value_t type_v = js_get(js, state, "type");
    const char *body_type = (vtype(type_v) == T_STR) ? js_getstr(js, type_v, NULL) : NULL;
    resolve_body_promise(js, promise, data, size, body_type, mode, true);
    free(data);
    return js_mkundef();
  }

  if (vtype(value) != T_UNDEF && vtype(value) != T_NULL) js_arr_push(js, chunks, value);
  stream_schedule_next_read(js, state, reader);
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
  js_set(js, state, "reader", reader);
  js_set(js, state, "chunks", js_mkarr(js));
  js_set(js, state, "mode", js_mknum(mode));
  js_set(js, state, "type", body_type ? js_mkstr(js, body_type, strlen(body_type)) : js_mkundef());

  stream_schedule_next_read(js, state, reader);
  return promise;
}

static ant_value_t consume_body(ant_t *js, int mode) {
  ant_value_t this = js_getthis(js);
  response_data_t *d = get_data(this);
  ant_value_t promise = js_mkpromise(js);
  ant_value_t stream = 0;

  if (!d) {
    js_reject_promise(js, promise, response_rejection_reason(js,
      js_mkerr_typed(js, JS_ERR_TYPE, "Invalid Response object")));
    return promise;
  }

  if (!d->has_body) {
    resolve_body_promise(js, promise, NULL, 0, response_effective_body_type(js, this, d), mode, false);
    return promise;
  }

  stream = js_get_slot(this, SLOT_RESPONSE_BODY_STREAM);
  if (d->body_used || (rs_is_stream(stream) && rs_stream_unusable(stream))) {
    js_reject_promise(js, promise, response_rejection_reason(js,
      js_mkerr_typed(js, JS_ERR_TYPE, "body stream is disturbed or locked")));
    return promise;
  }

  d->body_used = true;
  if (rs_is_stream(stream))
    return consume_body_from_stream(js, stream, promise, mode, response_effective_body_type(js, this, d));

  resolve_body_promise(js, promise, d->body_data, d->body_size, response_effective_body_type(js, this, d), mode, true);
  return promise;
}

static ant_value_t js_res_text(ant_t *js, ant_value_t *args, int nargs) {
  return consume_body(js, BODY_TEXT);
}

static ant_value_t js_res_json(ant_t *js, ant_value_t *args, int nargs) {
  return consume_body(js, BODY_JSON);
}

static ant_value_t js_res_array_buffer(ant_t *js, ant_value_t *args, int nargs) {
  return consume_body(js, BODY_ARRAYBUFFER);
}

static ant_value_t js_res_blob(ant_t *js, ant_value_t *args, int nargs) {
  return consume_body(js, BODY_BLOB);
}

static ant_value_t js_res_bytes(ant_t *js, ant_value_t *args, int nargs) {
  return consume_body(js, BODY_BYTES);
}

static ant_value_t js_res_form_data(ant_t *js, ant_value_t *args, int nargs) {
  return consume_body(js, BODY_FORMDATA);
}

static bool is_null_body_status(int status) {
  return status == 101 || status == 103 || status == 204 || status == 205 || status == 304;
}

static bool is_redirect_status(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static bool is_ok_status(int status) {
  return status >= 200 && status <= 299;
}

static bool is_valid_reason_phrase(const char *str, size_t len) {
  utf8proc_int32_t cp = 0;
  utf8proc_ssize_t n = 0;
  size_t pos = 0;

  while (pos < len) {
    n = utf8_next((const utf8proc_uint8_t *)(str + pos), (utf8proc_ssize_t)(len - pos), &cp);
    if (cp > 0xFF) return false;
    if (cp == '\r' || cp == '\n') return false;
    pos += (size_t)n;
  }

  return true;
}

static ant_value_t response_init_status(ant_t *js, ant_value_t init, response_data_t *resp) {
  ant_value_t status_v = js_get(js, init, "status");
  double status_num = 200;

  if (vtype(status_v) != T_UNDEF) {
    status_num = (vtype(status_v) == T_NUM) ? js_getnum(status_v) : js_to_number(js, status_v);
  }

  if (status_num < 200 || status_num > 599 || status_num != (int)status_num) {
    return js_mkerr_typed(js, JS_ERR_RANGE, "Failed to construct 'Response': status must be in the range 200-599");
  }

  resp->status = (int)status_num;
  return js_mkundef();
}

static ant_value_t response_init_status_text(ant_t *js, ant_value_t init, response_data_t *resp) {
  ant_value_t status_text_v = js_get(js, init, "statusText");
  size_t len = 0;
  const char *status_text = NULL;
  char *dup = NULL;

  if (vtype(status_text_v) == T_UNDEF) return js_mkundef();
  if (vtype(status_text_v) != T_STR) {
    status_text_v = js_tostring_val(js, status_text_v);
    if (is_err(status_text_v)) return status_text_v;
  }

  status_text = js_getstr(js, status_text_v, &len);
  if (!is_valid_reason_phrase(status_text, len)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to construct 'Response': Invalid statusText");
  }

  dup = strdup(status_text);
  if (!dup) return js_mkerr(js, "out of memory");
  free(resp->status_text);
  resp->status_text = dup;
  return js_mkundef();
}

static ant_value_t response_apply_body(
  ant_t *js, ant_value_t resp_obj, ant_value_t headers, response_data_t *resp,
  ant_value_t body_val
) {
  ant_value_t body_err = js_mkundef();
  ant_value_t body_stream = js_mkundef();
  uint8_t *body_data = NULL;
  size_t body_size = 0;
  char *body_type = NULL;

  ant_value_t current_type = js_mknull();

  if (vtype(body_val) == T_NULL || vtype(body_val) == T_UNDEF) return js_mkundef();

  if (!extract_body(js, body_val, &body_data, &body_size, &body_type, &body_stream, &body_err)) {
    return is_err(body_err) ? body_err : js_mkerr(js, "Failed to extract body");
  }

  if (is_null_body_status(resp->status)) {
    free(body_data);
    free(body_type);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to construct 'Response': Response with null body status cannot have body");
  }

  free(resp->body_data);
  free(resp->body_type);
  resp->body_data = body_data;
  resp->body_size = body_size;
  resp->body_type = body_type;
  resp->body_is_stream = rs_is_stream(body_stream);
  resp->has_body = true;

  if (resp->body_is_stream) js_set_slot_wb(js, resp_obj, SLOT_RESPONSE_BODY_STREAM, body_stream);
  current_type = headers_get_value(js, headers, "content-type");
  
  if (body_type && !is_err(current_type) && vtype(current_type) == T_NULL)
    headers_append_if_missing(headers, "content-type", body_type);
  else if (!is_err(current_type))
    response_maybe_normalize_text_content_type(js, headers, current_type, body_type);

  return js_mkundef();
}

static ant_value_t response_init_common(
  ant_t *js, ant_value_t resp_obj, ant_value_t init,
  ant_value_t body_val, headers_guard_t guard
) {
  response_data_t *resp = get_data(resp_obj);
  ant_value_t headers = js_get_slot(resp_obj, SLOT_RESPONSE_HEADERS);
  ant_value_t step = js_mkundef();

  if (!resp) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid Response object");
  if (vtype(init) != T_UNDEF) {
    ant_value_t init_headers = js_get(js, init, "headers");
    step = response_init_status(js, init, resp);
    if (is_err(step)) return step;
    step = response_init_status_text(js, init, resp);
    if (is_err(step)) return step;
    if (vtype(init_headers) != T_UNDEF) {
      headers = headers_create_from_init(js, init_headers);
      if (is_err(headers)) return headers;
    }
    headers_set_guard(headers, guard);
    headers_apply_guard(headers);
    js_set_slot_wb(js, resp_obj, SLOT_RESPONSE_HEADERS, headers);
  }

  return response_apply_body(js, resp_obj, headers, resp, body_val);
}

static ant_value_t response_new(headers_guard_t guard) {
  ant_t *js = rt->js;
  response_data_t *resp = data_new();
  ant_value_t obj = 0;
  ant_value_t headers = 0;

  if (!resp) return js_mkerr(js, "out of memory");
  obj = js_mkobj(js);
  js_set_proto_init(obj, g_response_proto);
  js_set_slot(obj, SLOT_BRAND, js_mknum(BRAND_RESPONSE));
  js_set_native(obj, resp, RESPONSE_NATIVE_TAG);

  headers = headers_create_empty(js);
  if (is_err(headers)) {
    data_free(resp);
    return headers;
  }

  headers_set_guard(headers, guard);
  headers_apply_guard(headers);
  js_set_slot_wb(js, obj, SLOT_RESPONSE_HEADERS, headers);
  js_set_slot_wb(js, obj, SLOT_RESPONSE_BODY_STREAM, js_mkundef());
  return obj;
}

static ant_value_t js_response_ctor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t body = (nargs >= 1) ? args[0] : js_mknull();
  ant_value_t init = (nargs >= 2 && vtype(args[1]) != T_UNDEF) ? args[1] : js_mkundef();
  ant_value_t obj = 0;
  ant_value_t proto = 0;
  ant_value_t step = 0;

  if (vtype(js->new_target) == T_UNDEF) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Response constructor requires 'new'");
  }

  obj = response_new(HEADERS_GUARD_RESPONSE);
  if (is_err(obj)) return obj;

  proto = js_instance_proto_from_new_target(js, g_response_proto);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);

  step = response_init_common(js, obj, init, body, HEADERS_GUARD_RESPONSE);
  if (is_err(step)) {
    data_free(get_data(obj));
    return step;
  }

  return obj;
}

static ant_value_t response_create_static(
  ant_t *js, const char *type, int status, const char *status_text, headers_guard_t guard
) {
  ant_value_t obj = response_new(guard);
  response_data_t *resp = NULL;

  if (is_err(obj)) return obj;
  resp = get_data(obj);

  free(resp->type);
  resp->type = strdup(type ? type : "default");
  if (!resp->type) {
    data_free(resp);
    return js_mkerr(js, "out of memory");
  }

  resp->status = status;
  free(resp->status_text);
  resp->status_text = strdup(status_text ? status_text : "");
  if (!resp->status_text) {
    data_free(resp);
    return js_mkerr(js, "out of memory");
  }

  return obj;
}

static ant_value_t js_response_error(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t obj = response_create_static(js, "error", 0, "", HEADERS_GUARD_IMMUTABLE);
  if (is_err(obj)) return obj;
  return obj;
}

static ant_value_t js_response_redirect(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t url_v = (nargs >= 1) ? args[0] : js_mkundef();
  ant_value_t status_v = (nargs >= 2) ? args[1] : js_mknum(302);
  ant_value_t obj = 0;
  ant_value_t headers = 0;
  const char *url_str = NULL;
  int status = 302;
  url_state_t parsed = {0};
  char *href = NULL;

  if (vtype(url_v) != T_STR) {
    url_v = js_tostring_val(js, url_v);
    if (is_err(url_v)) return url_v;
  }

  status = (vtype(status_v) == T_NUM) ? (int)js_getnum(status_v) : (int)js_to_number(js, status_v);
  if (!is_redirect_status(status)) {
    return js_mkerr_typed(js, JS_ERR_RANGE, "Response.redirect status must be 301, 302, 303, 307, or 308");
  }

  url_str = js_getstr(js, url_v, NULL);
  if (parse_url_to_state(url_str, NULL, &parsed) != 0) {
    url_state_clear(&parsed);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to construct 'Response': Invalid URL");
  }

  href = build_href(&parsed);
  if (!href) {
    url_state_clear(&parsed);
    return js_mkerr(js, "out of memory");
  }

  obj = response_create_static(js, "default", status, "", HEADERS_GUARD_IMMUTABLE);
  if (is_err(obj)) {
    free(href);
    url_state_clear(&parsed);
    return obj;
  }

  headers = js_get_slot(obj, SLOT_RESPONSE_HEADERS);
  headers_set_guard(headers, HEADERS_GUARD_NONE);
  headers_append_if_missing(headers, "location", href);
  headers_set_guard(headers, HEADERS_GUARD_IMMUTABLE);
  headers_apply_guard(headers);

  free(href);
  url_state_clear(&parsed);
  return obj;
}

static ant_value_t js_response_json_static(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t init = (nargs >= 2 && vtype(args[1]) != T_UNDEF) ? args[1] : js_mkundef();
  ant_value_t stringify = 0;
  ant_value_t obj = 0;
  ant_value_t headers = 0;
  ant_value_t step = 0;
  bool init_has_content_type = false;

  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Response.json requires 1 argument");
  stringify = json_stringify_value(js, args[0]);
  if (is_err(stringify)) return stringify;
  if (vtype(stringify) == T_UNDEF) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Response.json data is not JSON serializable");
  }

  init_has_content_type =
    vtype(init) != T_UNDEF &&
    headers_init_has_name(js, js_get(js, init, "headers"), "content-type");

  obj = response_new(HEADERS_GUARD_RESPONSE);
  if (is_err(obj)) return obj;

  step = response_init_common(js, obj, init, stringify, HEADERS_GUARD_RESPONSE);
  if (is_err(step)) {
    data_free(get_data(obj));
    return step;
  }

  headers = js_get_slot(obj, SLOT_RESPONSE_HEADERS);
  if (!init_has_content_type) headers_set_literal(js, headers, "content-type", "application/json");
  return obj;
}

static ant_value_t res_body_pull(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t resp_obj = js_get_slot(js->current_func, SLOT_DATA);
  response_data_t *d = get_data(resp_obj);
  ant_value_t ctrl = (nargs > 0) ? args[0] : js_mkundef();

  if (d && d->body_data && d->body_size > 0) {
    ArrayBufferData *ab = create_array_buffer_data(d->body_size);
    if (ab) {
      memcpy(ab->data, d->body_data, d->body_size);
      rs_controller_enqueue(js, ctrl,
        create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, d->body_size, "Uint8Array"));
    }
  }

  rs_controller_close(js, ctrl);
  return js_mkundef();
}

#define RES_GETTER_START(name)                                                    \
  static ant_value_t js_res_get_##name(ant_t *js, ant_value_t *args, int nargs) { \
    ant_value_t this = js_getthis(js);                                            \
    response_data_t *d = get_data(this);                                           \
    if (!d) return js_mkundef();

#define RES_GETTER_END }

RES_GETTER_START(type)
  const char *type = d->type ? d->type : "default";
  return js_mkstr(js, type, strlen(type));
RES_GETTER_END

RES_GETTER_START(url)
  char *href = NULL;
  char *hash = NULL;
  ant_value_t ret = 0;

  if (!d->has_url) return js_mkstr(js, "", 0);
  href = build_href(&d->url);
  if (!href) return js_mkstr(js, "", 0);
  hash = strchr(href, '#');
  if (hash) *hash = '\0';
  ret = js_mkstr(js, href, strlen(href));
  free(href);
  return ret;
RES_GETTER_END

RES_GETTER_START(redirected)
  return js_bool(d->url_list_size > 1);
RES_GETTER_END

RES_GETTER_START(status)
  return js_mknum(d->status);
RES_GETTER_END

RES_GETTER_START(ok)
  return js_bool(is_ok_status(d->status));
RES_GETTER_END

RES_GETTER_START(status_text)
  const char *status_text = d->status_text ? d->status_text : "";
  return js_mkstr(js, status_text, strlen(status_text));
RES_GETTER_END

RES_GETTER_START(headers)
  return js_get_slot(this, SLOT_RESPONSE_HEADERS);
RES_GETTER_END

RES_GETTER_START(body)
  ant_value_t stored_stream = js_get_slot(this, SLOT_RESPONSE_BODY_STREAM);
  if (!d->has_body) return js_mknull();
  if (rs_is_stream(stored_stream)) return stored_stream;
  if (d->body_used) return js_mknull();
  ant_value_t pull = js_heavy_mkfun(js, res_body_pull, this);
  ant_value_t stream = rs_create_stream(js, pull, js_mkundef(), 1.0);
  if (!is_err(stream)) js_set_slot_wb(js, this, SLOT_RESPONSE_BODY_STREAM, stream);
  return stream;
RES_GETTER_END

RES_GETTER_START(body_used)
  ant_value_t stored_stream = js_get_slot(this, SLOT_RESPONSE_BODY_STREAM);
  bool used = d->body_used || (rs_is_stream(stored_stream) && rs_stream_disturbed(stored_stream));
  return js_bool(used);
RES_GETTER_END

#undef RES_GETTER_START
#undef RES_GETTER_END

static ant_value_t response_inspect_finish(ant_t *js, ant_value_t this_obj, ant_value_t body_obj) {
  ant_value_t tag_val = js_get_sym(js, this_obj, get_toStringTag_sym());
  const char *tag = vtype(tag_val) == T_STR ? js_getstr(js, tag_val, NULL) : "Response";

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
static bool response_inspect_set(
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

static ant_value_t response_inspect(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  ant_value_t out = js_mkobj(js);
  ant_value_t err = 0;

  if (!response_inspect_set(js, out, "type", js_res_get_type(js, NULL, 0), &err)) return err;
  if (!response_inspect_set(js, out, "url", js_res_get_url(js, NULL, 0), &err)) return err;
  if (!response_inspect_set(js, out, "redirected", js_res_get_redirected(js, NULL, 0), &err)) return err;
  if (!response_inspect_set(js, out, "status", js_res_get_status(js, NULL, 0), &err)) return err;
  if (!response_inspect_set(js, out, "ok", js_res_get_ok(js, NULL, 0), &err)) return err;
  if (!response_inspect_set(js, out, "statusText", js_res_get_status_text(js, NULL, 0), &err)) return err;
  if (!response_inspect_set(js, out, "headers", js_res_get_headers(js, NULL, 0), &err)) return err;

  return response_inspect_finish(js, this_obj, out);
}

static ant_value_t js_response_clone(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this = js_getthis(js);
  response_data_t *d = get_data(this);
  response_data_t *nd = NULL;
  ant_value_t src_headers = 0;
  ant_value_t new_headers = 0;
  ant_value_t obj = 0;
  ant_value_t src_stream = 0;

  if (!d) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid Response object");
  if (d->body_used) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot clone a Response whose body is unusable");
  }

  nd = data_dup(d);
  if (!nd) return js_mkerr(js, "out of memory");

  src_headers = js_get_slot(this, SLOT_RESPONSE_HEADERS);
  new_headers = headers_create_empty(js);
  if (is_err(new_headers)) {
    data_free(nd);
    return new_headers;
  }

  headers_copy_from(js, new_headers, src_headers);
  headers_set_guard(new_headers, headers_get_guard(src_headers));
  headers_apply_guard(new_headers);

  obj = js_mkobj(js);
  js_set_proto_init(obj, g_response_proto);
  js_set_slot(obj, SLOT_BRAND, js_mknum(BRAND_RESPONSE));
  js_set_native(obj, nd, RESPONSE_NATIVE_TAG);
  js_set_slot_wb(js, obj, SLOT_RESPONSE_HEADERS, new_headers);
  js_set_slot_wb(js, obj, SLOT_RESPONSE_BODY_STREAM, js_mkundef());

  src_stream = js_get_slot(this, SLOT_RESPONSE_BODY_STREAM);
  if (!rs_is_stream(src_stream)) return obj;

  ant_value_t branches = readable_stream_tee(js, src_stream);
  if (!is_err(branches) && vtype(branches) == T_ARR) {
    ant_value_t b1 = js_arr_get(js, branches, 0);
    ant_value_t b2 = js_arr_get(js, branches, 1);
    js_set_slot_wb(js, this, SLOT_RESPONSE_BODY_STREAM, b1);
    js_set_slot_wb(js, obj, SLOT_RESPONSE_BODY_STREAM, b2);
  }

  return obj;
}

ant_value_t response_create(
  ant_t *js,
  const char *type,
  int status,
  const char *status_text,
  ant_value_t headers_obj,
  const uint8_t *body,
  size_t body_len,
  const char *body_type,
  headers_guard_t guard
) {
  ant_value_t obj = response_new(guard);
  ant_value_t headers = 0;
  response_data_t *resp = NULL;

  if (is_err(obj)) return obj;
  resp = get_data(obj);

  free(resp->type);
  resp->type = strdup(type ? type : "default");
  if (!resp->type) {
    data_free(resp);
    return js_mkerr(js, "out of memory");
  }

  resp->status = status;
  free(resp->status_text);
  resp->status_text = strdup(status_text ? status_text : "");
  if (!resp->status_text) {
    data_free(resp);
    return js_mkerr(js, "out of memory");
  }

  if (body_len > 0) {
    resp->body_data = malloc(body_len);
    if (!resp->body_data) {
      data_free(resp);
      return js_mkerr(js, "out of memory");
    }
    memcpy(resp->body_data, body, body_len);
  }

  resp->body_size = body_len;
  resp->body_type = body_type ? strdup(body_type) : NULL;
  resp->has_body = body || body_len > 0;
  resp->body_is_stream = false;

  headers = is_object_type(headers_obj) ? headers_obj : headers_create_empty(js);
  if (is_err(headers)) {
    data_free(resp);
    return headers;
  }

  headers_set_guard(headers, guard);
  headers_apply_guard(headers);
  
  ant_value_t current_type = headers_get_value(js, headers, "content-type");
  if (body_type && !is_err(current_type) && vtype(current_type) == T_NULL) {
    headers_append_if_missing(headers, "content-type", body_type);
  } else if (!is_err(current_type)) response_maybe_normalize_text_content_type(
    js, headers, current_type, body_type
  );
  js_set_slot_wb(js, obj, SLOT_RESPONSE_HEADERS, headers);
  return obj;
}

ant_value_t response_create_fetched(
  ant_t *js,
  int status,
  const char *status_text,
  const char *url,
  int url_list_size,
  ant_value_t headers_obj,
  const uint8_t *body,
  size_t body_len,
  ant_value_t body_stream,
  const char *body_type
) {
  ant_value_t obj = response_new(HEADERS_GUARD_IMMUTABLE);
  ant_value_t headers = 0;
  response_data_t *resp = NULL;
  url_state_t parsed = {0};

  if (is_err(obj)) return obj;
  resp = get_data(obj);

  resp->status = status;
  free(resp->status_text);
  resp->status_text = strdup(status_text ? status_text : "");
  if (!resp->status_text) {
    data_free(resp);
    return js_mkerr(js, "out of memory");
  }

  if (url && parse_url_to_state(url, NULL, &parsed) == 0) {
    url_state_clear(&resp->url);
    resp->url = parsed;
    resp->has_url = true;
    resp->url_list_size = url_list_size > 0 ? url_list_size : 1;
  } else url_state_clear(&parsed);

  if (rs_is_stream(body_stream)) {
    resp->body_is_stream = true;
    resp->has_body = true;
    js_set_slot_wb(js, obj, SLOT_RESPONSE_BODY_STREAM, body_stream);
  } else {
    if (body_len > 0) {
      resp->body_data = malloc(body_len);
      if (!resp->body_data) {
        data_free(resp);
        return js_mkerr(js, "out of memory");
      }
      memcpy(resp->body_data, body, body_len);
    }
    resp->body_size = body_len;
    resp->body_is_stream = false;
    resp->has_body = body || body_len > 0;
  }

  resp->body_type = body_type ? strdup(body_type) : NULL;
  if (body_type && !resp->body_type) {
    data_free(resp);
    return js_mkerr(js, "out of memory");
  }

  headers = is_object_type(headers_obj) ? headers_obj : headers_create_empty(js);
  if (is_err(headers)) {
    data_free(resp);
    return headers;
  }

  headers_set_guard(headers, HEADERS_GUARD_IMMUTABLE);
  headers_apply_guard(headers);
  js_set_slot_wb(js, obj, SLOT_RESPONSE_HEADERS, headers);
  
  return obj;
}

void init_response_module(void) {
  ant_t *js = rt->js;
  ant_value_t g = js_glob(js);
  ant_value_t ctor = 0;

  g_response_proto = js_mkobj(js);

  js_set(js, g_response_proto, "text", js_mkfun(js_res_text));
  js_set(js, g_response_proto, "json", js_mkfun(js_res_json));
  js_set(js, g_response_proto, "arrayBuffer", js_mkfun(js_res_array_buffer));
  js_set(js, g_response_proto, "blob", js_mkfun(js_res_blob));
  js_set(js, g_response_proto, "formData", js_mkfun(js_res_form_data));
  js_set(js, g_response_proto, "bytes", js_mkfun(js_res_bytes));
  js_set(js, g_response_proto, "clone", js_mkfun(js_response_clone));

#define GETTER(prop, fn) \
  js_set_getter_desc(js, g_response_proto, prop, sizeof(prop) - 1, js_mkfun(js_res_get_##fn), JS_DESC_C)
  GETTER("type", type);
  GETTER("url", url);
  GETTER("redirected", redirected);
  GETTER("status", status);
  GETTER("ok", ok);
  GETTER("statusText", status_text);
  GETTER("headers", headers);
  GETTER("body", body);
  GETTER("bodyUsed", body_used);
#undef GETTER

  js_set_sym(js, g_response_proto, get_inspect_sym(), js_mkfun(response_inspect));
  js_set_sym(js, g_response_proto, get_toStringTag_sym(), js_mkstr(js, "Response", 8));
  ctor = js_make_ctor(js, js_response_ctor, g_response_proto, "Response", 8);
  js_set(js, ctor, "error", js_mkfun(js_response_error));
  js_set(js, ctor, "redirect", js_mkfun(js_response_redirect));
  js_set(js, ctor, "json", js_mkfun(js_response_json_static));
  js_set(js, g, "Response", ctor);
  js_set_descriptor(js, g, "Response", 8, JS_DESC_W | JS_DESC_C);
}
