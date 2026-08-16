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

static ant_value_t shell_policy_fulfilled(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t result = nargs > 0 ? args[0] : js_mkundef();
  if (!is_special_object(result)) return result;

  ant_value_t exit_code = js_get(js, result, "exitCode");
  ant_value_t exited = js_get(js, result, "exited");
  if (!is_undefined(exited))
    (void)js_delete_prop(js, result, "exited", sizeof("exited") - 1);
  ant_value_t nothrow = js_get(js, state, "nothrow");
  if (vtype(exit_code) != T_NUM || (int)js_getnum(exit_code) == 0 || js_truthy(js, nothrow))
    return result;

  char message[96];
  snprintf(message, sizeof(message), "Shell command failed with exit code %d",
    (int)js_getnum(exit_code));
    
  ant_value_t error = js_make_error_silent(js, JS_ERR_GENERIC, message);
  if (is_special_object(error)) {
    js_set(js, error, "exitCode", exit_code);
    js_set(js, error, "stdout", js_get(js, result, "stdout"));
    js_set(js, error, "stderr", js_get(js, result, "stderr"));
  }
  
  return js_throw(js, error);
}

static ant_value_t shell_text_fulfilled(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_special_object(args[0])) return js_mkstr(js, "", 0);
  ant_value_t stdout_value = js_get(js, args[0], "stdout");
  return vtype(stdout_value) == T_STR ? stdout_value : js_mkstr(js, "", 0);
}

static ant_value_t shell_lines_fulfilled(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t lines = js_mkarr(js);
  if (nargs < 1 || !is_special_object(args[0])) return lines;
  ant_value_t stdout_value = js_get(js, args[0], "stdout");
  if (vtype(stdout_value) != T_STR) return lines;

  size_t text_len = 0;
  char *text = js_getstr(js, stdout_value, &text_len);
  if (!text) return lines;
  size_t line_start = 0;
  
  for (size_t i = 0; i <= text_len; i++) {
    if (i != text_len && text[i] != '\n') continue;
    if (i > line_start || i < text_len)
      js_arr_push(js, lines, js_mkstr(js, text + line_start, i - line_start));
    line_start = i + 1;
  }
  return lines;
}

static ant_value_t shell_promise_nothrow(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  js_set(js, state, "nothrow", js_true);
  return js_get(js, state, "wrapper");
}

static ant_value_t shell_promise_then(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t promise = js_get(js, state, "promise");
  if (vtype(promise) != T_PROMISE) return js_mkerr(js, "Invalid shell promise");
  ant_value_t fulfilled = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t rejected = nargs > 1 ? args[1] : js_mkundef();
  return js_promise_then(js, promise, fulfilled, rejected);
}

static ant_value_t shell_promise_catch(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t promise = js_get(js, state, "promise");
  if (vtype(promise) != T_PROMISE) return js_mkerr(js, "Invalid shell promise");
  ant_value_t rejected = nargs > 0 ? args[0] : js_mkundef();
  return js_promise_then(js, promise, js_mkundef(), rejected);
}

static ant_value_t shell_promise_finally(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
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
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t promise = js_get(js, state, "promise");
  if (vtype(promise) != T_PROMISE) return js_mkerr(js, "Invalid shell promise");
  return js_promise_then(js, promise, js_mkfun(shell_text_fulfilled), js_mkundef());
}

static ant_value_t shell_promise_lines(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t promise = js_get(js, state, "promise");
  if (vtype(promise) != T_PROMISE) return js_mkerr(js, "Invalid shell promise");
  return js_promise_then(js, promise, js_mkfun(shell_lines_fulfilled), js_mkundef());
}

static ant_value_t shell_wrap_promise(ant_t *js, ant_value_t raw_promise) {
  if (vtype(raw_promise) != T_PROMISE) return raw_promise;

  ant_value_t state = js_mkobj(js);
  js_set(js, state, "nothrow", js_false);
  ant_value_t fulfilled = js_heavy_mkfun(js, shell_policy_fulfilled, state);
  ant_value_t promise = js_promise_then(js, raw_promise, fulfilled, js_mkundef());
  js_set(js, state, "promise", promise);

  ant_value_t wrapper = js_mkobj(js);
  js_set(js, state, "wrapper", wrapper);
  js_set(js, wrapper, "then", js_heavy_mkfun(js, shell_promise_then, state));
  js_set(js, wrapper, "catch", js_heavy_mkfun(js, shell_promise_catch, state));
  js_set(js, wrapper, "finally", js_heavy_mkfun(js, shell_promise_finally, state));
  js_set(js, wrapper, "nothrow", js_heavy_mkfun(js, shell_promise_nothrow, state));
  js_set(js, wrapper, "text", js_heavy_mkfun(js, shell_promise_text, state));
  js_set(js, wrapper, "lines", js_heavy_mkfun(js, shell_promise_lines, state));
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
  }

  static const sv_param_t params[] = {
    SV_PARAM("__run"),
    SV_PARAM("__ctx"),
    SV_PARAM("__values"),
    SV_PARAM("__plan"),
  };
  
  compiled->func = sv_compile_function_with_params(
    js, params, 4, source,
    source_len, true
  );
  
  free(source);
  if (!compiled->func) {
    shell_compiled_program_free(compiled);
    return NULL;
  }
  return compiled;
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
  
  ant_value_t context = sh_runtime_context(js);
  if (is_err(context)) {
    GC_ROOT_RESTORE(js, root_mark);
    return context;
  }
  GC_ROOT_PIN(js, context);
  
  ant_value_t call_args[] = {
    js_mkfun(sh_runtime_run),
    context, values, holder,
  };
  
  ant_value_t raw_promise = sv_call_compiled_zero_upvalues(
    js, compiled->func, 
    js_mkundef(), call_args, 4
  );
  
  GC_ROOT_PIN(js, raw_promise);
  ant_value_t wrapper = shell_wrap_promise(js, raw_promise);
  GC_ROOT_RESTORE(js, root_mark);
  
  return wrapper;
}

ant_value_t shell_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);
  ant_value_t state = js_mkobj(js);
  
  js_set(js, state, "cache", collections_make_weakmap(js));
  js_set(js, lib, "$", js_heavy_mkfun(js, builtin_shell_dollar, state));
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "shell", 5));
  
  return lib;
}
