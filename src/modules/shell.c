#include <compat.h> // IWYU pragma: keep

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "debug.h"
#include "errors.h"
#include "gc/roots.h"
#include "internal.h"
#include "ptr.h"

#include "modules/collections.h"
#include "modules/blob.h"
#include "modules/buffer.h"
#include "modules/json.h"
#include "modules/textcodec.h"
#include "shell/shell_internal.h"
#include "modules/symbol.h"

#include "silver/compiler.h"
#include "silver/engine.h"

static void shell_compiled_program_free(sh_compiled_program_t *compiled) {
  if (!compiled) return;
  sh_program_free(&compiled->program);
  free(compiled);
}

static void shell_compiled_holder_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t holder = js_obj_from_ptr(obj);
  sh_compiled_program_t *compiled = js_get_native(
    holder, SH_COMPILED_PROGRAM_TAG
  );
  shell_compiled_program_free(compiled);
  js_clear_native(holder, SH_COMPILED_PROGRAM_TAG);
}

static bool shell_output_stdout_bytes(
  ant_t *js, ant_value_t output, const uint8_t **bytes, size_t *len
) {
  if (!is_special_object(output)) return false;
  return buffer_source_get_bytes(js, js_get(js, output, "stdout"), bytes, len);
}

static ant_value_t shell_output_text_value(ant_t *js, ant_value_t output) {
  const uint8_t *bytes = NULL;
  size_t len = 0;
  if (!shell_output_stdout_bytes(js, output, &bytes, &len))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid ShellOutput receiver");
  td_state_t decoder = { .encoding = TD_ENC_UTF8 };
  return td_decode(js, &decoder, bytes, len, false);
}

static ant_value_t shell_output_text(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  return shell_output_text_value(js, js->this_val);
}

static ant_value_t shell_output_json_value(ant_t *js, ant_value_t output) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, output);
  ant_value_t text = shell_output_text_value(js, output);
  if (is_err(text)) {
    GC_ROOT_RESTORE(js, root_mark);
    return text;
  }
  GC_ROOT_PIN(js, text);
  ant_value_t result = json_parse_value(js, text);
  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static ant_value_t shell_output_json(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  return shell_output_json_value(js, js->this_val);
}

static ant_value_t shell_output_array_buffer_value(
  ant_t *js, ant_value_t output
) {
  ant_value_t stdout_value = js_get(js, output, "stdout");
  if (!buffer_get_typedarray_data(stdout_value))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid ShellOutput receiver");
  return js_get(js, stdout_value, "buffer");
}

static ant_value_t shell_output_array_buffer(
  ant_t *js, ant_value_t *args, int nargs
) {
  (void)args;
  (void)nargs;
  return shell_output_array_buffer_value(js, js->this_val);
}

static ant_value_t shell_output_bytes_value(ant_t *js, ant_value_t output) {
  ant_value_t stdout_value = js_get(js, output, "stdout");
  TypedArrayData *typed = buffer_get_typedarray_data(stdout_value);
  if (!typed || typed->type != TYPED_ARRAY_UINT8)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid ShellOutput receiver");
  ant_value_t array_buffer = js_get(js, stdout_value, "buffer");
  return create_typed_array_with_buffer(
    js, TYPED_ARRAY_UINT8, typed->buffer,
    typed->byte_offset, typed->byte_length, "Uint8Array", array_buffer
  );
}

static ant_value_t shell_output_bytes(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  return shell_output_bytes_value(js, js->this_val);
}

static ant_value_t shell_output_blob_value(ant_t *js, ant_value_t output) {
  const uint8_t *bytes = NULL;
  size_t len = 0;
  if (!shell_output_stdout_bytes(js, output, &bytes, &len))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid ShellOutput receiver");
  return blob_create(js, bytes, len, "");
}

static ant_value_t shell_output_blob(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  return shell_output_blob_value(js, js->this_val);
}

static ant_value_t shell_output_lines_value(ant_t *js, ant_value_t output) {
  ant_value_t text_value = shell_output_text_value(js, output);
  if (is_err(text_value)) return text_value;
  size_t text_len = 0;
  char *text = js_getstr(js, text_value, &text_len);
  if (!text) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid shell output");

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, output);
  GC_ROOT_PIN(js, text_value);
  ant_value_t lines = js_mkarr(js);
  GC_ROOT_PIN(js, lines);
  size_t line_start = 0;
  for (size_t i = 0; i <= text_len; i++) {
    if (i != text_len && text[i] != '\n') continue;
    if (i > line_start || i < text_len)
      js_arr_push(js, lines, js_mkstr(
        js, text + line_start, i - line_start
      ));
    line_start = i + 1;
  }
  GC_ROOT_RESTORE(js, root_mark);
  return lines;
}

