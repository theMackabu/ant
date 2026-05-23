#include <compat.h> // IWYU pragma: keep

#include "sandbox/sandbox.h"
#include "sandbox/transport.h"

#include "ant.h"
#include "errors.h"
#include "gc/roots.h"
#include "internal.h"
#include "reactor.h"
#include "silver/engine.h"
#include "utils.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ant_sandbox_request_free(ant_sandbox_request_t *req) {
  if (!req) return;

  free(req->cwd);
  free(req->entry);
  free(req->source);

  for (int i = 0; i < req->argc; i++) free(req->argv[i]);
  free(req->argv);
  free(req->forward_ports);

  memset(req, 0, sizeof(*req));
}

static char *sandbox_dup_bytes(const uint8_t *data, size_t len) {
  char *out = try_oom(len + 1);

  memcpy(out, data, len);
  out[len] = '\0';

  return out;
}

static void sandbox_store16(uint8_t *out, uint16_t value) {
  out[0] = (uint8_t)value;
  out[1] = (uint8_t)(value >> 8);
}

static void sandbox_store32(uint8_t *out, uint32_t value) {
  out[0] = (uint8_t)value;
  out[1] = (uint8_t)(value >> 8);
  out[2] = (uint8_t)(value >> 16);
  out[3] = (uint8_t)(value >> 24);
}

static uint8_t *sandbox_write_u16(uint8_t *out, uint16_t value) {
  sandbox_store16(out, value);
  return out + 2;
}

static uint8_t *sandbox_write_u32(uint8_t *out, uint32_t value) {
  sandbox_store32(out, value);
  return out + 4;
}

