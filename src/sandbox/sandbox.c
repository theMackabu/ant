#include <compat.h> // IWYU pragma: keep

#include "sandbox/sandbox.h"

#include "ant.h"
#include "errors.h"
#include "gc/roots.h"
#include "internal.h"
#include "modules/io.h"
#include "reactor.h"
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
  size_t *len_out
) {
  if (!cwd || !entry || argc < 0 || !len_out) return NULL;

  size_t payload_len = 0;
  if (!sandbox_frame_add_size(&payload_len, 8)) return NULL;
  if (!sandbox_string_payload_size(&payload_len, cwd)) return NULL;
  if (!sandbox_string_payload_size(&payload_len, entry)) return NULL;
  if (!sandbox_frame_add_size(&payload_len, 4)) return NULL;

  for (int i = 0; i < argc; i++) {
    if (!argv[i] || !sandbox_string_payload_size(&payload_len, argv[i])) return NULL;
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

static int sandbox_print_eval_result(ant_t *js, ant_value_t result, bool should_print) {
  if (print_uncaught_throw(js)) return EXIT_FAILURE;

  char cbuf_stack[512]; js_cstr_t cstr = js_to_cstr(
    js, result, cbuf_stack, sizeof(cbuf_stack)
  );

  int status = EXIT_SUCCESS;
  if (vtype(result) == T_ERR) {
    fprintf(stderr, "%s\n", cstr.ptr);
    status = EXIT_FAILURE;
  } else if (should_print) {
    if (vtype(result) == T_STR) printf("%s\n", cstr.ptr ? cstr.ptr : "");
    else if (cstr.ptr && strcmp(cstr.ptr, "undefined") != 0) {
      print_value_colored(cstr.ptr, stdout); printf("\n");
    }
  }

  if (cstr.needs_free) free((void *)cstr.ptr);
  return status;
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
  if (is_err(result)) status = sandbox_print_eval_result(js, result, true);
  else {
    ant_value_t default_export = js_get_slot(ns, SLOT_DEFAULT);
    status = sandbox_print_eval_result(js, default_export, vtype(default_export) != T_UNDEF);
  }

  GC_ROOT_RESTORE(js, root_mark);
  return status;
}
