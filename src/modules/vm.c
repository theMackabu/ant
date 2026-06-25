#include <compat.h> // IWYU pragma: keep

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "descriptors.h"
#include "errors.h"
#include "internal.h"
#include "modules/buffer.h"
#include "modules/json.h"
#include "modules/symbol.h"
#include "silver/engine.h"

#define VM_CONTEXT_MARKER "__ant_vm_context__"
#define VM_CONTEXT_MARKER_LEN 18
#define VM_APPEND_LITERAL(buf, len, cap, literal) \
  vm_append((buf), (len), (cap), (literal), sizeof(literal) - 1)

static ant_value_t g_script_proto = 0;
static ant_value_t g_module_proto = 0;
static ant_value_t g_source_text_module_proto = 0;
static ant_value_t g_synthetic_module_proto = 0;

static bool vm_append(char **buf, size_t *len, size_t *cap, const char *src, size_t src_len);
static ant_value_t vm_run_in_new_context(ant_t *js, ant_value_t *args, int nargs);

static ant_value_t vm_invalid_arg_type(ant_t *js, const char *name, const char *expected) {
  ant_value_t err = js_mkerr_typed(js, JS_ERR_TYPE, "The \"%s\" argument must be of type %s.", name, expected);
  if (is_object_type(err)) js_set(js, err, "code", ANT_STRING("ERR_INVALID_ARG_TYPE"));
  return err;
}

static ant_value_t vm_unsupported(ant_t *js, const char *feature) {
  ant_value_t err = js_mkerr_typed(js, JS_ERR_TYPE, "%s is not implemented by Ant's node:vm module", feature);
  if (is_object_type(err)) js_set(js, err, "code", ANT_STRING("ERR_VM_UNSUPPORTED"));
  return err;
}

static ant_value_t vm_to_string_arg(ant_t *js, ant_value_t value, const char *name) {
  if (vtype(value) != T_STR) return vm_invalid_arg_type(js, name, "string");
  return value;
}

static const char *vm_string_ptr(ant_value_t value, ant_offset_t *len_out) {
  ant_offset_t len = 0;
  ant_offset_t off = vstr(rt->js, value, &len);
  if (len_out) *len_out = len;
  return (const char *)(uintptr_t)off;
}

static ant_value_t vm_get_filename(ant_t *js, ant_value_t options, const char *fallback, size_t fallback_len) {
  if (vtype(options) == T_STR) return options;

  if (is_object_type(options)) {
    ant_value_t filename = js_get(js, options, "filename");
    if (vtype(filename) == T_STR) return filename;
  }

  return js_mkstr(js, fallback, fallback_len);
}

static ant_value_t vm_eval_source(ant_t *js, ant_value_t code, ant_value_t filename) {
  ant_offset_t code_len = 0;
  const char *code_str = vm_string_ptr(code, &code_len);

  const char *prev_filename = js->filename;
  char *filename_copy = NULL;

  if (vtype(filename) == T_STR) {
    size_t filename_len = 0;
    const char *filename_str = js_getstr(js, filename, &filename_len);
    if (filename_str) {
      filename_copy = (char *)malloc(filename_len + 1);
      if (!filename_copy) return js_mkerr(js, "oom");
      memcpy(filename_copy, filename_str, filename_len);
      filename_copy[filename_len] = '\0';
      js_set_filename(js, filename_copy);
    }
  }

  ant_value_t result = js_eval_bytecode_eval(js, code_str, (size_t)code_len);
  js_set_filename(js, prev_filename);
  free(filename_copy);
  return result;
}

static ant_value_t vm_make_cached_data(ant_t *js) {
  ArrayBufferData *buffer = create_array_buffer_data(0);
  if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
  return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, 0, "Buffer");
}