static uint16_t sandbox_load16(const uint8_t *in) {
  return (uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
}

static uint32_t sandbox_load32(const uint8_t *in) {
  return (uint32_t)in[0] |
         ((uint32_t)in[1] << 8) |
         ((uint32_t)in[2] << 16) |
         ((uint32_t)in[3] << 24);
}

static bool sandbox_frame_add_size(size_t *total, size_t add) {
  if (add > SIZE_MAX - *total) return false;
  *total += add;
  return *total <= ANT_SANDBOX_FRAME_MAX_SIZE;
}

static bool sandbox_string_payload_size(size_t *total, const char *str) {
  size_t len = strlen(str);
  if (len > UINT32_MAX) return false;
  return sandbox_frame_add_size(total, 4 + len);
}

static uint8_t *sandbox_write_string(uint8_t *out, const char *str) {
  size_t len = strlen(str);
  sandbox_store32(out, (uint32_t)len);
  out += 4;
  memcpy(out, str, len);
  return out + len;
}

static uint8_t *sandbox_write_u64(uint8_t *out, uint64_t value) {
  for (int i = 0; i < 8; i++) out[i] = (uint8_t)(value >> (i * 8));
  return out + 8;
}

static uint64_t sandbox_load64(const uint8_t *in) {
  uint64_t value = 0;
  for (int i = 0; i < 8; i++) value |= ((uint64_t)in[i]) << (i * 8);
  return value;
}

static bool sandbox_payload_string_size(size_t *total, const char *str, size_t len) {
  if (len > UINT32_MAX) return false;
  (void)str;
  return sandbox_frame_add_size(total, 4 + len);
}

static uint8_t *sandbox_write_bytes_string(uint8_t *out, const char *str, size_t len) {
  sandbox_store32(out, (uint32_t)len);
  out += 4;
  if (len > 0) memcpy(out, str, len);
  return out + len;
}

static uint8_t *sandbox_alloc_frame(
  ant_sandbox_frame_type_t type,
  size_t payload_len,
  size_t *len_out
) {
  if (!len_out || payload_len > UINT32_MAX) return NULL;

  size_t frame_len = ANT_SANDBOX_FRAME_HEADER_SIZE + payload_len;
  if (frame_len > ANT_SANDBOX_FRAME_MAX_SIZE) return NULL;

  uint8_t *frame = try_oom(frame_len);
  memcpy(frame, ANT_SANDBOX_FRAME_MAGIC, 4);
  frame[4] = ANT_SANDBOX_FRAME_VERSION;
  frame[5] = (uint8_t)type;
  sandbox_store16(frame + 6, 0);
  sandbox_store32(frame + 8, (uint32_t)payload_len);

  *len_out = frame_len;
  return frame;
}

uint8_t *ant_sandbox_build_frame(
  ant_sandbox_frame_type_t type,
  const void *payload,
  size_t payload_len,
  size_t *len_out
) {
  if (payload_len > 0 && !payload) return NULL;

  uint8_t *frame = sandbox_alloc_frame(type, payload_len, len_out);
  if (!frame) return NULL;
  if (payload_len > 0) memcpy(frame + ANT_SANDBOX_FRAME_HEADER_SIZE, payload, payload_len);
  return frame;
}

uint8_t *ant_sandbox_build_run_request_frame(
  const char *cwd,
  const char *entry,
  int argc,
  char **argv,
  uint32_t capabilities,
  uint16_t tty_rows,
  uint16_t tty_cols,
  const uint16_t *forward_ports,
  uint32_t forward_count,
  size_t *len_out
) {
  if (!cwd || !entry || argc < 0 || !len_out) return NULL;
  if (forward_count > 0 && !forward_ports) return NULL;

  size_t payload_len = 0;
  if (!sandbox_frame_add_size(&payload_len, 8)) return NULL;
  if (!sandbox_string_payload_size(&payload_len, cwd)) return NULL;
  if (!sandbox_string_payload_size(&payload_len, entry)) return NULL;
  if (!sandbox_frame_add_size(&payload_len, 4)) return NULL;

  for (int i = 0; i < argc; i++) {
    if (!argv[i] || !sandbox_string_payload_size(&payload_len, argv[i])) return NULL;
  }
  if (forward_count > 0 &&
      !sandbox_frame_add_size(&payload_len, 4 + (size_t)forward_count * 2u)) {
    return NULL;
  }

  size_t frame_len = ANT_SANDBOX_FRAME_HEADER_SIZE + payload_len;
  if (payload_len > UINT32_MAX || frame_len > ANT_SANDBOX_FRAME_MAX_SIZE) return NULL;

  uint8_t *frame = sandbox_alloc_frame(ANT_SANDBOX_FRAME_RUN, payload_len, len_out);
  if (!frame) return NULL;

  uint8_t *p = frame + ANT_SANDBOX_FRAME_HEADER_SIZE;
  p = sandbox_write_u32(p, capabilities);
  p = sandbox_write_u16(p, tty_rows);
  p = sandbox_write_u16(p, tty_cols);
  p = sandbox_write_string(p, cwd);
  p = sandbox_write_string(p, entry);
  sandbox_store32(p, (uint32_t)argc);
  p += 4;
  for (int i = 0; i < argc; i++) p = sandbox_write_string(p, argv[i]);
  if (forward_count > 0) {
    p = sandbox_write_u32(p, forward_count);
    for (uint32_t i = 0; i < forward_count; i++) p = sandbox_write_u16(p, forward_ports[i]);
  }

  return frame;
}

uint8_t *ant_sandbox_build_eval_request_frame(
  const char *cwd,
  const char *source,
  uint32_t capabilities,
  uint16_t tty_rows,
  uint16_t tty_cols,
  size_t *len_out
) {
  if (!cwd || !source || !len_out) return NULL;

  size_t payload_len = 0;
  if (!sandbox_frame_add_size(&payload_len, 8)) return NULL;
  if (!sandbox_string_payload_size(&payload_len, cwd)) return NULL;
  if (!sandbox_string_payload_size(&payload_len, source)) return NULL;

  size_t frame_len = ANT_SANDBOX_FRAME_HEADER_SIZE + payload_len;
  if (payload_len > UINT32_MAX || frame_len > ANT_SANDBOX_FRAME_MAX_SIZE) return NULL;

  uint8_t *frame = sandbox_alloc_frame(ANT_SANDBOX_FRAME_EVAL, payload_len, len_out);
  if (!frame) return NULL;

  uint8_t *p = frame + ANT_SANDBOX_FRAME_HEADER_SIZE;
  p = sandbox_write_u32(p, capabilities);
  p = sandbox_write_u16(p, tty_rows);
  p = sandbox_write_u16(p, tty_cols);
  p = sandbox_write_string(p, cwd);
  p = sandbox_write_string(p, source);

  return frame;
}

typedef struct {
  const uint8_t *p;
  const uint8_t *end;
} sandbox_frame_reader_t;

static bool sandbox_read_u32(sandbox_frame_reader_t *r, uint32_t *out) {
  if ((size_t)(r->end - r->p) < 4) return false;
  *out = sandbox_load32(r->p);
  r->p += 4;
  return true;
}

static bool sandbox_read_u16(sandbox_frame_reader_t *r, uint16_t *out) {
  if ((size_t)(r->end - r->p) < 2) return false;
  *out = sandbox_load16(r->p);
  r->p += 2;
  return true;
}

static bool sandbox_read_string(sandbox_frame_reader_t *r, char **out) {
  uint32_t len;
  if (!sandbox_read_u32(r, &len)) return false;
  if ((size_t)(r->end - r->p) < len) return false;
  *out = sandbox_dup_bytes(r->p, len);
  r->p += len;
  return true;
}

static bool sandbox_read_string_view(sandbox_frame_reader_t *r, const char **out, size_t *len_out) {
  uint32_t len;
  if (!sandbox_read_u32(r, &len)) return false;
  if ((size_t)(r->end - r->p) < len) return false;
  *out = (const char *)r->p;
  *len_out = len;
  r->p += len;
  return true;
}

static bool sandbox_read_u64(sandbox_frame_reader_t *r, uint64_t *out) {
  if ((size_t)(r->end - r->p) < 8) return false;
  *out = sandbox_load64(r->p);
  r->p += 8;
  return true;
}

static bool sandbox_parse_run_payload(const uint8_t *payload, size_t payload_len, ant_sandbox_request_t *out) {
  sandbox_frame_reader_t r = { payload, payload + payload_len };
  uint32_t argc;

  out->mode = ANT_SANDBOX_REQUEST_RUN;
  if (!sandbox_read_u32(&r, &out->capabilities)) return false;
  if (!sandbox_read_u16(&r, &out->tty_rows)) return false;
  if (!sandbox_read_u16(&r, &out->tty_cols)) return false;
  if (!sandbox_read_string(&r, &out->cwd)) return false;
  if (!sandbox_read_string(&r, &out->entry)) return false;
  if (!sandbox_read_u32(&r, &argc)) return false;
  if (argc > (uint32_t)INT_MAX) return false;

  out->argv = try_oom(sizeof(*out->argv) * ((size_t)argc + 1));
  memset(out->argv, 0, sizeof(*out->argv) * ((size_t)argc + 1));
  out->argc = 0;

  for (uint32_t i = 0; i < argc; i++) {
    if (!sandbox_read_string(&r, &out->argv[i])) return false;
    out->argc = (int)i + 1;
  }

  out->argv[argc] = NULL;
  if (r.p < r.end) {
    uint32_t forward_count = 0;
    if (!sandbox_read_u32(&r, &forward_count)) return false;
    if ((size_t)(r.end - r.p) < (size_t)forward_count * 2u) return false;
    out->forward_count = forward_count;
    if (forward_count > 0) {
      out->forward_ports = try_oom(sizeof(*out->forward_ports) * (size_t)forward_count);
      for (uint32_t i = 0; i < forward_count; i++) {
        if (!sandbox_read_u16(&r, &out->forward_ports[i])) return false;
      }
    }
  }
  return r.p == r.end;
}

static bool sandbox_parse_eval_payload(const uint8_t *payload, size_t payload_len, ant_sandbox_request_t *out) {
  sandbox_frame_reader_t r = { payload, payload + payload_len };

  out->mode = ANT_SANDBOX_REQUEST_EVAL;
  if (!sandbox_read_u32(&r, &out->capabilities)) return false;
  if (!sandbox_read_u16(&r, &out->tty_rows)) return false;
  if (!sandbox_read_u16(&r, &out->tty_cols)) return false;
  if (!sandbox_read_string(&r, &out->cwd)) return false;
  if (!sandbox_read_string(&r, &out->source)) return false;
  return r.p == r.end;
}

bool ant_sandbox_parse_request_frame(const uint8_t *frame, size_t frame_len, ant_sandbox_request_t *out) {
  if (!frame || !out || frame_len < ANT_SANDBOX_FRAME_HEADER_SIZE) {
    fprintf(stderr, "sandbox daemon: invalid request frame\n");
    return false;
  }

  if (memcmp(frame, ANT_SANDBOX_FRAME_MAGIC, 4) != 0) {
    fprintf(stderr, "sandbox daemon: bad request frame magic\n");
    return false;
  }

  if (frame[4] != ANT_SANDBOX_FRAME_VERSION) {
    fprintf(stderr, "sandbox daemon: unsupported request frame version %u\n", frame[4]);
    return false;
  }

  if (sandbox_load16(frame + 6) != 0) {
    fprintf(stderr, "sandbox daemon: unsupported request frame flags\n");
    return false;
  }

  uint32_t payload_len = sandbox_load32(frame + 8);
  if ((size_t)payload_len != frame_len - ANT_SANDBOX_FRAME_HEADER_SIZE) {
    fprintf(stderr, "sandbox daemon: malformed request frame length\n");
    return false;
  }

  const uint8_t *payload = frame + ANT_SANDBOX_FRAME_HEADER_SIZE;
  bool ok;
  switch (frame[5]) {
    case ANT_SANDBOX_FRAME_RUN:
      ok = sandbox_parse_run_payload(payload, payload_len, out);
      break;
    case ANT_SANDBOX_FRAME_EVAL:
      ok = sandbox_parse_eval_payload(payload, payload_len, out);
      break;
    default:
      fprintf(stderr, "sandbox daemon: unsupported request frame type %u\n", frame[5]);
      return false;
  }

  if (!ok) {
    fprintf(stderr, "sandbox daemon: malformed request frame payload\n");
    ant_sandbox_request_free(out);
  }
  return ok;
}

uint8_t *ant_sandbox_build_result_payload(ant_t *js, ant_value_t value, size_t *len_out) {
  if (!js || !len_out) return NULL;

  ant_sandbox_value_type_t type = ANT_SANDBOX_VALUE_DISPLAY;
  const char *value_str = NULL;
  size_t value_len = 0;
  uint8_t bool_value = 0;
  uint64_t number_bits = 0;

  switch (vtype(value)) {
    case T_UNDEF:
      type = ANT_SANDBOX_VALUE_UNDEFINED;
      break;
    case T_NULL:
      type = ANT_SANDBOX_VALUE_NULL;
      break;
    case T_BOOL:
      type = ANT_SANDBOX_VALUE_BOOL;
      bool_value = value == js_true ? 1u : 0u;
      break;
    case T_NUM: {
      type = ANT_SANDBOX_VALUE_NUMBER;
      double number = js_getnum(value);
      memcpy(&number_bits, &number, sizeof(number_bits));
      break;
    }
    case T_STR:
      type = ANT_SANDBOX_VALUE_STRING;
      value_str = js_getstr(js, value, &value_len);
      break;
    default:
      type = ANT_SANDBOX_VALUE_DISPLAY;
      break;
  }

  char cbuf_stack[512]; js_cstr_t cstr = js_to_cstr(
    js, value, cbuf_stack, sizeof(cbuf_stack)
  );
  const char *display = cstr.ptr ? cstr.ptr : "";
  size_t display_len = strcmp(display, "undefined") == 0 ? 0 : strlen(display);

  size_t payload_len = 1;
  if (type == ANT_SANDBOX_VALUE_BOOL) {
    if (!sandbox_frame_add_size(&payload_len, 1)) goto fail;
  } else if (type == ANT_SANDBOX_VALUE_NUMBER) {
    if (!sandbox_frame_add_size(&payload_len, 8)) goto fail;
  } else if (type == ANT_SANDBOX_VALUE_STRING) {
    if (!sandbox_payload_string_size(&payload_len, value_str, value_len)) goto fail;
  }
  if (!sandbox_payload_string_size(&payload_len, display, display_len)) goto fail;

  uint8_t *payload = try_oom(payload_len);
  uint8_t *p = payload;
  *p++ = (uint8_t)type;
  if (type == ANT_SANDBOX_VALUE_BOOL) {
    *p++ = bool_value;
  } else if (type == ANT_SANDBOX_VALUE_NUMBER) {
    p = sandbox_write_u64(p, number_bits);
  } else if (type == ANT_SANDBOX_VALUE_STRING) {
    p = sandbox_write_bytes_string(p, value_str, value_len);
  }
  p = sandbox_write_bytes_string(p, display, display_len);
  *len_out = payload_len;
  if (cstr.needs_free) free((void *)cstr.ptr);
  return payload;

fail:
  if (cstr.needs_free) free((void *)cstr.ptr);
  return NULL;
}

uint8_t *ant_sandbox_build_error_payload(
  ant_t *js,
  ant_value_t value,
  ant_value_t fallback_stack,
  size_t *len_out
) {
  if (!js || !len_out) return NULL;

  ant_value_t obj = is_err(value) ? js_as_obj(value) : value;
  const char *name = "Error";
  const char *message = "";
  const char *stack = "";
  if (vtype(obj) == T_OBJ) {
    const char *n = get_str_prop(js, obj, "name", 4, NULL);
    const char *m = get_str_prop(js, obj, "message", 7, NULL);
    const char *s = get_str_prop(js, obj, "stack", 5, NULL);
    if (n && *n) name = n;
    if (m) message = m;
    if (s) stack = s;
  } else {
    message = js_str(js, value);
  }

  if (!*stack && vtype(fallback_stack) == T_STR) stack = js_getstr(js, fallback_stack, NULL);

  char cbuf_stack[512]; js_cstr_t cstr = js_to_cstr(
    js, value, cbuf_stack, sizeof(cbuf_stack)
  );
  const char *display = cstr.ptr ? cstr.ptr : "";
  size_t name_len = strlen(name);
  size_t message_len = strlen(message);
  size_t stack_len = strlen(stack);
  size_t display_len = strlen(display);

  size_t payload_len = 0;
  if (!sandbox_payload_string_size(&payload_len, name, name_len)) goto fail;
  if (!sandbox_payload_string_size(&payload_len, message, message_len)) goto fail;
  if (!sandbox_payload_string_size(&payload_len, stack, stack_len)) goto fail;
  if (!sandbox_payload_string_size(&payload_len, display, display_len)) goto fail;

  uint8_t *payload = try_oom(payload_len);
  uint8_t *p = payload;
  p = sandbox_write_bytes_string(p, name, name_len);
  p = sandbox_write_bytes_string(p, message, message_len);
  p = sandbox_write_bytes_string(p, stack, stack_len);
  p = sandbox_write_bytes_string(p, display, display_len);
  *len_out = payload_len;
  if (cstr.needs_free) free((void *)cstr.ptr);
  return payload;

fail:
  if (cstr.needs_free) free((void *)cstr.ptr);
  return NULL;
}

bool ant_sandbox_decode_result_value(
  ant_t *js,
  const void *payload,
  size_t payload_len,
  ant_value_t *out
) {
  if (!js || !payload || !out || payload_len < 1) return false;

  sandbox_frame_reader_t r = { payload, (const uint8_t *)payload + payload_len };
  uint8_t type = *r.p++;
  switch (type) {
    case ANT_SANDBOX_VALUE_UNDEFINED:
      *out = js_mkundef();
      break;
    case ANT_SANDBOX_VALUE_NULL:
      *out = js_mknull();
      break;
    case ANT_SANDBOX_VALUE_BOOL:
      if (r.p >= r.end) return false;
      *out = *r.p++ ? js_true : js_false;
      break;
    case ANT_SANDBOX_VALUE_NUMBER: {
      uint64_t bits = 0;
      if (!sandbox_read_u64(&r, &bits)) return false;
      double number = 0;
      memcpy(&number, &bits, sizeof(number));
      *out = js_mknum(number);
      break;
    }
    case ANT_SANDBOX_VALUE_STRING: {
      const char *str = NULL;
      size_t len = 0;
      if (!sandbox_read_string_view(&r, &str, &len)) return false;
      *out = js_mkstr(js, str, len);
      break;
    }
    case ANT_SANDBOX_VALUE_DISPLAY: {
      const char *display = NULL;
      size_t len = 0;
      if (!sandbox_read_string_view(&r, &display, &len)) return false;
      *out = js_mkstr(js, display, len);
      return r.p == r.end;
    }
    default:
      return false;
  }

  const char *display = NULL;
  size_t display_len = 0;
  if (!sandbox_read_string_view(&r, &display, &display_len)) return false;
  return r.p == r.end;
}

ant_value_t ant_sandbox_decode_error_value(ant_t *js, const void *payload, size_t payload_len) {
  if (!js || !payload) return js_mkerr_typed(js, JS_ERR_TYPE, "malformed sandbox error frame");

  sandbox_frame_reader_t r = { payload, (const uint8_t *)payload + payload_len };
  const char *name = NULL, *message = NULL, *stack = NULL, *display = NULL;
  size_t name_len = 0, message_len = 0, stack_len = 0, display_len = 0;
  if (!sandbox_read_string_view(&r, &name, &name_len) ||
      !sandbox_read_string_view(&r, &message, &message_len) ||
      !sandbox_read_string_view(&r, &stack, &stack_len) ||
      !sandbox_read_string_view(&r, &display, &display_len) ||
      r.p != r.end) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "malformed sandbox error frame");
  }

  ant_value_t err = js_mkerr_typed(js, JS_ERR_GENERIC, "%.*s", (int)message_len, message);
  ant_value_t obj = is_err(err) ? js_as_obj(err) : err;
  if (is_object_type(obj)) {
    if (name_len > 0) js_set(js, obj, "name", js_mkstr(js, name, name_len));
    if (stack_len > 0) js_set(js, obj, "stack", js_mkstr(js, stack, stack_len));
  }
  return err;
}

