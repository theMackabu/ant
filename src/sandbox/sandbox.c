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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yyjson.h>

void ant_sandbox_request_free(ant_sandbox_request_t *req) {
  if (!req) return;

  free(req->cwd);
  free(req->entry);
  free(req->source);

  for (int i = 0; i < req->argc; i++) free(req->argv[i]);
  free(req->argv);

  memset(req, 0, sizeof(*req));
}

static char *sandbox_dup_yyjson_str(yyjson_val *val) {
  if (!yyjson_is_str(val)) return NULL;

  size_t len = yyjson_get_len(val);
  char *out = try_oom(len + 1);

  memcpy(out, yyjson_get_str(val), len);
  out[len] = '\0';

  return out;
}

static bool sandbox_parse_argv(yyjson_val *arr, ant_sandbox_request_t *out) {
  if (!arr) return true;
  if (!yyjson_is_arr(arr)) {
    fprintf(stderr, "sandbox daemon: argv must be an array of strings\n");
    return false;
  }

  size_t count = yyjson_arr_size(arr);
  if (count > (size_t)INT_MAX) {
    fprintf(stderr, "sandbox daemon: argv has too many entries\n");
    return false;
  }

  out->argv = try_oom(sizeof(*out->argv) * (count + 1));
  memset(out->argv, 0, sizeof(*out->argv) * (count + 1));
  out->argc = 0;

  yyjson_val *item;
  yyjson_arr_iter iter;
  size_t index = 0;

  yyjson_arr_iter_init(arr, &iter);
  while ((item = yyjson_arr_iter_next(&iter))) {
    if (!yyjson_is_str(item)) {
      fprintf(stderr, "sandbox daemon: argv[%zu] must be a string\n", index);
      return false;
    }

    out->argv[index++] = sandbox_dup_yyjson_str(item);
    out->argc = (int)index;
  }

  out->argv[index] = NULL;
  return true;
}

bool ant_sandbox_parse_request_json(const char *json, size_t json_len, ant_sandbox_request_t *out) {
  yyjson_read_err err;
  yyjson_doc *doc = yyjson_read_opts((char *)json, json_len, 0, NULL, &err);

  if (!doc) {
    fprintf(stderr, "sandbox daemon: failed to parse request JSON: %s\n", err.msg);
    return false;
  }
  
  bool ok = false;
  yyjson_val *root = yyjson_doc_get_root(doc);

  if (!yyjson_is_obj(root)) {
    fprintf(stderr, "sandbox daemon: request must be a JSON object\n");
    goto done;
  }

  yyjson_val *mode = yyjson_obj_get(root, "mode");
  if (!yyjson_is_str(mode)) {
    fprintf(stderr, "sandbox daemon: request.mode must be a string\n");
    goto done;
  }

  const char *mode_str = yyjson_get_str(mode);
  if (strcmp(mode_str, "run") == 0) {
    yyjson_val *entry = yyjson_obj_get(root, "entry");
    if (!yyjson_is_str(entry)) {
      fprintf(stderr, "sandbox daemon: run request requires string entry\n");
      goto done;
    }

    out->mode = ANT_SANDBOX_REQUEST_RUN;
    out->entry = sandbox_dup_yyjson_str(entry);
  } else if (strcmp(mode_str, "eval") == 0) {
    yyjson_val *source = yyjson_obj_get(root, "source");
    if (!yyjson_is_str(source)) {
      fprintf(stderr, "sandbox daemon: eval request requires string source\n");
      goto done;
    }

    out->mode = ANT_SANDBOX_REQUEST_EVAL;
    out->source = sandbox_dup_yyjson_str(source);
  } else {
    fprintf(stderr, "sandbox daemon: unsupported request mode '%s'\n", mode_str);
    goto done;
  }

  yyjson_val *cwd = yyjson_obj_get(root, "cwd");
  if (cwd) {
    if (!yyjson_is_str(cwd)) {
      fprintf(stderr, "sandbox daemon: cwd must be a string\n");
      goto done;
    }

    out->cwd = sandbox_dup_yyjson_str(cwd);
  }

  if (!sandbox_parse_argv(yyjson_obj_get(root, "argv"), out)) goto done;

  ok = true;

done:
  yyjson_doc_free(doc);
  if (!ok) ant_sandbox_request_free(out);
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