static ant_value_t vm_script_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Class constructor Script cannot be invoked without 'new'");
  if (nargs < 1) return vm_invalid_arg_type(js, "code", "string");

  ant_value_t code = vm_to_string_arg(js, args[0], "code");
  if (is_err(code)) return code;

  ant_value_t options = nargs > 1 ? args[1] : js_mkundef();
  ant_value_t filename = vm_get_filename(js, options, "evalmachine.<anonymous>", 23);
  if (is_err(filename)) return filename;

  ant_value_t this_obj = js->this_val;
  if (!is_object_type(this_obj)) this_obj = js_mkobj(js);
  if (is_err(this_obj)) return this_obj;

  if (is_object_type(g_script_proto)) js_set_proto_init(this_obj, g_script_proto);
  js_set_slot_wb(js, this_obj, SLOT_DATA, code);
  js_set_slot_wb(js, this_obj, SLOT_AUX, filename);
  js_set(js, this_obj, "code", code);
  js_set_descriptor(js, this_obj, "code", 4, 0);
  js_set(js, this_obj, "filename", filename);
  js_set_descriptor(js, this_obj, "filename", 8, 0);
  js_set(js, this_obj, "sourceMapURL", js_mkundef());

  ant_value_t cached_rejected = js_mkundef();
  if (is_object_type(options)) {
    ant_value_t cached_data = js_get(js, options, "cachedData");
    if (vtype(cached_data) != T_UNDEF) cached_rejected = js_false;
  }
  js_set(js, this_obj, "cachedDataRejected", cached_rejected);

  return this_obj;
}

static ant_value_t vm_script_code(ant_t *js, ant_value_t this_val) {
  if (!is_object_type(this_val)) return js_mkerr_typed(js, JS_ERR_TYPE, "Script method called on incompatible receiver");
  ant_value_t code = js_get_slot(this_val, SLOT_DATA);
  if (vtype(code) != T_STR) return js_mkerr_typed(js, JS_ERR_TYPE, "Script method called on incompatible receiver");
  return code;
}

static ant_value_t vm_script_filename(ant_t *js, ant_value_t this_val, ant_value_t options) {
  if (is_object_type(options) || vtype(options) == T_STR)
    return vm_get_filename(js, options, "evalmachine.<anonymous>", 23);

  ant_value_t filename = js_get_slot(this_val, SLOT_AUX);
  if (vtype(filename) == T_STR) return filename;
  return js_mkstr(js, "evalmachine.<anonymous>", 23);
}

static ant_value_t vm_script_create_cached_data(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t code = vm_script_code(js, js->this_val);
  if (is_err(code)) return code;
  return vm_make_cached_data(js);
}

static ant_value_t vm_script_run_in_this_context(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t code = vm_script_code(js, js->this_val);
  if (is_err(code)) return code;
  ant_value_t options = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t filename = vm_script_filename(js, js->this_val, options);
  if (is_err(filename)) return filename;
  return vm_eval_source(js, code, filename);
}

static ant_value_t vm_is_context_value(ant_t *js, ant_value_t value) {
  if (!is_object_type(value)) return js_false;
  ant_value_t marker = js_get(js, value, VM_CONTEXT_MARKER);
  return js_bool(marker == js_true);
}

static ant_value_t vm_validate_context(ant_t *js, ant_value_t value) {
  if (vm_is_context_value(js, value) == js_true) return js_mkundef();
  return vm_invalid_arg_type(js, "contextifiedObject", "vm.Context");
}

static ant_value_t vm_create_context(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t context = nargs > 0 ? args[0] : js_mkobj(js);
  if (vtype(context) == T_UNDEF) context = js_mkobj(js);
  if (!is_object_type(context) && vtype(context) != T_FUNC)
    return vm_invalid_arg_type(js, "contextObject", "object");

  js_set(js, context, VM_CONTEXT_MARKER, js_true);
  js_set_descriptor(js, context, VM_CONTEXT_MARKER, VM_CONTEXT_MARKER_LEN, JS_DESC_C);
  return context;
}