static ant_value_t shell_output_lines(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  return shell_output_lines_value(js, js->this_val);
}

static ant_value_t shell_output_constructor(
  ant_t *js, ant_value_t *args, int nargs
) {
  (void)args;
  (void)nargs;
  return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal constructor");
}

static ant_value_t shell_output_from_result(
  ant_t *js, ant_value_t result, ant_value_t prototype
) {
  const uint8_t *bytes = NULL;
  size_t len = 0;
  if (!buffer_source_get_bytes(
      js, js_get(js, result, "stdout"), &bytes, &len
    ) || !buffer_source_get_bytes(
      js, js_get(js, result, "stderr"), &bytes, &len
    ))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid shell output");
  js_set_proto_init(result, prototype);
  (void)js_delete_prop(js, result, "exited", sizeof("exited") - 1);
  return result;
}

static ant_value_t shell_policy_fulfilled(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t result = nargs > 0 ? args[0] : js_mkundef();
  if (!is_special_object(result)) return result;

  ant_value_t exit_code = js_get(js, result, "exitCode");
  ant_value_t output = shell_output_from_result(
    js, result, js_get(js, state, "outputPrototype")
  );
  if (is_err(output)) return output;
  ant_value_t nothrow = js_get(js, state, "nothrow");
  if (vtype(exit_code) != T_NUM || (int)js_getnum(exit_code) == 0 || js_truthy(js, nothrow))
    return output;

  char message[96];
  snprintf(message, sizeof(message), "Shell command failed with exit code %d",
    (int)js_getnum(exit_code));
    
  ant_value_t error = js_make_error_silent(js, JS_ERR_GENERIC, message);
  if (is_special_object(error)) {
    js_set(js, error, "exitCode", exit_code);
    js_set(js, error, "stdout", js_get(js, output, "stdout"));
    js_set(js, error, "stderr", js_get(js, output, "stderr"));
  }
  
  return js_throw(js, error);
}

static ant_value_t shell_text_fulfilled(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkstr(js, "", 0);
  return shell_output_text_value(js, args[0]);
}

static ant_value_t shell_lines_fulfilled(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkarr(js);
  return shell_output_lines_value(js, args[0]);
}

static ant_value_t shell_json_fulfilled(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  return shell_output_json_value(js, args[0]);
}

static ant_value_t shell_array_buffer_fulfilled(
  ant_t *js, ant_value_t *args, int nargs
) {
  if (nargs < 1) return js_mkundef();
  return shell_output_array_buffer_value(js, args[0]);
}

static ant_value_t shell_bytes_fulfilled(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  return shell_output_bytes_value(js, args[0]);
}

static ant_value_t shell_blob_fulfilled(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  return shell_output_blob_value(js, args[0]);
}

static ant_value_t shell_promise_state(ant_t *js) {
  if (!is_special_object(js->this_val)) return js_mkundef();
  return js_get_slot(js->this_val, SLOT_DATA);
}

static ant_value_t shell_promise_nothrow(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = shell_promise_state(js);
  if (!is_special_object(state)) return js_mkerr(js, "Invalid shell promise");
  js_set(js, state, "nothrow", js_true);
  return js->this_val;
}

static ant_value_t shell_promise_then(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = shell_promise_state(js);
  ant_value_t promise = js_get(js, state, "promise");
  if (vtype(promise) != T_PROMISE) return js_mkerr(js, "Invalid shell promise");
  ant_value_t fulfilled = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t rejected = nargs > 1 ? args[1] : js_mkundef();
  return js_promise_then(js, promise, fulfilled, rejected);
}

static ant_value_t shell_promise_catch(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = shell_promise_state(js);
  ant_value_t promise = js_get(js, state, "promise");
  if (vtype(promise) != T_PROMISE) return js_mkerr(js, "Invalid shell promise");
  ant_value_t rejected = nargs > 0 ? args[0] : js_mkundef();
  return js_promise_then(js, promise, js_mkundef(), rejected);
}

