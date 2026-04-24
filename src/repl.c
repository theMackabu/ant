#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "ant.h"
#include "repl.h"
#include "readline.h"
#include "reactor.h"
#include "runtime.h"
#include "internal.h"
#include "descriptors.h"

#include "silver/ast.h"
#include "silver/engine.h"

#include <crprintf.h>
#include "modules/io.h"
#include "highlight.h"
#include "highlight/regex.h"

typedef ant_history_t history_t;

typedef enum {
  CMD_OK,
  CMD_EXIT,
  CMD_NOT_FOUND
} cmd_result_t;

typedef struct {
  const char *name;
  const char *description;
  bool has_arg;
  cmd_result_t (*handler)(ant_t *js, history_t *history, const char *arg);
} repl_command_t;

typedef struct {
  char *name;
  size_t len;
} repl_decl_name_t;

typedef struct {
  repl_decl_name_t *items;
  size_t count;
  size_t cap;
} repl_decl_registry_t;

typedef struct {
  const char **names;
  uint32_t *lens;
  size_t count;
  size_t cap;
} repl_decl_pending_t;

static repl_decl_registry_t *g_repl_decl_registry = NULL;

static inline void repl_clear_exception_state(ant_t *js) {
  js->thrown_exists = false;
  js->thrown_value = js_mkundef();
}

static void repl_decl_registry_free(repl_decl_registry_t *reg) {
  if (!reg) return;
  for (size_t i = 0; i < reg->count; i++) 
    free(reg->items[i].name);
  free(reg->items);
  reg->items = NULL;
  reg->count = 0;
  reg->cap = 0;
}

static bool repl_decl_registry_contains(
  const repl_decl_registry_t *reg,
  const char *name, uint32_t len
) {
  if (!reg || !name) return false;
  for (size_t i = 0; i < reg->count; i++) {
    if (
      reg->items[i].len == (size_t)len 
      && memcmp(reg->items[i].name, name, (size_t)len) == 0
    ) return true;
  }
  return false;
}

static bool repl_decl_registry_add(
  ant_t *js, repl_decl_registry_t *reg,
  const char *name, uint32_t len
) {
  if (!reg || !name) return true;
  if (repl_decl_registry_contains(reg, name, len)) return true;
  
  if (reg->count >= reg->cap) {
    size_t new_cap = reg->cap ? reg->cap * 2 : 32;
    repl_decl_name_t *ni = realloc(reg->items, new_cap * sizeof(*ni));
    if (!ni) {
      js_mkerr_typed(js, JS_ERR_INTERNAL | JS_ERR_NO_STACK, "out of memory");
      return false;
    }
    reg->items = ni;
    reg->cap = new_cap;
  }
  
  char *copy = malloc((size_t)len + 1);
  if (!copy) {
    js_mkerr_typed(js, JS_ERR_INTERNAL | JS_ERR_NO_STACK, "out of memory");
    return false;
  }
  
  memcpy(copy, name, (size_t)len);
  copy[len] = '\0';
  reg->items[reg->count++] = (repl_decl_name_t){ .name = copy, .len = (size_t)len };
  
  return true;
}

static void repl_decl_pending_free(repl_decl_pending_t *p) {
  if (!p) return;
  free(p->names);
  free(p->lens);
  p->names = NULL;
  p->lens = NULL;
  p->count = 0;
  p->cap = 0;
}

static bool repl_decl_pending_contains(
  const repl_decl_pending_t *p,
  const char *name, uint32_t len
) {
  if (!p || !name) return false;
  for (size_t i = 0; i < p->count; i++) 
    if (p->lens[i] == len && memcmp(p->names[i], name, (size_t)len) == 0) return true;
  return false;
}

static bool repl_decl_pending_push(
  ant_t *js, repl_decl_pending_t *p,
  const char *name, uint32_t len
) {
  if (!p || !name || len == 0) return true;
  if (repl_decl_pending_contains(p, name, len)) return true;
  if (p->count >= p->cap) {
    size_t new_cap = p->cap ? p->cap * 2 : 16;
    const char **nn = realloc(p->names, new_cap * sizeof(*nn));
    if (!nn) {
      js_mkerr_typed(js, JS_ERR_INTERNAL | JS_ERR_NO_STACK, "out of memory");
      return false;
    }
    uint32_t *nl = realloc(p->lens, new_cap * sizeof(*nl));
    if (!nl) {
      p->names = nn;
      js_mkerr_typed(js, JS_ERR_INTERNAL | JS_ERR_NO_STACK, "out of memory");
      return false;
    }
    p->names = nn;
    p->lens = nl;
    p->cap = new_cap;
  }
  p->names[p->count] = name;
  p->lens[p->count] = len;
  p->count++;
  return true;
}

