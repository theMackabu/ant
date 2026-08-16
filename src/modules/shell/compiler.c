#include "shell_internal.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char *data;
  size_t len;
  size_t capacity;
  bool failed;
} sh_source_t;

static bool sh_source_format(sh_source_t *source, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
  __attribute__((format(printf, 2, 3)))
#endif
;

static bool sh_source_reserve(sh_source_t *source, size_t additional) {
  if (source->failed) return false;
  if (additional > SIZE_MAX - source->len - 1) {
    source->failed = true;
    return false;
  }
  
  size_t need = source->len + additional + 1;
  if (need <= source->capacity) return true;
  size_t next = source->capacity ? source->capacity * 2 : 1024;
  
  while (next < need) {
    if (next > SIZE_MAX / 2) {
      source->failed = true;
      return false;
    }
    next *= 2;
  }
  
  char *grown = realloc(source->data, next);
  if (!grown) {
    source->failed = true;
    return false;
  }
  
  source->data = grown;
  source->capacity = next;
  
  return true;
}

static bool sh_source_append(sh_source_t *source, const char *text, size_t len) {
  if (!sh_source_reserve(source, len)) return false;
  if (len) memcpy(source->data + source->len, text, len);
  source->len += len;
  source->data[source->len] = '\0';
  return true;
}

static bool sh_source_cstr(sh_source_t *source, const char *text) {
  return sh_source_append(source, text, strlen(text));
}

static bool sh_source_format(sh_source_t *source, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  va_list copy;
  
  va_copy(copy, args);
  int required = vsnprintf(NULL, 0, fmt, copy);
  va_end(copy);
  
  if (required < 0 || !sh_source_reserve(source, (size_t)required)) {
    va_end(args);
    source->failed = true;
    return false;
  }
  
  vsnprintf(source->data + source->len, (size_t)required + 1, fmt, args);
  va_end(args);
  source->len += (size_t)required;
  
  return true;
}

static bool sh_source_js_string(
  sh_source_t *source, const char *text, size_t len
) {
  if (!sh_source_cstr(source, "\"")) return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char ch = (unsigned char)text[i];
    switch (ch) {
      case '"': if (!sh_source_cstr(source, "\\\"")) return false; break;
      case '\\': if (!sh_source_cstr(source, "\\\\")) return false; break;
      case '\b': if (!sh_source_cstr(source, "\\b")) return false; break;
      case '\f': if (!sh_source_cstr(source, "\\f")) return false; break;
      case '\n': if (!sh_source_cstr(source, "\\n")) return false; break;
      case '\r': if (!sh_source_cstr(source, "\\r")) return false; break;
      case '\t': if (!sh_source_cstr(source, "\\t")) return false; break;
      default:
        if (ch < 0x20) {
          if (!sh_source_format(source, "\\u%04x", ch)) return false;
        } else if (!sh_source_append(source, text + i, 1)) return false;
        break;
    }
  }
  return sh_source_cstr(source, "\"");
}

static const char *sh_debug_quote_name(sh_quote_t quote) {
  switch (quote) {
    case SH_QUOTE_SINGLE: return "single";
    case SH_QUOTE_DOUBLE: return "double";
    default: return "none";
  }
}

static const char *sh_debug_connector_name(sh_connector_t connector) {
  switch (connector) {
    case SH_CONNECT_AND: return "and";
    case SH_CONNECT_OR: return "or";
    default: return "always";
  }
}

static const char *sh_debug_redirection_name(sh_redir_kind_t kind) {
  switch (kind) {
    case SH_REDIR_STDIN: return "stdin";
    case SH_REDIR_STDOUT: return "stdout";
    case SH_REDIR_STDOUT_APPEND: return "stdout-append";
    case SH_REDIR_STDERR_TO_STDOUT: return "stderr-to-stdout";
  }
  return "unknown";
}

static bool sh_debug_word(sh_source_t *source, const sh_word_t *word) {
  if (!sh_source_cstr(source, "[")) return false;
  for (size_t i = 0; i < word->part_count; i++) {
    const sh_word_part_t *part = &word->parts[i];
    if (i && !sh_source_cstr(source, ",")) return false;
    if (part->kind == SH_PART_LITERAL) {
      if (!sh_source_cstr(source, "{\"literal\":")) return false;
      if (!sh_source_js_string(source, part->text, part->text_len)) return false;
    } else if (!sh_source_format(
      source, "{\"interpolation\":%zu", part->interpolation
    )) return false;
    if (!sh_source_format(
      source, ",\"quote\":\"%s\"}", sh_debug_quote_name(part->quote)
    )) return false;
  }
  return sh_source_cstr(source, "]");
}

