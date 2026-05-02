#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "errors.h"
#include "internal.h"

#include "ptr.h"
#include "gc/roots.h"
#include "modules/buffer.h"
#include "modules/wasi.h"
#include "wasm_c_api.h"
#include "wasm_export.h"

#define WASM_MAX_PARAMS 32
#define WASM_MAX_ARGS   256

enum {
  WASI_INSTANCE_TAG = 0x57415349u, // WASI
  WASI_FUNC_TAG     = 0x57415346u, // WASF
};

typedef struct {
  uint8_t *binary;
  wasm_module_t module;
  wasm_module_inst_t inst;
  wasm_exec_env_t exec_env;
} wasi_instance_handle_t;

typedef struct {
  ant_t *js;
  wasm_module_inst_t inst;
  wasm_exec_env_t exec_env;
  wasm_function_inst_t func;
} wasi_func_env_t;

enum {
  WASM_SECTION_IMPORT = 2,
  WASM_SECTION_EXPORT = 7,
};

static bool wasm_read_u32_leb(const uint8_t *buf, size_t len, size_t *offset, uint32_t *out) {
  uint32_t value = 0;
  uint32_t shift = 0;

  while (*offset < len && shift < 35) {
    uint8_t byte = buf[(*offset)++];
    value |= (uint32_t)(byte & 0x7f) << shift;
    
    if ((byte & 0x80) == 0) {
      *out = value;
      return true;
    }
    
    shift += 7;
  }

  return false;
}

static bool wasm_read_name(const uint8_t *buf, size_t len, size_t *offset, const uint8_t **data, uint32_t *name_len) {
  uint32_t size = 0;
  
  if (
    !wasm_read_u32_leb(buf, len, offset, &size) 
    || *offset + size > len
  ) return false;
  
  *data = buf + *offset;
  *name_len = size;
  *offset += size;
  
  return true;
}

static bool wasm_name_equals(const uint8_t *data, uint32_t len, const char *expected) {
  size_t expected_len = strlen(expected);
  return len == expected_len && memcmp(data, expected, expected_len) == 0;
}

static bool wasi_bytes_have_wasi_imports(const uint8_t *wasm_bytes, size_t wasm_len) {
  size_t offset = 8;

  if (!wasm_bytes || wasm_len < 8 || memcmp(wasm_bytes, "\0asm\x01\0\0\0", 8) != 0)
    return false;

  while (offset < wasm_len) {
    uint8_t section_id = wasm_bytes[offset++];
    uint32_t section_size = 0;
    const uint8_t *section;
    size_t section_offset = 0;
    size_t section_len;
    
    if (
      !wasm_read_u32_leb(wasm_bytes, wasm_len, &offset, &section_size)
      || offset + section_size > wasm_len
    ) return false;
    
    section = wasm_bytes + offset;
    section_len = section_size;
    offset += section_size;

    if (section_id != WASM_SECTION_IMPORT) continue;
    if (!wasm_read_u32_leb(section, section_size, &section_offset, &section_size))
      return false;
    
    for (uint32_t i = 0; i < section_size; i++) {
      const uint8_t *module_name = NULL;
      const uint8_t *field_name = NULL;
      uint32_t module_name_len = 0;
      uint32_t field_name_len = 0;
      uint32_t discard = 0;
      uint32_t flags = 0;
      
      if (!wasm_read_name(section, section_len, &section_offset, &module_name, &module_name_len)
          || !wasm_read_name(section, section_len, &section_offset, &field_name, &field_name_len)
          || section_offset >= section_len)
        return false;
      
      if (wasm_name_equals(module_name, module_name_len, "wasi_snapshot_preview1"))
        return true;
      
      switch (section[section_offset++]) {
        case 0:
          if (!wasm_read_u32_leb(section, section_len, &section_offset, &discard))
            return false;
          break;
        case 1: {
          if (section_offset >= section_len) return false;
          section_offset++;
          if (!wasm_read_u32_leb(section, section_len, &section_offset, &flags)
              || !wasm_read_u32_leb(section, section_len, &section_offset, &discard))
            return false;
          if ((flags & 0x1) != 0
              && !wasm_read_u32_leb(section, section_len, &section_offset, &discard))
            return false;
          break;
        }
        case 2:
          if (!wasm_read_u32_leb(section, section_len, &section_offset, &flags)
              || !wasm_read_u32_leb(section, section_len, &section_offset, &discard))
            return false;
          if ((flags & 0x1) != 0
              && !wasm_read_u32_leb(section, section_len, &section_offset, &discard))
            return false;
          break;
        case 3:
          if (section_offset + 2 > section_len) return false;
          section_offset += 2;
          break;
        default:
          return false;
      }
    }
  }

  return false;
}