bool ant_sandbox_result_payload_display(const void *payload, size_t payload_len, const char **out, size_t *out_len) {
  if (!payload || !out || !out_len || payload_len < 1) return false;
  sandbox_frame_reader_t r = { payload, (const uint8_t *)payload + payload_len };
  uint8_t type = *r.p++;
  if (type == ANT_SANDBOX_VALUE_BOOL) {
    if (r.p >= r.end) return false;
    r.p++;
  } else if (type == ANT_SANDBOX_VALUE_NUMBER) {
    uint64_t bits;
    if (!sandbox_read_u64(&r, &bits)) return false;
  } else if (type == ANT_SANDBOX_VALUE_STRING) {
    const char *str;
    size_t len;
    if (!sandbox_read_string_view(&r, &str, &len)) return false;
  } else if (type != ANT_SANDBOX_VALUE_UNDEFINED &&
             type != ANT_SANDBOX_VALUE_NULL &&
             type != ANT_SANDBOX_VALUE_DISPLAY) {
    return false;
  }
  return sandbox_read_string_view(&r, out, out_len) && r.p == r.end;
}

bool ant_sandbox_error_payload_display(const void *payload, size_t payload_len, const char **out, size_t *out_len) {
  if (!payload || !out || !out_len) return false;
  sandbox_frame_reader_t r = { payload, (const uint8_t *)payload + payload_len };
  const char *unused = NULL;
  size_t unused_len = 0;
  if (!sandbox_read_string_view(&r, &unused, &unused_len)) return false;
  if (!sandbox_read_string_view(&r, &unused, &unused_len)) return false;
  if (!sandbox_read_string_view(&r, &unused, &unused_len)) return false;
  return sandbox_read_string_view(&r, out, out_len) && r.p == r.end;
}