static ant_value_t shell_promise_finally(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = shell_promise_state(js);
  ant_value_t promise = js_get(js, state, "promise");
  if (vtype(promise) != T_PROMISE) return js_mkerr(js, "Invalid shell promise");

  ant_value_t finally_method = js_get(js, promise, "finally");
  if (!is_callable(finally_method)) return js_mkerr(js, "Invalid Promise.finally");
  ant_value_t callback = nargs > 0 ? args[0] : js_mkundef();
  return sv_vm_call(
    js->vm, js, finally_method, promise,
    &callback, 1, NULL, false
  );
}

static ant_value_t shell_promise_text(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t state = shell_promise_state(js);
  ant_value_t promise = js_get(js, state, "promise");
  if (vtype(promise) != T_PROMISE) return js_mkerr(js, "Invalid shell promise");
  return js_promise_then(js, promise, js_mkfun(shell_text_fulfilled), js_mkundef());
}

static ant_value_t shell_promise_lines(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t state = shell_promise_state(js);
  ant_value_t promise = js_get(js, state, "promise");
  if (vtype(promise) != T_PROMISE) return js_mkerr(js, "Invalid shell promise");
  return js_promise_then(js, promise, js_mkfun(shell_lines_fulfilled), js_mkundef());
}

static ant_value_t shell_promise_json(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t state = shell_promise_state(js);
  ant_value_t promise = js_get(js, state, "promise");
  if (vtype(promise) != T_PROMISE) return js_mkerr(js, "Invalid shell promise");
  return js_promise_then(js, promise, js_mkfun(shell_json_fulfilled), js_mkundef());
}

static ant_value_t shell_promise_array_buffer(
  ant_t *js, ant_value_t *args, int nargs
) {
  (void)args;
  (void)nargs;
  ant_value_t state = shell_promise_state(js);
  ant_value_t promise = js_get(js, state, "promise");
  if (vtype(promise) != T_PROMISE) return js_mkerr(js, "Invalid shell promise");
  return js_promise_then(
    js, promise, js_mkfun(shell_array_buffer_fulfilled), js_mkundef()
  );
}

static ant_value_t shell_promise_bytes(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t state = shell_promise_state(js);
  ant_value_t promise = js_get(js, state, "promise");
  if (vtype(promise) != T_PROMISE) return js_mkerr(js, "Invalid shell promise");
  return js_promise_then(js, promise, js_mkfun(shell_bytes_fulfilled), js_mkundef());
}

static ant_value_t shell_promise_blob(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t state = shell_promise_state(js);
  ant_value_t promise = js_get(js, state, "promise");
  if (vtype(promise) != T_PROMISE) return js_mkerr(js, "Invalid shell promise");
  return js_promise_then(js, promise, js_mkfun(shell_blob_fulfilled), js_mkundef());
}

static ant_value_t shell_wrap_promise(
  ant_t *js, ant_value_t raw_promise,
  ant_value_t promise_prototype, ant_value_t output_prototype
) {
  if (vtype(raw_promise) != T_PROMISE) return raw_promise;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, raw_promise);
  GC_ROOT_PIN(js, promise_prototype);
  GC_ROOT_PIN(js, output_prototype);
  
  ant_value_t state = js_mkobj(js);
  GC_ROOT_PIN(js, state);
  
  js_set(js, state, "nothrow", js_false);
  js_set(js, state, "outputPrototype", output_prototype);
  
  ant_value_t fulfilled = js_heavy_mkfun(js, shell_policy_fulfilled, state);
  ant_value_t promise = js_promise_then(js, raw_promise, fulfilled, js_mkundef());
  
  GC_ROOT_PIN(js, promise);
  js_set(js, state, "promise", promise);

  ant_value_t wrapper = js_mkobj(js);
  GC_ROOT_PIN(js, wrapper);
  
  js_set_proto_init(wrapper, promise_prototype);
  js_set_slot_wb(js, wrapper, SLOT_DATA, state);
  GC_ROOT_RESTORE(js, root_mark);
  
  return wrapper;
}

static sh_compiled_program_t *shell_cache_lookup(
  ant_t *js,
  ant_value_t state,
  ant_value_t key,
  ant_value_t *holder_out
) {
  ant_value_t cache = js_get(js, state, "cache");
  ant_value_t holder = collections_weakmap_get(cache, key);
  sh_compiled_program_t *compiled = js_get_native(
    holder, SH_COMPILED_PROGRAM_TAG
  );
  if (compiled && holder_out) *holder_out = holder;
  return compiled;
}

