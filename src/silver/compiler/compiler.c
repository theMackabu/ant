#include "compiler_internal.h"

sv_func_t *sv_compile(
  ant_t *js, sv_ast_t *program, sv_compile_mode_t mode,
  const char *source, ant_offset_t source_len, ant_value_t eval_env
) {
  if (!program || program->type != N_PROGRAM) return NULL;
  if (mode != SV_COMPILE_EVAL_FUNCTION && ast_contains_lexical_new_target(program)) {
    js_mkerr_typed(js, JS_ERR_SYNTAX, "new.target is only valid in functions");
    return NULL;
  }

  if (sv_compile_mode_is_eval(mode)) {
    const sv_ast_t *offender = NULL;
    for (int i = 0; i < program->args.count; i++)
      if (ast_contains_direct_suspend(program->args.items[i], &offender)) break;

    if (offender) {
      js_mkerr_typed(js, JS_ERR_SYNTAX,
        offender->type == N_AWAIT ? "await is only valid in async functions and the top level bodies of modules" :
        offender->type == N_FOR_AWAIT_OF ? "Unexpected reserved word" :
        "yield is only valid in generator functions");
      return NULL;
    }
  }
  
  if (sv_debug_unlikely(SV_DEBUG_COMPILE)) fprintf(
    stderr, "[compile] start kind=program mode=%d len=%u body=%d strict=%d\n",
    (int)mode, (unsigned)source_len,
    program->args.count, (program->flags & FN_PARSE_STRICT) != 0 ? 1 : 0
  );
  
  static const char *k_top_name_script = "<script>";
  static const char *k_top_name_module = "<module>";
  static const char *k_top_name_eval   = "<eval>";
  static const char *k_top_name_repl   = "<repl>";
  const        char *top_name          = k_top_name_script;
  
  switch (mode) {
    case SV_COMPILE_MODULE: top_name = k_top_name_module; break;
    case SV_COMPILE_EVAL_FUNCTION:
    case SV_COMPILE_EVAL:   top_name = k_top_name_eval; break;
    case SV_COMPILE_REPL:   top_name = k_top_name_repl; break;
    case SV_COMPILE_SCRIPT:
    default:                top_name = k_top_name_script; break;
  }

  sv_ast_t top_fn;
  memset(&top_fn, 0, sizeof(top_fn));
  
  top_fn.type = N_FUNC;
  top_fn.str = top_name;
  top_fn.len = (uint32_t)strlen(top_name);
  top_fn.src_off = 0;
  top_fn.src_end = (source_len > 0) ? (uint32_t)source_len : 0;
  top_fn.body = sv_ast_new(js, N_BLOCK);
  top_fn.body->args = program->args;
  top_fn.flags |= program->flags & (FN_SCOPE_FACTS | FN_DIRECT_EVAL | FN_OWN_YIELD);
  
  if (
    sv_compile_mode_is_eval(mode) && 
    (ast_references_new_target(program) || 
    ast_contains_direct_eval(program))
  ) top_fn.flags |= FN_USES_NEW_TARGET;

  sv_code_unit_t *unit = sv_compile_mode_is_eval(mode) || mode == SV_COMPILE_REPL ? sv_code_unit_begin(js) : NULL;
  const char *text = copy_source_text(js, source, source_len);
  
  if (!text && source && source_len > 0) {
    sv_code_unit_finish(unit);
    sv_code_unit_unpin(unit);
    return NULL;
  }

  sv_compiler_t root;
  sv_compile_ctx_init_root(
    &root, js, js->filename, text,
    source_len, mode,
    (program->flags & FN_PARSE_STRICT) != 0,  NULL
  );
  
  if (sv_compile_mode_is_eval(mode)) {
    root.eval_shadows_undefined = sv_eval_env_shadows_global(js, eval_env, "undefined", 9);
    root.eval_shadows_global_this = sv_eval_env_shadows_global(js, eval_env, "globalThis", 10);
  }
  
  root.line_table = sv_compile_ctx_build_line_table(root.source, source_len);
  sv_func_t *func = compile_function_body(&root, &top_fn, mode);
  sv_compile_ctx_free_line_table(root.line_table);
  sv_code_unit_finish(unit);
  
  if (sv_debug_unlikely(SV_DEBUG_COMPILE)) fprintf(
    stderr, "[compile] end kind=program mode=%d thrown=%d func=%p\n",
    (int)mode, Ant_Exception_Pending(js) ? 1 : 0, (void *)func
  );
  
  if (Ant_Exception_Pending(js) || !func) {
    sv_code_unit_unpin(unit);
    return NULL;
  }
  return func;
}