static bool repl_collect_pattern_names(ant_t *js, sv_ast_t *pat, repl_decl_pending_t *p) {
  if (!pat) return true;
  switch (pat->type) {
    case N_IDENT:
      return repl_decl_pending_push(js, p, pat->str, pat->len);
    case N_ASSIGN_PAT:
    case N_ASSIGN:
      return repl_collect_pattern_names(js, pat->left, p);
    case N_REST:
    case N_SPREAD:
      return repl_collect_pattern_names(js, pat->right, p);
    case N_ARRAY:
    case N_ARRAY_PAT:
      for (int i = 0; i < pat->args.count; i++) {
        if (!repl_collect_pattern_names(js, pat->args.items[i], p)) return false;
      }
      return true;
    case N_OBJECT:
    case N_OBJECT_PAT:
      for (int i = 0; i < pat->args.count; i++) {
        sv_ast_t *prop = pat->args.items[i];
        if (!prop) continue;
        if (prop->type == N_PROPERTY) {
          if (!repl_collect_pattern_names(js, prop->right, p)) return false;
        } else if (prop->type == N_REST || prop->type == N_SPREAD) {
          if (!repl_collect_pattern_names(js, prop->right, p)) return false;
        }
      }
      return true;
    default: return true;
  }
}

static bool repl_collect_top_level_decls(ant_t *js, sv_ast_t *stmt, repl_decl_pending_t *p) {
  if (!stmt) return true;
  sv_ast_t *node = (stmt->type == N_EXPORT) ? stmt->left : stmt;
  if (!node) return true;

  if (node->type == N_VAR && node->var_kind != SV_VAR_VAR) {
    for (int i = 0; i < node->args.count; i++) {
      sv_ast_t *decl = node->args.items[i];
      if (!decl || decl->type != N_VARDECL || !decl->left) continue;
      if (!repl_collect_pattern_names(js, decl->left, p)) return false;
    }
    return true;
  }

  if (node->type == N_CLASS && node->str && node->len > 0)
    return repl_decl_pending_push(js, p, node->str, node->len);

  if (node->type == N_IMPORT_DECL) {
    for (int i = 0; i < node->args.count; i++) {
      sv_ast_t *spec = node->args.items[i];
      if (!spec || spec->type != N_IMPORT_SPEC || !spec->right) continue;
      if (spec->right->type != N_IDENT) continue;
      if (!repl_decl_pending_push(js, p, spec->right->str, spec->right->len)) return false;
    }
  }

  return true;
}

static bool repl_precheck_and_commit_lexicals(
  ant_t *js, repl_decl_registry_t *reg,
  const char *code, size_t len
) {
  if (!js || !reg || !code || len == 0) return true;

  code_arena_mark_t mark = code_arena_mark();
  repl_decl_pending_t pending = {0};
  bool ok = true;

  repl_clear_exception_state(js);
  sv_ast_t *program = sv_parse(js, code, (ant_offset_t)len, false);
  
  if (!program || js->thrown_exists) {
    ok = true;
    goto done;
  }

  for (int i = 0; i < program->args.count; i++) {
    if (
      !repl_collect_top_level_decls(
      js, program->args.items[i], &pending)
    ) { ok = false; goto done; }
  }

  for (size_t i = 0; i < pending.count; i++) {
    if (repl_decl_registry_contains(reg, pending.names[i], pending.lens[i])) {
      js_mkerr_typed(
        js, JS_ERR_SYNTAX, "Identifier '%.*s' has already been declared",
        (int)pending.lens[i], pending.names[i]
      );
      ok = false; goto done;
    }
  }

  for (size_t i = 0; i < pending.count; i++) {
    if (
      !repl_decl_registry_add(
      js, reg, pending.names[i], pending.lens[i])
    ) { ok = false; goto done; }
  }

done:
  code_arena_rewind(mark);
  repl_decl_pending_free(&pending);

  if (ok && js->thrown_exists)
    repl_clear_exception_state(js);

  return ok;
}