static ant_value_t shell_cache_store(
  ant_t *js,
  ant_value_t state,
  ant_value_t key,
  sh_compiled_program_t *compiled
) {
  ant_value_t cache = js_get(js, state, "cache");
  ant_value_t holder = js_mkobj(js);
  js_set_native(holder, compiled, SH_COMPILED_PROGRAM_TAG);

  if (js_get_native(holder, SH_COMPILED_PROGRAM_TAG) != compiled)
    return js_mkundef();
  js_set_finalizer(holder, shell_compiled_holder_finalize);
  (void)collections_weakmap_set(js, cache, key, holder);
  return holder;
}

static bool shell_template_segments(
  ant_t *js,
  ant_value_t input,
  const char ***segments_out,
  size_t **lengths_out,
  size_t *count_out
) {
  ant_value_t raw = js_get(js, input, "raw");
  ant_value_t strings = vtype(raw) == T_ARR ? raw : input;
  if (vtype(strings) != T_ARR) return false;
  
  ant_offset_t count = js_arr_len(js, strings);
  if (count <= 0) return false;

  const char **segments = calloc((size_t)count, sizeof(*segments));
  size_t *lengths = calloc((size_t)count, sizeof(*lengths));
  if (!segments || !lengths) {
    free(segments);
    free(lengths);
    return false;
  }

  for (ant_offset_t i = 0; i < count; i++) {
    ant_value_t value = js_arr_get(js, strings, i);
    if (vtype(value) != T_STR) {
      free(segments);
      free(lengths);
      return false;
    }
    segments[i] = js_getstr(js, value, &lengths[i]);
    if (!segments[i]) {
      free(segments);
      free(lengths);
      return false;
    }
  }

  *segments_out = segments;
  *lengths_out = lengths;
  *count_out = (size_t)count;
  return true;
}

static sh_compiled_program_t *shell_compile(
  ant_t *js,
  const char *const *segments,
  const size_t *lengths,
  size_t segment_count
) {
  sh_compiled_program_t *compiled = calloc(1, sizeof(*compiled));
  if (!compiled) {
    js_mkerr(js, "Out of memory");
    return NULL;
  }
  sh_parse_error_t error = {0};
  
  if (!sh_parse_segments(
    segments, lengths, segment_count, &compiled->program, &error
  )) {
    js_mkerr_typed(js, JS_ERR_SYNTAX, "ant:shell: %s at template segment %zu offset %zu",
    error.message[0] ? error.message : "parse error", error.segment, error.offset);
    shell_compiled_program_free(compiled);
    return NULL;
  }

  size_t source_len = 0;
  char *source = sh_compile_program_source(
    &compiled->program, &source_len, &error
  );
  
  if (!source) {
    js_mkerr(js, "ant:shell: %s", error.message[0] ? error.message : "compile error");
    shell_compiled_program_free(compiled);
    return NULL;
  }

  if (sv_dump_shell_unlikely) {
    fprintf(stderr, "[shell:compile] JavaScript (%zu bytes)\n", source_len);
    if (source_len) fwrite(source, 1, source_len, stderr);
    fputc('\n', stderr);

    size_t plan_len = 0;
    char *plan = sh_debug_program_plan_source(&compiled->program, &plan_len);
    fprintf(stderr, "[shell:compile] __plan (%zu bytes)\n", plan_len);
    if (plan) {
      if (plan_len) fwrite(plan, 1, plan_len, stderr);
      free(plan);
    } else fputs("<unavailable>", stderr);
    fputc('\n', stderr);
  }

  static const sv_param_t params[] = {
    SV_PARAM("__run"),
    SV_PARAM("__finish"),
    SV_PARAM("__ctx"),
    SV_PARAM("__values"),
    SV_PARAM("__plan"),
  };
  
  compiled->func = sv_compile_function_with_params(
    js, params, 5, source,
    source_len, true
  );
  
  free(source);
  if (!compiled->func) {
    shell_compiled_program_free(compiled);
    return NULL;
  }
  return compiled;
}