sv_func_t *sv_compile_function(ant_t *js, const sv_function_parts_t *parts) {
  if (sv_debug_unlikely(SV_DEBUG_COMPILE)) fprintf(
    stderr, "[compile] start kind=function len=%u async=%d generator=%d\n",
    (unsigned)parts->len, parts->is_async ? 1 : 0, parts->is_generator ? 1 : 0
  );

  code_arena_mark_t parse_mark = parse_arena_mark(js);
  sv_ast_t *func_node = sv_parse_function_parts(js, parts);
  if (!func_node) {
    parse_arena_rewind(js, parse_mark);
    return NULL;
  }

  sv_code_unit_t *unit = sv_code_unit_begin(js);
  const char *text = copy_source_text(js, parts->text, parts->len);
  
  if (!text) {
    sv_code_unit_finish(unit);
    sv_code_unit_unpin(unit);
    parse_arena_rewind(js, parse_mark);
    return NULL;
  }

  sv_compiler_t root;
  sv_compile_ctx_init_root(&root, js, js->filename, text, parts->len, SV_COMPILE_SCRIPT, false, NULL);
  
  root.allows_new_target = true;
  root.function_ctor_root = true;
  root.line_table = sv_compile_ctx_build_line_table(root.source, parts->len);
  sv_func_t *func = compile_function_body(&root, func_node, SV_COMPILE_SCRIPT);
  
  sv_compile_ctx_free_line_table(root.line_table);
  sv_code_unit_finish(unit);
  parse_arena_rewind(js, parse_mark);

  if (sv_debug_unlikely(SV_DEBUG_COMPILE)) fprintf(
    stderr, "[compile] end kind=function thrown=%d func=%p\n",
    Ant_Exception_Pending(js) ? 1 : 0, (void *)func
  );
    
  if (Ant_Exception_Pending(js) || !func) {
    sv_code_unit_unpin(unit);
    return NULL;
  }
  return func;
}

sv_func_t *sv_compile_function_with_params(
  ant_t *js,
  const sv_param_t *params,
  int param_count,
  const char *body,
  size_t body_len,
  bool is_async
) {
  if (sv_debug_unlikely(SV_DEBUG_COMPILE)) fprintf(
    stderr, "[compile] start kind=function-with-params len=%u params=%d async=%d\n",
    (unsigned)body_len, param_count, is_async ? 1 : 0
  );

  if (!body) {
    body = "";
    body_len = 0;
  }

  bool parse_strict = sv_vm_is_strict(js->vm);
  code_arena_mark_t parse_mark = parse_arena_mark(js);
  sv_ast_t *program = sv_parse_function_body(js, body, (ant_offset_t)body_len, parse_strict);
  
  if (!program) {
    parse_arena_rewind(js, parse_mark);
    return NULL;
  }

  if (program->flags & FN_MODULE_SYNTAX) {
    js_mkerr_typed(js, JS_ERR_SYNTAX, "Cannot use import/export syntax in CommonJS");
    parse_arena_rewind(js, parse_mark);
    return NULL;
  }

  static const char *k_top_name_function = "<function>";
  static const char *k_top_name_async_function = "<async function>";
  const char *top_name = is_async ? k_top_name_async_function : k_top_name_function;

  sv_ast_t top_fn;
  memset(&top_fn, 0, sizeof(top_fn));

  top_fn.type = N_FUNC;
  top_fn.str = top_name;
  top_fn.len = (uint32_t)strlen(top_name);
  top_fn.src_off = 0;
  top_fn.src_end = (body_len > 0) ? (uint32_t)body_len : 0;
  if (is_async) top_fn.flags |= FN_ASYNC;

  for (int i = 0; i < param_count; i++) {
    const char *name = (params && params[i].name) ? params[i].name : "";
    size_t name_len = 0;
    if (params && params[i].name) {
      name_len = params[i].len ? params[i].len : strlen(name);
    }

    sv_ast_t *ident = sv_ast_new(js, N_IDENT);
    if (!ident) {
      parse_arena_rewind(js, parse_mark);
      return NULL;
    }

    ident->str = name;
    ident->len = (uint32_t)name_len;
    sv_ast_list_push(js, &top_fn.args, ident);
  }

  top_fn.body = sv_ast_new(js, N_BLOCK);
  if (!top_fn.body) {
    parse_arena_rewind(js, parse_mark);
    return NULL;
  }
  
  top_fn.body->args = program->args;
  top_fn.flags |= program->flags & (FN_SCOPE_FACTS | FN_DIRECT_EVAL | FN_OWN_YIELD);
  const char *text = copy_source_text(js, body, (ant_offset_t)body_len);
  if (!text && body && body_len > 0) {
    parse_arena_rewind(js, parse_mark);
    return NULL;
  }

  sv_compiler_t root;
  
  sv_compile_ctx_init_root(
    &root, js, js->filename, text,
    (ant_offset_t)body_len, SV_COMPILE_SCRIPT,
    (program->flags & FN_PARSE_STRICT) != 0, NULL
  );
  
  root.allows_new_target = true;
  root.line_table = sv_compile_ctx_build_line_table(root.source, (ant_offset_t)body_len);
  sv_func_t *func = compile_function_body(&root, &top_fn, SV_COMPILE_SCRIPT);
  
  sv_compile_ctx_free_line_table(root.line_table);
  parse_arena_rewind(js, parse_mark);
  
  if (sv_debug_unlikely(SV_DEBUG_COMPILE)) fprintf(
    stderr, "[compile] end kind=function-with-params thrown=%d func=%p\n",
    Ant_Exception_Pending(js) ? 1 : 0, (void *)func
  );
  
  if (Ant_Exception_Pending(js) || !func) return NULL;
  return func;
}