static bool wasi_bytes_have_command_or_reactor_entry(const uint8_t *wasm_bytes, size_t wasm_len) {
  size_t offset = 8;

  if (!wasm_bytes || wasm_len < 8 || memcmp(wasm_bytes, "\0asm\x01\0\0\0", 8) != 0)
    return false;

  while (offset < wasm_len) {
    uint8_t section_id = wasm_bytes[offset++];
    uint32_t section_size = 0;
    const uint8_t *section;
    size_t section_offset = 0;
    size_t section_len;

    if (!wasm_read_u32_leb(wasm_bytes, wasm_len, &offset, &section_size)
        || offset + section_size > wasm_len)
      return false;

    section = wasm_bytes + offset;
    section_len = section_size;
    offset += section_size;

    if (section_id != WASM_SECTION_EXPORT) continue;
    if (!wasm_read_u32_leb(section, section_len, &section_offset, &section_size))
      return false;

    for (uint32_t i = 0; i < section_size; i++) {
      const uint8_t *name = NULL;
      uint32_t name_len = 0;
      uint32_t discard = 0;

      if (!wasm_read_name(section, section_len, &section_offset, &name, &name_len)
          || section_offset >= section_len)
        return false;

      if (section[section_offset++] != 0) {
        if (!wasm_read_u32_leb(section, section_len, &section_offset, &discard))
          return false;
        continue;
      }

      if (!wasm_read_u32_leb(section, section_len, &section_offset, &discard))
        return false;

      if (wasm_name_equals(name, name_len, "_start")
          || wasm_name_equals(name, name_len, "_initialize"))
        return true;
    }
  }

  return false;
}

static inline bool wasi_is_proc_exit_exception(const char *exception) {
  return exception != NULL && strstr(exception, "wasi proc exit") != NULL;
}

static ant_value_t wasi_handle_proc_exit(wasm_module_inst_t inst) {
  uint32_t exit_code = wasm_runtime_get_wasi_exit_code(inst);
  wasm_runtime_clear_exception(inst);
  if (exit_code != 0) exit((int)exit_code);
  return js_mkundef();
}

static ant_value_t wasi_exported_func_call(ant_t *js, ant_value_t *args, int nargs) {
  if (!js_check_native_tag(js->current_func, WASI_FUNC_TAG))
    return js_mkerr(js, "Invalid WASI function");
  wasi_func_env_t *env = (wasi_func_env_t *)js_get_native(js->current_func, WASI_FUNC_TAG);

  uint32_t param_count = wasm_func_get_param_count(env->func, env->inst);
  uint32_t result_count = wasm_func_get_result_count(env->func, env->inst);
  if (param_count > WASM_MAX_PARAMS) param_count = WASM_MAX_PARAMS;

  uint32_t wasm_argv[WASM_MAX_PARAMS];
  memset(wasm_argv, 0, sizeof(wasm_argv));

  for (int i = 0; i < nargs && (uint32_t)i < param_count; i++) {
    wasm_argv[i] = (uint32_t)js_getnum(args[i]);
  }

  if (!wasm_runtime_call_wasm(env->exec_env, env->func, param_count, wasm_argv)) {
    const char *exception = wasm_runtime_get_exception(env->inst);
    if (wasi_is_proc_exit_exception(exception)) return wasi_handle_proc_exit(env->inst);
    return js_mkerr(js, "%s", exception ? exception : "WASI function call failed");
  }

  if (result_count == 0) return js_mkundef();
  return js_mknum((double)(int32_t)wasm_argv[0]);
}

static void wasi_func_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  free(js_get_native(value, WASI_FUNC_TAG));
  js_clear_native(value, WASI_FUNC_TAG);
}

