#include "wasm_internal.h"

void gc_mark_wasm(ant_t *js, gc_mark_fn mark) {
  for (wasm_import_func_env_t *env = js->wasm_import_envs; env; env = env->next) {
    mark(js, env->fn);
    mark(js, env->owner);
  }
}

void wasm_isolate_destroy(ant_t *js) {
  while (js->wasm_import_envs) Ant_List_Remove(js->wasm_import_envs);
  while (js->wasm_memory_views) Ant_List_Remove(js->wasm_memory_views);
  ant_ptr_pair_free(&js->wasm_instance_owners);
}

static ant_value_t wasm_make_proto(ant_t *js, const char *tag) {
  ant_value_t proto = Ant_Object_Create(js);
  js_set_tostringtag(js, proto, js_mkstr(js, tag, strlen(tag)));
  return proto;
}

void init_wasm_module(ant_t *js) {
  if (!ensure_wasm_engine()) return;

  ant_value_t ns = Ant_Object_Create(js);
  js_set_tostringtag(js, ns, Ant_String_FromLiteral(js, "WebAssembly"));

  js->builtins.wasm_module_proto = wasm_make_proto(js, "WebAssembly.Module");
  js->builtins.wasm_instance_proto = wasm_make_proto(js, "WebAssembly.Instance");
  js->builtins.wasm_global_proto = wasm_make_proto(js, "WebAssembly.Global");
  js->builtins.wasm_memory_proto = wasm_make_proto(js, "WebAssembly.Memory");
  js->builtins.wasm_table_proto = wasm_make_proto(js, "WebAssembly.Table");
  js->builtins.wasm_tag_proto = wasm_make_proto(js, "WebAssembly.Tag");
  js->builtins.wasm_exception_proto = wasm_make_proto(js, "WebAssembly.Exception");

  js_set(js, js->builtins.wasm_global_proto, "valueOf", Ant_Function_Create(js_wasm_global_value_of));
  js_set_getter_desc(js, js->builtins.wasm_global_proto, "value", 5, Ant_Function_Create(js_wasm_global_value_getter), JS_DESC_E | JS_DESC_C);
  js_set_setter_desc(js, js->builtins.wasm_global_proto, "value", 5, Ant_Function_Create(js_wasm_global_value_setter), JS_DESC_E | JS_DESC_C);

  js_set_getter_desc(js, js->builtins.wasm_instance_proto, "exports", 7, Ant_Function_Create(js_wasm_instance_exports_getter), JS_DESC_E | JS_DESC_C);
  js_set_getter_desc(js, js->builtins.wasm_memory_proto, "buffer", 6, Ant_Function_Create(js_wasm_memory_buffer_getter), JS_DESC_E | JS_DESC_C);
  js_set(js, js->builtins.wasm_memory_proto, "grow", Ant_Function_CreateWithArity(js_wasm_memory_grow, 1));
  js_set(js, js->builtins.wasm_memory_proto, "toFixedLengthBuffer", Ant_Function_Create(js_wasm_memory_to_fixed_length_buffer));

  js_set_getter_desc(js, js->builtins.wasm_table_proto, "length", 6, Ant_Function_Create(js_wasm_table_length_getter), JS_DESC_E | JS_DESC_C);
  js_set(js, js->builtins.wasm_table_proto, "get", Ant_Function_CreateWithArity(js_wasm_table_get, 1));
  js_set(js, js->builtins.wasm_table_proto, "set", Ant_Function_CreateWithArity(js_wasm_table_set, 1));
  js_set(js, js->builtins.wasm_table_proto, "grow", Ant_Function_CreateWithArity(js_wasm_table_grow, 1));

  js_set(js, js->builtins.wasm_exception_proto, "getArg", Ant_Function_CreateWithArity(js_wasm_exception_get_arg, 2));
  js_set(js, js->builtins.wasm_exception_proto, "is", Ant_Function_CreateWithArity(js_wasm_exception_is, 1));
  js_set_getter_desc(js, js->builtins.wasm_exception_proto, "stack", 5, Ant_Function_Create(js_wasm_exception_stack_getter), JS_DESC_E | JS_DESC_C);

  ant_value_t module_ctor = Ant_Function_CreateIntrinsicConstructor(js, js_wasm_module_ctor, js->builtins.wasm_module_proto, "Module", 1);
  ant_value_t instance_ctor = Ant_Function_CreateIntrinsicConstructor(js, js_wasm_instance_ctor, js->builtins.wasm_instance_proto, "Instance", 1);
  ant_value_t global_ctor = Ant_Function_CreateIntrinsicConstructor(js, js_wasm_global_ctor, js->builtins.wasm_global_proto, "Global", 1);
  ant_value_t memory_ctor = Ant_Function_CreateIntrinsicConstructor(js, js_wasm_memory_ctor, js->builtins.wasm_memory_proto, "Memory", 1);
  ant_value_t table_ctor = Ant_Function_CreateIntrinsicConstructor(js, js_wasm_table_ctor, js->builtins.wasm_table_proto, "Table", 1);
  ant_value_t tag_ctor = Ant_Function_CreateIntrinsicConstructor(js, js_wasm_tag_ctor, js->builtins.wasm_tag_proto, "Tag", 1);
  ant_value_t exception_ctor = Ant_Function_CreateIntrinsicConstructor(js, js_wasm_exception_ctor, js->builtins.wasm_exception_proto, "Exception", 2);

  ant_value_t compile_error_ctor = Ant_Error_CreateConstructor(js, "CompileError", &js->builtins.wasm_compileerror_proto);
  ant_value_t link_error_ctor = Ant_Error_CreateConstructor(js, "LinkError", &js->builtins.wasm_linkerror_proto);
  ant_value_t runtime_error_ctor = Ant_Error_CreateConstructor(js, "RuntimeError", &js->builtins.wasm_runtimeerror_proto);

  js_set(js, module_ctor, "imports", Ant_Function_CreateWithArity(js_wasm_module_imports, 1));
  js_set(js, module_ctor, "exports", Ant_Function_CreateWithArity(js_wasm_module_exports, 1));
  js_set(js, module_ctor, "customSections", Ant_Function_CreateWithArity(js_wasm_module_custom_sections, 2));

  js_set(js, ns, "validate", Ant_Function_CreateWithArity(js_wasm_validate, 1));
  js_set(js, ns, "compile", Ant_Function_CreateWithArity(js_wasm_compile, 1));
  js_set(js, ns, "instantiate", Ant_Function_CreateWithArity(js_wasm_instantiate, 1));
  js_set(js, ns, "compileStreaming", Ant_Function_CreateWithArity(js_wasm_compile_streaming, 1));
  js_set(js, ns, "instantiateStreaming", Ant_Function_CreateWithArity(js_wasm_instantiate_streaming, 1));

  defhidden(js, ns, "Module", 6, module_ctor);
  defhidden(js, ns, "Instance", 8, instance_ctor);
  defhidden(js, ns, "Global", 6, global_ctor);
  defhidden(js, ns, "Memory", 6, memory_ctor);
  defhidden(js, ns, "Table", 5, table_ctor);
  defhidden(js, ns, "Tag", 3, tag_ctor);
  defhidden(js, ns, "Exception", 9, exception_ctor);
  defhidden(js, ns, "CompileError", 12, compile_error_ctor);
  defhidden(js, ns, "LinkError", 9, link_error_ctor);
  defhidden(js, ns, "RuntimeError", 12, runtime_error_ctor);

  ant_value_t js_tag_kinds = Ant_Array_Create(js);
  js_arr_push(js, js_tag_kinds, Ant_Value_FromNumber((double)WASM_EXTERNREF));
  defhidden(js, ns, "JSTag", 5, wasm_make_tag(js, js->builtins.wasm_tag_proto, js_tag_kinds, true));
  js_set_global_builtin(js, "WebAssembly", ns);
}