static void shell_debug_write_string(
  FILE *stream, const char *text, size_t len
) {
  fputc('"', stream);
  for (size_t i = 0; i < len; i++) {
    unsigned char ch = (unsigned char)text[i];
    switch (ch) {
      case '"': fputs("\\\"", stream); break;
      case '\\': fputs("\\\\", stream); break;
      case '\b': fputs("\\b", stream); break;
      case '\f': fputs("\\f", stream); break;
      case '\n': fputs("\\n", stream); break;
      case '\r': fputs("\\r", stream); break;
      case '\t': fputs("\\t", stream); break;
      default:
        if (ch < 0x20) fprintf(stream, "\\u%04x", ch);
        else fputc(ch, stream);
        break;
    }
  }
  fputc('"', stream);
}

static void shell_debug_write_value(
  ant_t *js, FILE *stream, ant_value_t value, unsigned depth
) {
  switch (vtype(value)) {
    case T_STR: {
      size_t len = 0;
      const char *text = js_getstr(js, value, &len);
      shell_debug_write_string(stream, text ? text : "", text ? len : 0);
      break;
    }
    case T_NUM: fprintf(stream, "%.17g", js_getnum(value)); break;
    case T_BOOL: fputs(js_truthy(js, value) ? "true" : "false", stream); break;
    case T_NULL: fputs("null", stream); break;
    case T_UNDEF: fputs("undefined", stream); break;
    case T_ARR: {
      if (depth >= 4) {
        fputs("<array>", stream);
        break;
      }
      ant_offset_t len = js_arr_len(js, value);
      ant_offset_t shown = len < 32 ? len : 32;
      fputc('[', stream);
      for (ant_offset_t i = 0; i < shown; i++) {
        if (i) fputs(", ", stream);
        shell_debug_write_value(js, stream, js_arr_get(js, value, i), depth + 1);
      }
      if (shown < len) fprintf(stream, ", ... %u more", (unsigned)(len - shown));
      fputc(']', stream);
      break;
    }
    case T_TYPEDARRAY: {
      TypedArrayData *typed = buffer_get_typedarray_data(value);
      fprintf(
        stream, "<%s length=%zu>",
        typed ? buffer_typedarray_type_name(typed->type) : "TypedArray",
        typed ? typed->length : 0
      );
      break;
    }
    case T_BIGINT: fputs("<bigint>", stream); break;
    case T_SYMBOL: fputs("<symbol>", stream); break;
    case T_FUNC:
    case T_CFUNC: fputs("<function>", stream); break;
    case T_PROMISE: fputs("<promise>", stream); break;
    case T_GENERATOR: fputs("<generator>", stream); break;
    case T_ERR: fputs("<error>", stream); break;
    default: fputs("<object>", stream); break;
  }
}

static void shell_debug_dump_invocation(
  ant_t *js, const sh_compiled_program_t *compiled,
  ant_value_t context, ant_value_t values
) {
  fputs("[shell:invoke] bindings\n", stderr);
  fputs("__run = [native sh_runtime_run]\n", stderr);
  fputs("__finish = [native sh_runtime_finish]\n", stderr);
  fputs("__ctx = { cwd: ", stderr);
  shell_debug_write_value(js, stderr, js_get(js, context, "cwd"), 0);
  fprintf(
    stderr, ", accumulator: %s }\n",
    compiled->program.clause_count == 1 ? "false" : "true"
  );
  fputs("__values = ", stderr);
  shell_debug_write_value(js, stderr, values, 0);
  fputc('\n', stderr);
  fprintf(
    stderr, "__plan = [compiled shell plan: %zu clause%s]\n",
    compiled->program.clause_count,
    compiled->program.clause_count == 1 ? "" : "s"
  );
}