static bool sandbox_send_result_frame(ant_t *js, ant_value_t value) {
  size_t payload_len = 0;
  uint8_t *payload = ant_sandbox_build_result_payload(js, value, &payload_len);
  if (!payload) return false;
  bool ok = ant_sandbox_transport_send_frame(ANT_SANDBOX_FRAME_RESULT, payload, payload_len);
  free(payload);
  return ok;
}

static bool sandbox_send_error_frame(ant_t *js, ant_value_t value, ant_value_t fallback_stack) {
  size_t payload_len = 0;
  uint8_t *payload = ant_sandbox_build_error_payload(js, value, fallback_stack, &payload_len);
  if (!payload) return false;
  bool ok = ant_sandbox_transport_send_frame(ANT_SANDBOX_FRAME_ERROR, payload, payload_len);
  free(payload);
  return ok;
}

static bool sandbox_send_uncaught_throw(ant_t *js) {
  if (!js->thrown_exists) return false;
  sandbox_send_error_frame(js, js->thrown_value, js->thrown_stack);
  js->thrown_exists = false;
  js->thrown_value = js_mkundef();
  js->thrown_stack = js_mkundef();
  return true;
}


int ant_sandbox_eval_module(ant_t *js, const char *script, size_t len) {
  const char *tag = "[sandbox eval].mjs";
  ant_value_t ns = js_mkobj(js);

  js_set_slot(ns, SLOT_BRAND, js_mknum(BRAND_MODULE_NAMESPACE));
  js_set_slot(ns, SLOT_MODULE_LOADING, js_true);
  js_set_filename(js, tag);

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, ns);

  ant_value_t result = js_esm_eval_module_source(js, tag, script, len, ns);
  GC_ROOT_PIN(js, result);
  js_run_event_loop(js);

  int status;
  if (sandbox_send_uncaught_throw(js)) status = EXIT_FAILURE;
  else if (is_err(result)) {
    sandbox_send_error_frame(js, result, js_mkundef());
    status = EXIT_FAILURE;
  }
  else {
    ant_value_t default_export = js_get_slot(ns, SLOT_DEFAULT);
    sandbox_send_result_frame(js, default_export);
    status = EXIT_SUCCESS;
  }

  GC_ROOT_RESTORE(js, root_mark);
  return status;
}