static ant_value_t vm_is_context(ant_t *js, ant_value_t *args, int nargs) {
  return vm_is_context_value(js, nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t vm_run_in_this_context(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return vm_invalid_arg_type(js, "code", "string");

  ant_value_t code = vm_to_string_arg(js, args[0], "code");
  if (is_err(code)) return code;

  ant_value_t options = nargs > 1 ? args[1] : js_mkundef();
  ant_value_t filename = vm_get_filename(js, options, "evalmachine.<anonymous>", 23);
  if (is_err(filename)) return filename;

  return vm_eval_source(js, code, filename);
}

static ant_value_t vm_run_in_context(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return vm_invalid_arg_type(js, "code", "string");
  if (nargs < 2) return vm_invalid_arg_type(js, "contextifiedObject", "vm.Context");

  ant_value_t code = vm_to_string_arg(js, args[0], "code");
  if (is_err(code)) return code;

  ant_value_t valid = vm_validate_context(js, args[1]);
  if (is_err(valid)) return valid;

  ant_value_t context = args[1];
  ant_value_t options = nargs > 2 ? args[2] : js_mkundef();
  ant_value_t filename = vm_get_filename(js, options, "evalmachine.<anonymous>", 23);
  if (is_err(filename)) return filename;

  char *src = NULL;
  size_t len = 0, cap = 0;
  ant_value_t *values = NULL;
  size_t value_count = 0, value_cap = 0;

  if (!VM_APPEND_LITERAL(&src, &len, &cap, "(function(")) goto oom;

  ant_iter_t iter = js_prop_iter_begin(js, context);
  const char *key = NULL;
  size_t key_len = 0;
  ant_value_t value = js_mkundef();
  while (js_prop_iter_next(&iter, &key, &key_len, &value)) {
    if (value_count == value_cap) {
      size_t next = value_cap ? value_cap * 2 : 8;
      ant_value_t *tmp = (ant_value_t *)realloc(values, next * sizeof(ant_value_t));
      if (!tmp) {
        js_prop_iter_end(&iter);
        goto oom;
      }
      values = tmp;
      value_cap = next;
    }
    if (value_count > 0 && !VM_APPEND_LITERAL(&src, &len, &cap, ",")) {
      js_prop_iter_end(&iter);
      goto oom;
    }
    if (!vm_append(&src, &len, &cap, key, key_len)) {
      js_prop_iter_end(&iter);
      goto oom;
    }
    values[value_count++] = value;
  }
  js_prop_iter_end(&iter);

  if (!VM_APPEND_LITERAL(&src, &len, &cap, "){return eval(")) goto oom;

  ant_offset_t code_len = 0;
  const char *code_str = vm_string_ptr(code, &code_len);
  ant_value_t stringify_arg = js_mkstr(js, code_str, (size_t)code_len);
  ant_value_t quoted = js_json_stringify(js, &stringify_arg, 1);
  if (is_err(quoted)) {
    free(src);
    free(values);
    return quoted;
  }
  ant_offset_t quoted_len = 0;
  const char *quoted_str = vm_string_ptr(quoted, &quoted_len);
  if (!vm_append(&src, &len, &cap, quoted_str, (size_t)quoted_len)) goto oom;
  if (!VM_APPEND_LITERAL(&src, &len, &cap, ");})")) goto oom;

  ant_value_t source = js_mkstr(js, src, len);
  free(src);
  ant_value_t fn = vm_eval_source(js, source, filename);
  if (is_err(fn)) {
    free(values);
    return fn;
  }

  ant_value_t result = sv_vm_call(js->vm, js, fn, js_mkundef(), values, (int)value_count, NULL, false);
  free(values);
  return result;

oom:
  free(src);
  free(values);
  return js_mkerr(js, "oom");
}

static ant_value_t vm_script_run_in_context(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t code = vm_script_code(js, js->this_val);
  if (is_err(code)) return code;
  ant_value_t run_args[] = {
    code,
    nargs > 0 ? args[0] : js_mkundef(),
    nargs > 1 ? args[1] : js_mkundef()
  };
  return vm_run_in_context(js, run_args, 3);
}

static ant_value_t vm_script_run_in_new_context(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t code = vm_script_code(js, js->this_val);
  if (is_err(code)) return code;
  ant_value_t run_args[] = {
    code,
    nargs > 0 ? args[0] : js_mkundef(),
    nargs > 1 ? args[1] : js_mkundef()
  };
  return vm_run_in_new_context(js, run_args, 3);
}

static ant_value_t vm_run_in_new_context(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return vm_invalid_arg_type(js, "code", "string");

  ant_value_t context = nargs > 1 ? args[1] : js_mkobj(js);
  ant_value_t create_args[] = { context };
  ant_value_t created = vm_create_context(js, create_args, 1);
  if (is_err(created)) return created;

  ant_value_t run_args[] = {
    args[0],
    created,
    nargs > 2 ? args[2] : js_mkundef()
  };
  return vm_run_in_context(js, run_args, 3);
}

static bool vm_append(char **buf, size_t *len, size_t *cap, const char *src, size_t src_len) {
  if (*len + src_len + 1 > *cap) {
    size_t next = *cap ? *cap * 2 : 128;
    while (*len + src_len + 1 > next) next *= 2;
    char *tmp = (char *)realloc(*buf, next);
    if (!tmp) return false;
    *buf = tmp;
    *cap = next;
  }
  memcpy(*buf + *len, src, src_len);
  *len += src_len;
  (*buf)[*len] = '\0';
  return true;
}

static ant_value_t vm_compile_function(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return vm_invalid_arg_type(js, "code", "string");

  ant_value_t code = vm_to_string_arg(js, args[0], "code");
  if (is_err(code)) return code;

  ant_value_t params = nargs > 1 ? args[1] : js_mkarr(js);
  if (vtype(params) != T_ARR) return vm_invalid_arg_type(js, "params", "Array");

  char *src = NULL;
  size_t len = 0, cap = 0;
  if (!VM_APPEND_LITERAL(&src, &len, &cap, "(function(")) goto oom;

  ant_offset_t count = js_arr_len(js, params);
  for (ant_offset_t i = 0; i < count; i++) {
    ant_value_t param = js_arr_get(js, params, i);
    if (vtype(param) != T_STR) {
      free(src);
      return vm_invalid_arg_type(js, "params[]", "string");
    }
    ant_offset_t plen = 0;
    const char *p = vm_string_ptr(param, &plen);
    if (i > 0 && !VM_APPEND_LITERAL(&src, &len, &cap, ",")) goto oom;
    if (!vm_append(&src, &len, &cap, p, (size_t)plen)) goto oom;
  }

  if (!VM_APPEND_LITERAL(&src, &len, &cap, "){\n")) goto oom;
  ant_offset_t code_len = 0;
  const char *code_str = vm_string_ptr(code, &code_len);
  if (!vm_append(&src, &len, &cap, code_str, (size_t)code_len)) goto oom;
  if (!VM_APPEND_LITERAL(&src, &len, &cap, "\n})")) goto oom;

  ant_value_t source = js_mkstr(js, src, len);
  free(src);
  return vm_eval_source(js, source, js_mkstr(js, "vm.compileFunction", 18));

oom:
  free(src);
  return js_mkerr(js, "oom");
}

static ant_value_t vm_measure_memory(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t result = js_mkobj(js);
  ant_value_t total = js_mkobj(js);
  ant_value_t range = js_mkarr(js);
  js_arr_push(js, range, js_mknum(0));
  js_arr_push(js, range, js_mknum(0));
  js_set(js, total, "jsMemoryEstimate", js_mknum(0));
  js_set(js, total, "jsMemoryRange", range);
  js_set(js, result, "total", total);
  ant_value_t promise = js_mkpromise(js);
  js_resolve_promise(js, promise, result);
  return promise;
}

static ant_value_t vm_module_ctor(ant_t *js, ant_value_t *args, int nargs) {
  return vm_unsupported(js, "vm.Module");
}

static ant_value_t vm_make_subclass_ctor(
  ant_t *js, ant_value_t proto, ant_value_t parent_proto,
  const char *name, size_t name_len
) {
  if (is_object_type(parent_proto)) js_set_proto_init(proto, parent_proto);
  ant_value_t ctor = js_make_ctor(js, vm_module_ctor, proto, name, name_len);
  js_mark_constructor(ctor, true);
  return ctor;
}

static void vm_init_protos(ant_t *js) {
  if (is_object_type(g_script_proto)) return;

  g_script_proto = js_mkobj(js);
  js_set(js, g_script_proto, "createCachedData", js_mkfun(vm_script_create_cached_data));
  js_set(js, g_script_proto, "runInContext", js_mkfun(vm_script_run_in_context));
  js_set(js, g_script_proto, "runInNewContext", js_mkfun(vm_script_run_in_new_context));
  js_set(js, g_script_proto, "runInThisContext", js_mkfun(vm_script_run_in_this_context));
  js_set_sym(js, g_script_proto, get_toStringTag_sym(), ANT_STRING("Script"));

  g_module_proto = js_mkobj(js);
  js_set_sym(js, g_module_proto, get_toStringTag_sym(), ANT_STRING("Module"));

  g_source_text_module_proto = js_mkobj(js);
  g_synthetic_module_proto = js_mkobj(js);
}

ant_value_t vm_library(ant_t *js) {
  vm_init_protos(js);

  ant_value_t lib = js_mkobj(js);

  ant_value_t script_ctor = js_make_ctor(js, vm_script_ctor, g_script_proto, "Script", 6);
  js_mark_constructor(script_ctor, true);
  ant_value_t module_ctor = js_make_ctor(js, vm_module_ctor, g_module_proto, "Module", 6);
  js_mark_constructor(module_ctor, true);
  ant_value_t source_text_module_ctor = vm_make_subclass_ctor(
    js, g_source_text_module_proto, g_module_proto, "SourceTextModule", 16
  );
  ant_value_t synthetic_module_ctor = vm_make_subclass_ctor(
    js, g_synthetic_module_proto, g_module_proto, "SyntheticModule", 15
  );

  ant_value_t constants = js_mkobj(js);
  js_set(js, constants, "USE_MAIN_CONTEXT_DEFAULT_LOADER", js_mksym_well_known(js, "vm.constants.USE_MAIN_CONTEXT_DEFAULT_LOADER"));
  js_set(js, constants, "DONT_CONTEXTIFY", js_mksym_well_known(js, "vm.constants.DONT_CONTEXTIFY"));

  js_set(js, lib, "Script", script_ctor);
  js_set(js, lib, "Module", module_ctor);
  js_set(js, lib, "SourceTextModule", source_text_module_ctor);
  js_set(js, lib, "SyntheticModule", synthetic_module_ctor);
  js_set(js, lib, "compileFunction", js_mkfun(vm_compile_function));
  js_set(js, lib, "constants", constants);
  js_set(js, lib, "createContext", js_mkfun(vm_create_context));
  js_set(js, lib, "isContext", js_mkfun(vm_is_context));
  js_set(js, lib, "measureMemory", js_mkfun(vm_measure_memory));
  js_set(js, lib, "runInContext", js_mkfun(vm_run_in_context));
  js_set(js, lib, "runInNewContext", js_mkfun(vm_run_in_new_context));
  js_set(js, lib, "runInThisContext", js_mkfun(vm_run_in_this_context));
  js_set_sym(js, lib, get_toStringTag_sym(), ANT_STRING("vm"));

  return lib;
}