typedef enum {
  REPL_PRINT_INTERACTIVE,
  REPL_PRINT_LOAD,
} repl_print_mode_t;

static void repl_eval_chunk(
  ant_t *js, repl_decl_registry_t *decl_registry,
  const char *code, size_t len,
  repl_print_mode_t print_mode
) {
  if (!repl_precheck_and_commit_lexicals(js, decl_registry, code, len)) {
    if (js->thrown_exists) js_set(js, js_glob(js), "_error", js->thrown_value);
    print_uncaught_throw(js);
    return;
  }

  repl_clear_exception_state(js);
  ant_value_t result = js_eval_bytecode_repl(js, code, len);
  js_run_event_loop(js);

  if (js->thrown_exists) {
    js_set(js, js_glob(js), "_error", js->thrown_value);
    if (print_uncaught_throw(js)) return;
  }

  if (print_mode == REPL_PRINT_INTERACTIVE) {
    js_set(js, js_glob(js), "_", result);
    print_repl_value(js, result, stdout);
    return;
  }

  if (vtype(result) == T_ERR) fprintf(stderr, "%s\n", js_str(js, result));
  else if (vtype(result) != T_UNDEF) printf("%s\n", js_str(js, result));
}

static cmd_result_t cmd_help(ant_t *js, history_t *history, const char *arg);
static cmd_result_t cmd_exit(ant_t *js, history_t *history, const char *arg);
static cmd_result_t cmd_load(ant_t *js, history_t *history, const char *arg);
static cmd_result_t cmd_save(ant_t *js, history_t *history, const char *arg);
static cmd_result_t cmd_stats(ant_t *js, history_t *history, const char *arg);
static cmd_result_t cmd_copy(ant_t *js, history_t *history, const char *arg);
static cmd_result_t cmd_clear(ant_t *js, history_t *history, const char *arg);
static cmd_result_t cmd_history(ant_t *js, history_t *history, const char *arg);

static const repl_command_t commands[] = {
  { "help",    "Show this help message",                                     false, cmd_help },
  { "exit",    "Exit the REPL",                                              false, cmd_exit },
  { "clear",   "Clear the screen",                                           false, cmd_clear },
  { "history", "Show command history",                                       false, cmd_history },
  { "load",    "Load JS from a file into the REPL session",                  true, cmd_load },
  { "save",    "Save all evaluated commands in this REPL session to a file", true, cmd_save },
  { "stats",   "Show memory statistics",                                     false, cmd_stats },
  { "copy",    "Evaluate expression and copy its value",                     true, cmd_copy },
  { NULL, NULL, false, NULL }
};

static const char *repl_command_usage(const repl_command_t *cmd) {
  if (!cmd || !cmd->name) return "";
  if (strcmp(cmd->name, "copy") == 0)    return ".copy [expr]";
  if (strcmp(cmd->name, "load") == 0)    return ".load <file>";
  if (strcmp(cmd->name, "save") == 0)    return ".save <file>";
  if (strcmp(cmd->name, "history") == 0) return ".history";
  if (strcmp(cmd->name, "clear") == 0)   return ".clear";
  if (strcmp(cmd->name, "stats") == 0)   return ".stats";
  if (strcmp(cmd->name, "exit") == 0)    return ".exit";
  if (strcmp(cmd->name, "help") == 0)    return ".help";
  return cmd->name;
}

static cmd_result_t cmd_help(ant_t *js, history_t *history, const char *arg) {
  printf("\n%sREPL Commands:%s\n", C_BOLD, C_RESET);
  for (const repl_command_t *cmd = commands; cmd->name; cmd++) {
    const char *usage = repl_command_usage(cmd);
    printf("  %s%-12s%s %s\n", C_CYAN, usage, C_RESET, cmd->description);
  }
  printf("\n%sKeybindings:%s\n", C_BOLD, C_RESET);
  printf("  Ctrl+C       Abort current expression (press twice to exit)\n");
  printf("  Left/Right   Move backward/forward one character\n");
  printf("  Home/End     Jump to start/end of line\n");
  printf("  Up/Down      Navigate history\n");
  printf("  Backspace    Delete character backward\n");
  printf("  Delete       Delete character under cursor\n");
  printf("  Enter        Submit input\n");
  printf("\n%sSpecial Variables:%s\n", C_BOLD, C_RESET);
  printf("  %s_%s           Last expression result\n", C_CYAN, C_RESET);
  printf("  %s_error%s      Last error\n\n", C_CYAN, C_RESET);
  return CMD_OK;
}