static void wasi_instance_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  wasi_instance_handle_t *handle = (wasi_instance_handle_t *)js_get_native(value, WASI_INSTANCE_TAG);

  if (!handle) return;
  if (handle->exec_env) wasm_runtime_destroy_exec_env(handle->exec_env);
  if (handle->inst) wasm_runtime_deinstantiate(handle->inst);
  js_clear_native(value, WASI_INSTANCE_TAG);
  if (handle->module) wasm_runtime_unload(handle->module);

  free(handle->binary);
  free(handle);
}

bool wasi_module_has_wasi_imports(void *c_api_module) {
  wasm_importtype_vec_t import_types = {0};
  wasm_module_imports((wasm_module_t *)c_api_module, &import_types);

  bool has_wasi = false;
  for (size_t i = 0; i < import_types.size; i++) {
  const wasm_name_t *mod = wasm_importtype_module(import_types.data[i]);
  if (mod && mod->size >= 22 && memcmp(mod->data, "wasi_snapshot_preview1", 22) == 0) {
    has_wasi = true;
    break;
  }}

  wasm_importtype_vec_delete(&import_types);
  return has_wasi;
}

bool wasi_module_is_command_or_reactor(void *c_api_module) {
  wasm_exporttype_vec_t export_types = {0};
  bool has_entry = false;

  wasm_module_exports((wasm_module_t *)c_api_module, &export_types);

  for (size_t i = 0; i < export_types.size; i++) {
    const wasm_name_t *name = wasm_exporttype_name(export_types.data[i]);
    const wasm_externtype_t *type = wasm_exporttype_type(export_types.data[i]);
    const wasm_functype_t *func_type;
    size_t name_len;

    if (!name || wasm_externtype_kind(type) != WASM_EXTERN_FUNC) continue;
    name_len = name->size;
    if (name_len > 0 && name->data[name_len - 1] == '\0') name_len--;

    if (!((name_len == 6 && memcmp(name->data, "_start", 6) == 0)
          || (name_len == 11 && memcmp(name->data, "_initialize", 11) == 0)))
      continue;

    func_type = wasm_externtype_as_functype_const(type);
    if (!func_type) continue;
    if (wasm_functype_params(func_type)->size == 0 && wasm_functype_results(func_type)->size == 0) {
      has_entry = true;
      break;
    }
  }

  wasm_exporttype_vec_delete(&export_types);
  return has_entry;
}

bool wasi_bytes_need_wasi_command_warning_suppression(const uint8_t *wasm_bytes, size_t wasm_len) {
  return wasi_bytes_have_wasi_imports(wasm_bytes, wasm_len)
      && !wasi_bytes_have_command_or_reactor_entry(wasm_bytes, wasm_len);
}

static void wasi_bind_func_export(
  ant_t *js, ant_value_t exports_obj, ant_value_t instance_obj,
  wasm_module_inst_t inst, wasm_exec_env_t exec_env, const char *name
) {
  wasm_function_inst_t func = wasm_runtime_lookup_function(inst, name);
  if (!func) return;

  wasi_func_env_t *fenv = calloc(1, sizeof(*fenv));
  if (!fenv) return;
  fenv->js = js;
  fenv->inst = inst;
  fenv->exec_env = exec_env;
  fenv->func = func;

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t obj = js_mkobj(js);
  GC_ROOT_PIN(js, obj);
  
  js_set_slot(obj, SLOT_CFUNC, js_mkfun(wasi_exported_func_call));
  js_set_native(obj, fenv, WASI_FUNC_TAG);
  js_set_slot_wb(js, obj, SLOT_ENTRIES, instance_obj);
  js_set_finalizer(obj, wasi_func_finalize);
  js_set(js, exports_obj, name, js_obj_to_func(obj));
  GC_ROOT_RESTORE(js, root_mark);
}

static void wasi_bind_memory_export(
  ant_t *js, ant_value_t exports_obj, ant_value_t instance_obj,
  wasm_module_inst_t inst, const char *name
) {
  void *mem_data = wasm_runtime_addr_app_to_native(inst, 0);
  if (!mem_data) return;

  wasm_memory_inst_t mem = wasm_runtime_get_default_memory(inst);
  uint64_t pages = mem ? wasm_memory_get_cur_page_count(mem) : 0;
  size_t mem_size = (size_t)(pages * 65536);

  ArrayBufferData *buffer = calloc(1, sizeof(ArrayBufferData));
  if (!buffer) return;

  buffer->data = (uint8_t *)mem_data;
  buffer->length = mem_size;
  buffer->capacity = mem_size;
  buffer->ref_count = 1;

  ant_value_t ab = create_arraybuffer_obj(js, buffer);
  ant_value_t mem_obj = js_mkobj(js);
  js_set_slot_wb(js, mem_obj, SLOT_DATA, ab);
  js_set_slot_wb(js, mem_obj, SLOT_CTOR, instance_obj);
  js_set(js, exports_obj, name, mem_obj);
}