static bool sh_debug_command(
  sh_source_t *source, const sh_command_t *command
) {
  if (!sh_source_cstr(source, "{\"words\":[")) return false;
  for (size_t i = 0; i < command->word_count; i++) {
    if (i && !sh_source_cstr(source, ",")) return false;
    if (!sh_debug_word(source, &command->words[i])) return false;
  }
  if (!sh_source_cstr(source, "],\"redirections\":[")) return false;
  for (size_t i = 0; i < command->redir_count; i++) {
    const sh_redir_t *redir = &command->redirs[i];
    if (i && !sh_source_cstr(source, ",")) return false;
    if (!sh_source_format(
      source, "{\"kind\":\"%s\"", sh_debug_redirection_name(redir->kind)
    )) return false;
    if (redir->kind != SH_REDIR_STDERR_TO_STDOUT) {
      if (!sh_source_cstr(source, ",\"target\":")) return false;
      if (!sh_debug_word(source, &redir->target)) return false;
    }
    if (!sh_source_cstr(source, "}")) return false;
  }
  return sh_source_cstr(source, "]}");
}

char *sh_debug_program_plan_source(
  const sh_program_t *program, size_t *source_len
) {
  if (source_len) *source_len = 0;
  if (!program) return NULL;

  sh_source_t source = {0};
  sh_source_cstr(&source, "[");
  for (size_t i = 0; i < program->clause_count; i++) {
    const sh_clause_t *clause = &program->clauses[i];
    if (i && !sh_source_cstr(&source, ",")) break;
    if (!sh_source_format(
      &source, "{\"connector\":\"%s\",\"commands\":[",
      sh_debug_connector_name(clause->connector)
    )) break;
    for (size_t j = 0; j < clause->pipeline.command_count; j++) {
      if (j && !sh_source_cstr(&source, ",")) break;
      if (!sh_debug_command(&source, &clause->pipeline.commands[j])) break;
    }
    if (!sh_source_cstr(&source, "]}")) break;
  }
  sh_source_cstr(&source, "]");

  if (source.failed) {
    free(source.data);
    return NULL;
  }
  if (!source.data) {
    source.data = calloc(1, 1);
    if (!source.data) return NULL;
  }
  if (source_len) *source_len = source.len;
  return source.data;
}

char *sh_compile_program_source(
  const sh_program_t *program,
  size_t *source_len,
  sh_parse_error_t *error
) {
  if (source_len) *source_len = 0;
  if (error) memset(error, 0, sizeof(*error));
  if (!program) return NULL;

  sh_source_t source = {0};
  if (program->clause_count == 0) {
    sh_source_cstr(&source, "return __finish(__ctx,null);");
    goto done;
  }
  
  if (program->clause_count == 1) {
    sh_source_cstr(&source, "return await __run(__ctx,__plan,0,__values);");
    goto done;
  }

  sh_source_cstr(&source, "let __result;");
  for (size_t i = 0; i < program->clause_count; i++) {
    const sh_clause_t *clause = &program->clauses[i];
    if (clause->connector == SH_CONNECT_AND)
      sh_source_cstr(&source, "if(__result.exitCode===0){");
    else if (clause->connector == SH_CONNECT_OR)
      sh_source_cstr(&source, "if(__result.exitCode!==0){");

    sh_source_format(
      &source, "__result=await __run(__ctx,__plan,%zu,__values);", i
    );
    sh_source_cstr(&source,
      "if(__result.exited)return __finish(__ctx,__result);"
    );

    if (clause->connector != SH_CONNECT_ALWAYS) sh_source_cstr(&source, "}");
  }

  sh_source_cstr(&source, "return __finish(__ctx,__result);");

done:
  if (source.failed) {
    free(source.data);
    if (error) snprintf(error->message, sizeof(error->message),
      "out of memory while lowering shell program");
    return NULL;
  }
  if (!source.data) {
    source.data = calloc(1, 1);
    if (!source.data) return NULL;
  }
  if (source_len) *source_len = source.len;
  return source.data;
}