static cmd_result_t cmd_exit(ant_t *js, history_t *history, const char *arg) {
  return CMD_EXIT;
}

static cmd_result_t cmd_load(ant_t *js, history_t *history, const char *arg) {
  (void)history;
  if (!arg || *arg == '\0') {
    fprintf(stderr, "Usage: .load <filename>\n");
    return CMD_OK;
  }
  
  FILE *fp = fopen(arg, "r");
  if (fp == NULL) {
    fprintf(stderr, "Failed to open file: %s\n", arg);
    return CMD_OK;
  }
  
  fseek(fp, 0, SEEK_END);
  long file_size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  
  char *file_buffer = malloc(file_size + 1);
  if (file_buffer) {
    size_t len = fread(file_buffer, 1, file_size, fp);
    file_buffer[len] = '\0';
    repl_eval_chunk(
      js, g_repl_decl_registry, 
      file_buffer, len, REPL_PRINT_LOAD
    );
    free(file_buffer);
  }
  fclose(fp);
  return CMD_OK;
}

static cmd_result_t cmd_save(ant_t *js, history_t *history, const char *arg) {
  if (!arg || *arg == '\0') {
    fprintf(stderr, "Usage: .save <filename>\n");
    return CMD_OK;
  }
  
  FILE *fp = fopen(arg, "w");
  if (fp == NULL) {
    fprintf(stderr, "Failed to open file for writing: %s\n", arg);
    return CMD_OK;
  }
  
  for (int i = 0; i < history->count; i++) {
    fprintf(fp, "%s\n", history->lines[i]);
  }
  fclose(fp);
  printf("Session saved to %s\n", arg);
  return CMD_OK;
}

static cmd_result_t cmd_stats(ant_t *js, history_t *history, const char *arg) {
  ant_value_t stats_fn = js_get(js, rt->ant_obj, "stats");
  ant_value_t result = sv_vm_call(js->vm, js, stats_fn, js_mkundef(), NULL, 0, NULL, false);
  console_emit(js, false, NULL, &result, 1);
  return CMD_OK;
}

static cmd_result_t cmd_clear(ant_t *js, history_t *history, const char *arg) {
  fputs("\033[2J\033[H", stdout);
  fflush(stdout);
  return CMD_OK;
}

static cmd_result_t cmd_history(ant_t *js, history_t *history, const char *arg) {
  for (int i = 0; i < history->count; i++) {
    printf("%4d  %s\n", i + 1, history->lines[i]);
  }
  return CMD_OK;
}

#ifdef _WIN32
static bool repl_copy_with_command(const char *data, size_t len) {
  FILE *pipe = _popen("clip", "wb");
  if (!pipe) return false;

  size_t written = fwrite(data, 1, len, pipe);
  int close_rc = _pclose(pipe);
  return written == len && close_rc == 0;
}
#else
static bool repl_copy_with_single_command(const char *cmd, const char *data, size_t len) {
  FILE *pipe = popen(cmd, "w");
  if (!pipe) return false;

  size_t written = fwrite(data, 1, len, pipe);
  int close_rc = pclose(pipe);
  return written == len && close_rc == 0;
}

static bool repl_copy_with_command(const char *data, size_t len) {
  static const char *cmds[] = {
    "pbcopy",
    "wl-copy",
    "xclip -selection clipboard",
    "xsel --clipboard --input",
  };
  for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
    if (repl_copy_with_single_command(cmds[i], data, len)) return true;
  }
  return false;
}
#endif

static cmd_result_t cmd_copy(ant_t *js, history_t *history, const char *arg) {
  (void)history;
  if (!arg || *arg == '\0') {
    fprintf(stderr, "Usage: .copy <expression>\n");
    return CMD_OK;
  }

  repl_clear_exception_state(js);
  ant_value_t result = js_eval_bytecode_repl(js, arg, strlen(arg));
  
  js_run_event_loop(js);
  if (js->thrown_exists) {
    js_set(js, js_glob(js), "_error", js->thrown_value);
    if (print_uncaught_throw(js)) return CMD_OK;
  }

  js_set(js, js_glob(js), "_", result);

  char cbuf[512];
  js_cstr_t cstr = js_to_cstr(js, result, cbuf, sizeof(cbuf));
  
  bool copied_command = repl_copy_with_command(cstr.ptr, cstr.len);
  if (cstr.needs_free) free((void *)cstr.ptr);

  if (!copied_command) {
    fprintf(stderr, "Failed to copy to clipboard (no clipboard command available).\n");
    return CMD_OK;
  }

  printf("Copied to clipboard.\n");
  return CMD_OK;
}