ant_value_t wasi_instantiate(
  ant_t *js, const uint8_t *wasm_bytes, size_t wasm_len,
  ant_value_t module_obj, ant_value_t wasi_opts
) {
  char error_buf[128] = {0};
  uint8_t *bin_copy = malloc(wasm_len);

  if (!bin_copy) return js_mkerr(js, "out of memory");
  memcpy(bin_copy, wasm_bytes, wasm_len);

  wasm_module_t rt_module = wasm_runtime_load(bin_copy, (uint32_t)wasm_len, error_buf, sizeof(error_buf));
  if (!rt_module) {
    free(bin_copy);
    return js_mkerr(js, "%s", error_buf[0] ? error_buf : "Failed to load WASI module");
  }

  const char *dirs[] = { "." };
  ant_value_t args_val = is_object_type(wasi_opts)
    ? js_get(js, wasi_opts, "args")
    : js_mkundef();

  int argc = vtype(args_val) == T_ARR ? (int)js_arr_len(js, args_val) : 0;
  if (argc < 1) argc = 1;
  if (argc > WASM_MAX_ARGS) argc = WASM_MAX_ARGS;

  char *argv[argc];
  if (vtype(args_val) == T_ARR) {
    for (int i = 0; i < argc; i++) {
      char *s = js_getstr(js, js_arr_get(js, args_val, (ant_offset_t)i), NULL);
      argv[i] = s ? s : (char *)"";
    }
  } else argv[0] = (char *)"wasi";

  wasm_runtime_set_wasi_args(rt_module, dirs, 1, NULL, 0, NULL, 0, argv, argc);
  wasm_module_inst_t inst = wasm_runtime_instantiate(rt_module, 512 * 1024, 256 * 1024, error_buf, sizeof(error_buf));

  if (!inst) {
    wasm_runtime_unload(rt_module);
    free(bin_copy);
    return js_mkerr(js, "%s", error_buf[0] ? error_buf : "Failed to instantiate WASI module");
  }

  wasm_exec_env_t exec_env = wasm_runtime_create_exec_env(inst, 512 * 1024);
  if (!exec_env) {
    wasm_runtime_deinstantiate(inst);
    wasm_runtime_unload(rt_module);
    free(bin_copy);
    return js_mkerr(js, "Failed to create WASI exec env");
  }

  wasi_instance_handle_t *handle = calloc(1, sizeof(*handle));
  if (!handle) {
    wasm_runtime_destroy_exec_env(exec_env);
    wasm_runtime_deinstantiate(inst);
    wasm_runtime_unload(rt_module);
    free(bin_copy);
    return js_mkerr(js, "out of memory");
  }

  handle->binary = bin_copy;
  handle->module = rt_module;
  handle->inst = inst;
  handle->exec_env = exec_env;

  ant_value_t instance_obj = js_mkobj(js);
  ant_value_t exports_obj = js_mkobj(js);

  js_set_native(instance_obj, handle, WASI_INSTANCE_TAG);
  js_set_slot_wb(js, instance_obj, SLOT_CTOR, module_obj);
  js_set_finalizer(instance_obj, wasi_instance_finalize);

  int32_t export_count = wasm_runtime_get_export_count(rt_module);
  for (int32_t i = 0; i < export_count; i++) {
    wasm_export_t export_info;
    wasm_runtime_get_export_type(rt_module, i, &export_info);
    
    if (export_info.kind == WASM_IMPORT_EXPORT_KIND_FUNC)
      wasi_bind_func_export(js, exports_obj, instance_obj, inst, exec_env, export_info.name);
    else if (export_info.kind == WASM_IMPORT_EXPORT_KIND_MEMORY)
      wasi_bind_memory_export(js, exports_obj, instance_obj, inst, export_info.name);
  }

  js_set_slot_wb(js, instance_obj, SLOT_ENTRIES, exports_obj);
  js_set(js, instance_obj, "exports", exports_obj);

  return instance_obj;
}
