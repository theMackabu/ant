#include "ant/values.h"
#include "regex_internal.h"

static_assert(sizeof(PCRE2_SIZE) == sizeof(size_t), "realm ovector element");
static_assert(
  sizeof(((ant_t *)0)->mutable_roots.regexp_static_values) / sizeof(ant_value_t) == REGEXP_STATIC_VALUE_COUNT,
  "regexp_static_values must hold every RegExp legacy static"
);

void init_regex_module(ant_t *js) {
  if (!js->regex_state) js->regex_state = calloc(1, sizeof(*js->regex_state));
  
  ant_value_t glob = js->global;
  ant_value_t object_proto = js->sym.object_proto;

  ant_value_t regexp_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(regexp_proto, object_proto);
  js->builtins.regexp_proto = regexp_proto;
  
  defmethod(js, regexp_proto, "test", 4, Ant_Function_CreateWithArity(builtin_regexp_test, 1));
  defmethod(js, regexp_proto, "exec", 4, Ant_Function_CreateWithArity(builtin_regexp_exec, 1));
  defmethod(js, regexp_proto, "toString", 8, Ant_Function_Create(builtin_regexp_toString));

  js_set_getter_desc(js, regexp_proto, "source", 6, Ant_Function_Create(builtin_regexp_source_getter), JS_DESC_C);
#define REGEXP_FLAG(id, name, ch) \
  js_set_getter_desc(js, regexp_proto, #name, sizeof(#name) - 1, Ant_Function_Create(builtin_regexp_get_##name), JS_DESC_C);
#include "modules/regex_flags.h"

  defmethod_sym(js, regexp_proto, js->sym.split_sym, Ant_Function_CreateWithArity(builtin_regexp_symbol_split, 2));
  defmethod_sym(js, regexp_proto, js->sym.match_sym, Ant_Function_CreateWithArity(builtin_regexp_symbol_match, 1));
  defmethod_sym(js, regexp_proto, js->sym.matchAll_sym, Ant_Function_CreateWithArity(builtin_regexp_symbol_matchAll, 1));

  js->builtins.regexp_matchall_iter_proto_val = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.regexp_matchall_iter_proto_val, js->sym.iterator_proto);
  defmethod(js, js->builtins.regexp_matchall_iter_proto_val, "next", 4, Ant_Function_Create(regexp_matchall_next));
  mkprop(js, js->builtins.regexp_matchall_iter_proto_val, js->sym.toStringTag_sym, Ant_String_FromLiteral(js, "RegExp String Iterator"), ANT_PROP_ATTR_CONFIGURABLE);
  
  defmethod_sym(js, regexp_proto, js->sym.replace_sym, Ant_Function_CreateWithArity(builtin_regexp_symbol_replace, 2));
  defmethod_sym(js, regexp_proto, js->sym.search_sym, Ant_Function_CreateWithArity(builtin_regexp_symbol_search, 1));
  js_set_getter_desc(js, regexp_proto, "flags", 5, Ant_Function_Create(builtin_regexp_flags_getter), JS_DESC_C);
  defmethod(js, regexp_proto, "compile", 7, Ant_Function_CreateWithArity(builtin_regexp_compile, 2));

  ant_value_t regexp_func = Ant_Function_CreateIntrinsicConstructor(js, builtin_RegExp, regexp_proto, "RegExp", 2);
  ant_value_t regexp_ctor = js_as_obj(regexp_func);
  js_define_species_getter(js, regexp_func);
  js->builtins.regexp_ctor_value = regexp_func;

  ant_value_t empty = js_mkstr_permanent(js, "", 0);
  js->builtins.regexp_empty_string = empty;
  js->mutable_roots.regexp_static_subject = empty;
  for (size_t i = 0; i < sizeof(js->mutable_roots.regexp_static_values) / sizeof(js->mutable_roots.regexp_static_values[0]); i++)
    js->mutable_roots.regexp_static_values[i] = empty;

  static const struct {
    const char *name, *alias;
    ant_cfunc_meta_t get, set, alias_get, alias_set;
  } statics[] = {
#define REGEXP_STATIC(id, name, alias) { name, alias, \
    { regexp_static_get_##id, "get " name, 0, 0 }, { regexp_static_set_##id, "set " name, 1, 0 }, \
    { regexp_static_get_##id, "get " alias, 0, 0 }, { regexp_static_set_##id, "set " alias, 1, 0 } },
#include "modules/regex_statics.h"
  };
  for (size_t i = 0; i < sizeof(statics) / sizeof(statics[0]); i++) {
    js_set_accessor_desc(
      js, regexp_ctor, statics[i].name, strlen(statics[i].name),
      js_mkfun_meta(&statics[i].get), js_mkfun_meta(&statics[i].set), JS_DESC_C
    );
    if (statics[i].alias[0]) js_set_accessor_desc(
      js, regexp_ctor, statics[i].alias, strlen(statics[i].alias),
      js_mkfun_meta(&statics[i].alias_get), js_mkfun_meta(&statics[i].alias_set), JS_DESC_C
    );
  }
  defmethod(js, regexp_ctor, "escape", 6, Ant_Function_CreateWithArity(builtin_regexp_escape, 1));
  js_set_global_builtin(js, "RegExp", regexp_func);

  ant_value_t string_ctor = js_get(js, glob, "String");
  ant_value_t string_proto = js_get(js, string_ctor, "prototype");
  
  defmethod(js, string_proto, "search", 6, Ant_Function_CreateWithArity(builtin_string_search, 1));
  defmethod(js, string_proto, "match", 5, Ant_Function_CreateWithArity(builtin_string_match, 1));
  defmethod(js, string_proto, "matchAll", 8, Ant_Function_CreateWithArity(builtin_string_matchAll, 1));
  defmethod(js, string_proto, "replace", 7, Ant_Function_CreateWithArity(builtin_string_replace, 2));
  defmethod(js, string_proto, "replaceAll", 10, Ant_Function_CreateWithArity(builtin_string_replaceAll, 2));
  
  js->regexp.replace_written = false;
  js->regexp.write_guard_armed = true;

  js->regexp.proto_ptr = js_obj_ptr(regexp_proto);
  js->regexp.ctor_ptr = js_obj_ptr(js_func_obj(regexp_func));
  js->regexp.protector_invalid = false;
  ant_regexp_protector_holder_mark(regexp_proto);
  ant_regexp_protector_holder_mark(js_func_obj(regexp_func));
}

void gc_age_regex_cache(ant_t *js, bool minor) {
  ant_regex_state_t *state = js->regex_state;
  if (!state || minor) return;
  ant_gc_shapes_mark(js, state->instance_shape.base);
  regexp_sites_clear(state);
  compiled_regex_cache_rotate(state);
  regexp_code_units_gc(js, state);
}

void cleanup_regex_module(ant_t *js) {
  ant_regex_state_t *state = js->regex_state;
  if (!state) return;

  regexp_sites_clear(state);
  compiled_regex_table_clear(&state->compiled[0]);
  compiled_regex_table_clear(&state->compiled[1]);

  ant_shape_release(state->instance_shape.base);
  ant_shape_release(state->instance_shape.shape);
  ant_shape_release(state->result_shape.shape);
  ant_shape_release(state->result_indices_shape.shape);
  free(state->out_buf);
  pcre2_jit_stack_free(state->jit_stack);
  pcre2_match_context_free(state->match_ctx);
  free(state);
  js->regex_state = NULL;
}