static cmd_result_t execute_command(ant_t *js, history_t *history, const char *line) {
  const char *cmd_start = line + 1;
  
  for (const repl_command_t *cmd = commands; cmd->name; cmd++) {
    size_t n = strlen(cmd->name);
    if (strncmp(cmd_start, cmd->name, n) != 0) continue;
    
    char next = cmd_start[n];
    if (cmd->has_arg && (next == ' ' || next == '\0')) {
      const char *arg = cmd_start + n;
      while (*arg == ' ') arg++;
      return cmd->handler(js, history, arg);
    }
    if (!cmd->has_arg && next == '\0') return cmd->handler(js, history, NULL);
  }
  
  return CMD_NOT_FOUND;
}

typedef struct {
  int paren, bracket, brace;
  int *templates;
  int template_count, template_cap;
  char string_char;
  bool in_string, escaped;
} parse_state_t;

static void push_template(parse_state_t *s) {
  if (s->template_count >= s->template_cap) {
    s->template_cap = s->template_cap ? s->template_cap * 2 : 8;
    int *new_templates = realloc(s->templates, s->template_cap * sizeof(int));
    if (!new_templates) { return; } s->templates = new_templates;
  }
  s->templates[s->template_count++] = s->brace;
}

static bool in_template_text(parse_state_t *s) {
  return s->template_count > 0 && s->brace == s->templates[s->template_count - 1];
}

static bool is_incomplete_input(const char *code, size_t len) {
  parse_state_t s = {0};
  
  for (size_t i = 0; i < len; i++) {
    char c = code[i];
    
    if (s.escaped) { s.escaped = false; continue; }
    if (c == '\\' && (s.in_string || s.template_count > 0)) { s.escaped = true; continue; }
    if (s.in_string) { if (c == s.string_char) s.in_string = false; continue; }
    
    if (in_template_text(&s)) {
      if (c == '`') s.template_count--;
      else if (c == '$' && i + 1 < len && code[i + 1] == '{') { s.brace++; i++; }
      continue;
    }
    
    if (c == '/' && i + 1 < len) {
      if (code[i + 1] == '/') { while (i < len && code[i] != '\n') i++; continue; }
      if (code[i + 1] == '*') {
        for (i += 2; i + 1 < len && !(code[i] == '*' && code[i + 1] == '/'); i++);
        if (i + 1 >= len) { free(s.templates); return true; }
        i++; continue;
      }
      if (js_regex_can_start(code, i)) {
        size_t regex_end = 0;
        if (!js_scan_regex_literal(code, len, i, &regex_end)) { free(s.templates); return true; }
        i = regex_end - 1;
        continue;
      }
    }
    
    switch (c) {
      case '"': case '\'': s.in_string = true; s.string_char = c; break;
      case '`': push_template(&s); break;
      case '(': s.paren++; break;   case ')': s.paren--; break;
      case '[': s.bracket++; break; case ']': s.bracket--; break;
      case '{': s.brace++; break;   case '}': s.brace--; break;
    }
  }
  
  bool incomplete = s.in_string || s.template_count > 0 || s.paren > 0 || s.bracket > 0 || s.brace > 0;
  free(s.templates);
  return incomplete;
}