static ant_value_t builtin_shell_dollar(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "$() requires a tagged template");
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);

  const char **segments = NULL;
  size_t *lengths = NULL;
  size_t segment_count = 0;
  
  if (!is_special_object(args[0]) || !shell_template_segments(
    js, args[0], &segments,
    &lengths, &segment_count
  )) return js_mkerr_typed(js, JS_ERR_TYPE, "$() must be used as a tagged template");

  ant_value_t cache_key = args[0];
  ant_value_t holder = js_mkundef();
  sh_compiled_program_t *compiled = shell_cache_lookup(
    js, state, cache_key, &holder
  );

  // TODO: reduce nesting
  if (!compiled) {
    compiled = shell_compile(js, segments, lengths, segment_count);
    if (compiled) {
      holder = shell_cache_store(js, state, cache_key, compiled);
      if (is_undefined(holder)) {
        shell_compiled_program_free(compiled);
        compiled = NULL;
        js_mkerr(js, "Out of memory");
      }
    }
  }

  free((void *)segments);
  free(lengths);
  if (!compiled) return js->thrown_exists
    ? mkval(T_ERR, 0)
    : js_mkerr(js, "Shell compilation failed");

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, holder);
  ant_value_t values = js_mkarr(js);
  
  GC_ROOT_PIN(js, values);
  for (int i = 1; i < nargs; i++) js_arr_push(js, values, args[i]);
  
  ant_value_t context = sh_runtime_context(
    js, compiled->program.clause_count != 1
  );
  if (is_err(context)) {
    GC_ROOT_RESTORE(js, root_mark);
    return context;
  }
  GC_ROOT_PIN(js, context);

  if (sv_dump_shell_unlikely)
    shell_debug_dump_invocation(js, compiled, context, values);
  
  ant_value_t call_args[] = {
    js_mkfun(sh_runtime_run),
    js_mkfun(sh_runtime_finish),
    context, values, holder,
  };
  
  ant_value_t raw_promise = sv_call_compiled_zero_upvalues(
    js, compiled->func, 
    js_mkundef(), call_args, 5
  );
  
  GC_ROOT_PIN(js, raw_promise);
  ant_value_t wrapper = shell_wrap_promise(
    js, raw_promise,
    js_get(js, state, "promisePrototype"),
    js_get(js, state, "outputPrototype")
  );
  GC_ROOT_RESTORE(js, root_mark);
  
  return wrapper;
}

ant_value_t shell_library(ant_t *js) {
  GC_ROOT_SAVE(root_mark, js);
  
  ant_value_t lib = js_mkobj(js);
  GC_ROOT_PIN(js, lib);
  
  ant_value_t state = js_mkobj(js);
  GC_ROOT_PIN(js, state);

  ant_value_t output_prototype = js_mkobj(js);
  GC_ROOT_PIN(js, output_prototype);
  js_set_proto_init(output_prototype, js->sym.object_proto);
  defmethod(js, output_prototype, "text", 4, js_mkfun(shell_output_text));
  defmethod(js, output_prototype, "json", 4, js_mkfun(shell_output_json));
  defmethod(js, output_prototype, "arrayBuffer", 11,
    js_mkfun(shell_output_array_buffer));
  defmethod(js, output_prototype, "bytes", 5, js_mkfun(shell_output_bytes));
  defmethod(js, output_prototype, "blob", 4, js_mkfun(shell_output_blob));
  defmethod(js, output_prototype, "lines", 5, js_mkfun(shell_output_lines));
  
  (void)js_make_ctor(
    js, shell_output_constructor,
    output_prototype, "ShellOutput", 11
  );

  ant_value_t promise_prototype = js_mkobj(js);
  GC_ROOT_PIN(js, promise_prototype);
  js_set_proto_init(promise_prototype, js->sym.object_proto);
  defmethod(js, promise_prototype, "then", 4, js_mkfun(shell_promise_then));
  defmethod(js, promise_prototype, "catch", 5, js_mkfun(shell_promise_catch));
  defmethod(js, promise_prototype, "finally", 7,
    js_mkfun(shell_promise_finally));
  defmethod(js, promise_prototype, "nothrow", 7,
    js_mkfun(shell_promise_nothrow));
  defmethod(js, promise_prototype, "text", 4, js_mkfun(shell_promise_text));
  defmethod(js, promise_prototype, "json", 4, js_mkfun(shell_promise_json));
  defmethod(js, promise_prototype, "arrayBuffer", 11,
    js_mkfun(shell_promise_array_buffer));
  defmethod(js, promise_prototype, "bytes", 5, js_mkfun(shell_promise_bytes));
  defmethod(js, promise_prototype, "blob", 4, js_mkfun(shell_promise_blob));
  defmethod(js, promise_prototype, "lines", 5, js_mkfun(shell_promise_lines));

  (void)js_make_ctor(
    js, shell_output_constructor,
    promise_prototype, "ShellPromise", 12
  );

  js_set(js, state, "cache", collections_make_weakmap(js));
  js_set(js, state, "outputPrototype", output_prototype);
  js_set(js, state, "promisePrototype", promise_prototype);
  js_set(js, lib, "$", js_heavy_mkfun(js, builtin_shell_dollar, state));
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "shell", 5));

  GC_ROOT_RESTORE(js, root_mark);
  return lib;
}