void ant_repl_run() {
  ant_t *js = rt->js;
  ant_readline_install_signal_handler();
  
  js_set_filename(js, "[repl]");
  js_setup_import_meta(js, "[repl]");
  
  crprintf(
    "Welcome to <red+bold>Ant JavaScript</> v%s\n"
    "Type <cyan>.copy [code]</cyan> to copy, <cyan>.help</cyan> for more information.\n\n",
    ANT_VERSION
  );
  
  history_t history;
  ant_history_init(&history, 512);
  ant_history_load(&history);
  
  repl_decl_registry_t decl_registry = {0};
  g_repl_decl_registry = &decl_registry;
  
  js_set(js, js_glob(js), "__dirname", js_mkstr(js, ".", 1));
  js_set(js, js_glob(js), "__filename", js_mkstr(js, "[repl]", 6));
  
  js_set(js, js_glob(js), "_", js_mkundef());
  js_set(js, js_glob(js), "_error", js_mkundef());
  
  js_set_descriptor(js, js_as_obj(js_glob(js)), "_", 1, JS_DESC_W | JS_DESC_C);
  js_set_descriptor(js, js_as_obj(js_glob(js)), "_error", 6, JS_DESC_W | JS_DESC_C);

  int prev_ctrl_c_count = 0;
  char *multiline_buf = NULL;
  size_t multiline_len = 0;
  size_t multiline_cap = 0;
  
  while (1) {
    const char *prompt = multiline_buf ? "\x1b[2m|\x1b[0m " : "\x1b[2m❯\x1b[0m ";
    highlight_state prefix_state = HL_STATE_INIT;
    if (multiline_buf && multiline_len > 0) {
      char scratch[8192];
      ant_highlight_stateful(multiline_buf, multiline_len, scratch, sizeof(scratch), &prefix_state);
    }

    fputs(prompt, stdout);
    fflush(stdout);

    char *line = NULL;
    ant_readline_result_t readline_status =
      ant_readline(&history, prompt, prefix_state, &line);

    if (readline_status == ANT_READLINE_INTERRUPT) {
      if (multiline_buf) {
        free(multiline_buf);
        multiline_buf = NULL;
        multiline_len = 0;
        multiline_cap = 0;
        prev_ctrl_c_count = 0;
        if (line) free(line);
        continue;
      }
      if (prev_ctrl_c_count > 0) {
        if (line) free(line);
        break;
      }
      printf("(To exit, press Ctrl+C again or type .exit)\n");
      prev_ctrl_c_count++;
      if (line) free(line);
      continue;
    }

    if (readline_status == ANT_READLINE_EOF || line == NULL) {
      if (multiline_buf) {
        free(multiline_buf);
        multiline_buf = NULL;
        multiline_len = 0;
        multiline_cap = 0;
        continue;
      }
      break;
    }
    
    prev_ctrl_c_count = 0;
    size_t line_len = strlen(line);
    
    if (line_len == 0 && multiline_buf) {
      if (multiline_len + 1 >= multiline_cap) {
        multiline_cap = multiline_cap ? multiline_cap * 2 : 256;
        multiline_buf = realloc(multiline_buf, multiline_cap);
      }
      multiline_buf[multiline_len++] = '\n';
      multiline_buf[multiline_len] = '\0';
      free(line);
      continue;
    }
    
    if (line_len == 0) {
      free(line);
      continue;
    }
    
    if (!multiline_buf && line[0] == '.') {
      cmd_result_t result = execute_command(js, &history, line);
      if (result == CMD_EXIT) {
        free(line);
        break;
      } else if (result == CMD_NOT_FOUND) {
        printf("Unknown command: %s\n", line);
        printf("Type \".help\" for more information.\n");
      }
      free(line);
      continue;
    }
    
    size_t new_len = multiline_len + line_len + 1;
    if (new_len >= multiline_cap || !multiline_buf) {
      multiline_cap = multiline_cap ? multiline_cap * 2 : 256;
      if (multiline_cap < new_len + 1) multiline_cap = new_len + 1;
      multiline_buf = realloc(multiline_buf, multiline_cap);
    }
    
    if (multiline_len > 0) {
      multiline_buf[multiline_len++] = '\n';
    }
    memcpy(multiline_buf + multiline_len, line, line_len);
    multiline_len += line_len;
    multiline_buf[multiline_len] = '\0';
    free(line);
    
    if (is_incomplete_input(multiline_buf, multiline_len)) continue;
    ant_history_add(&history, multiline_buf);
    
    repl_eval_chunk(
      js, &decl_registry, multiline_buf, 
      multiline_len, REPL_PRINT_INTERACTIVE
    );
    
    free(multiline_buf);
    multiline_buf = NULL;
    multiline_len = 0;
    multiline_cap = 0;
  }
  
  if (multiline_buf) free(multiline_buf);
  ant_readline_shutdown();
  
  repl_decl_registry_free(&decl_registry);
  g_repl_decl_registry = NULL;
  
  ant_history_save(&history);
  ant_history_free(&history);
}
