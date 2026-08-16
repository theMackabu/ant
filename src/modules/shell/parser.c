#include "modules/shell_internal.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *const *segments;
  const size_t *lengths;
  size_t count;
  size_t segment;
  size_t offset;
  bool boundary_pending;
} sh_input_t;

typedef enum {
  SH_INPUT_EOF = 0,
  SH_INPUT_CHAR,
  SH_INPUT_INTERPOLATION,
} sh_input_kind_t;

typedef struct {
  sh_input_kind_t kind;
  unsigned char ch;
  size_t interpolation;
} sh_input_event_t;

typedef enum {
  SH_TOKEN_EOF = 0,
  SH_TOKEN_WORD,
  SH_TOKEN_PIPE,
  SH_TOKEN_AND,
  SH_TOKEN_OR,
  SH_TOKEN_SEQUENCE,
  SH_TOKEN_REDIR_STDIN,
  SH_TOKEN_REDIR_STDOUT,
  SH_TOKEN_REDIR_STDOUT_APPEND,
  SH_TOKEN_REDIR_STDERR_TO_STDOUT,
  SH_TOKEN_ERROR,
} sh_token_kind_t;

typedef struct {
  sh_token_kind_t kind;
  sh_word_t word;
} sh_token_t;

typedef struct {
  char *data;
  size_t len;
  size_t capacity;
} sh_buffer_t;

typedef struct {
  sh_input_t input;
  sh_parse_error_t *error;
} sh_lexer_t;

static void sh_set_error(sh_lexer_t *lexer, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
  __attribute__((format(printf, 2, 3)))
#endif
;

static bool sh_grow(void **ptr, size_t *capacity, size_t need, size_t item_size) {
  if (need <= *capacity) return true;
  size_t next = *capacity ? *capacity * 2 : 4;
  while (next < need) {
    if (next > SIZE_MAX / 2) return false;
    next *= 2;
  }
  if (item_size && next > SIZE_MAX / item_size) return false;
  void *grown = realloc(*ptr, next * item_size);
  if (!grown) return false;
  *ptr = grown;
  *capacity = next;
  return true;
}

static void sh_set_error(sh_lexer_t *lexer, const char *fmt, ...) {
  if (!lexer->error || lexer->error->message[0]) return;
  lexer->error->segment = lexer->input.segment;
  lexer->error->offset = lexer->input.offset;
  va_list args;
  va_start(args, fmt);
  vsnprintf(lexer->error->message, sizeof(lexer->error->message), fmt, args);
  va_end(args);
}

static sh_input_event_t sh_input_peek(const sh_input_t *input) {
  sh_input_t cursor = *input;
  for (;;) {
    if (cursor.segment >= cursor.count) return (sh_input_event_t){ .kind = SH_INPUT_EOF };
    if (cursor.offset < cursor.lengths[cursor.segment]) {
      return (sh_input_event_t){
        .kind = SH_INPUT_CHAR,
        .ch = (unsigned char)cursor.segments[cursor.segment][cursor.offset],
      };
    }
    if (cursor.segment + 1 < cursor.count && !cursor.boundary_pending) {
      return (sh_input_event_t){
        .kind = SH_INPUT_INTERPOLATION,
        .interpolation = cursor.segment,
      };
    }
    cursor.segment++;
    cursor.offset = 0;
    cursor.boundary_pending = false;
  }
}

static sh_input_event_t sh_input_take(sh_input_t *input) {
  for (;;) {
    if (input->segment >= input->count)
      return (sh_input_event_t){ .kind = SH_INPUT_EOF };
    if (input->offset < input->lengths[input->segment]) {
      return (sh_input_event_t){
        .kind = SH_INPUT_CHAR,
        .ch = (unsigned char)input->segments[input->segment][input->offset++],
      };
    }
    if (input->segment + 1 < input->count && !input->boundary_pending) {
      input->boundary_pending = true;
      return (sh_input_event_t){
        .kind = SH_INPUT_INTERPOLATION,
        .interpolation = input->segment,
      };
    }
    input->segment++;
    input->offset = 0;
    input->boundary_pending = false;
  }
}

static bool sh_buffer_append(sh_buffer_t *buffer, unsigned char ch) {
  if (!sh_grow((void **)&buffer->data, &buffer->capacity, buffer->len + 1, 1)) return false;
  buffer->data[buffer->len++] = (char)ch;
  return true;
}

static bool sh_word_add_literal(sh_word_t *word, sh_quote_t quote, const char *text, size_t len) {
  if (!sh_grow(
    (void **)&word->parts, &word->part_capacity,
    word->part_count + 1, sizeof(*word->parts)
  )) return false;
  char *copy = malloc(len + 1);
  if (!copy) return false;
  if (len) memcpy(copy, text, len);
  copy[len] = '\0';
  word->parts[word->part_count++] = (sh_word_part_t){
    .kind = SH_PART_LITERAL,
    .quote = quote,
    .text = copy,
    .text_len = len,
  };
  return true;
}

static bool sh_word_add_interpolation(sh_word_t *word, sh_quote_t quote, size_t interpolation) {
  if (!sh_grow(
    (void **)&word->parts, &word->part_capacity,
    word->part_count + 1, sizeof(*word->parts)
  )) return false;
  word->parts[word->part_count++] = (sh_word_part_t){
    .kind = SH_PART_INTERPOLATION,
    .quote = quote,
    .interpolation = interpolation,
  };
  return true;
}

static bool sh_flush_literal(sh_word_t *word, sh_quote_t quote, sh_buffer_t *literal) {
  if (!literal->len) return true;
  bool ok = sh_word_add_literal(word, quote, literal->data, literal->len);
  literal->len = 0;
  return ok;
}

static void sh_word_free(sh_word_t *word) {
  for (size_t i = 0; i < word->part_count; i++) free(word->parts[i].text);
  free(word->parts);
  memset(word, 0, sizeof(*word));
}

static bool sh_event_is_char(sh_input_event_t event, unsigned char ch) {
  return event.kind == SH_INPUT_CHAR && event.ch == ch;
}

static bool sh_is_space(unsigned char ch) {
  return ch == ' ' || ch == '\t' || ch == '\r';
}

static bool sh_is_operator_start(unsigned char ch) {
  return ch == '|' || ch == '&' || ch == ';' || ch == '\n' || ch == '<' || ch == '>';
}

static bool sh_take_matching_char(sh_input_t *input, unsigned char ch) {
  sh_input_event_t event = sh_input_peek(input);
  if (!sh_event_is_char(event, ch)) return false;
  sh_input_take(input);
  return true;
}

static sh_token_t sh_lex_word(sh_lexer_t *lexer) {
  sh_token_t token = { .kind = SH_TOKEN_WORD };
  sh_buffer_t literal = {0};
  sh_quote_t quote = SH_QUOTE_NONE;
  sh_quote_t literal_quote = SH_QUOTE_NONE;
  bool word_started = false;

  for (;;) {
    sh_input_event_t event = sh_input_peek(&lexer->input);
    if (event.kind == SH_INPUT_EOF) break;

    if (event.kind == SH_INPUT_INTERPOLATION) {
      if (!sh_flush_literal(&token.word, literal_quote, &literal)) goto oom;
      sh_input_take(&lexer->input);
      if (!sh_word_add_interpolation(&token.word, quote, event.interpolation)) goto oom;
      word_started = true;
      literal_quote = quote;
      continue;
    }

    unsigned char ch = event.ch;
    if (quote == SH_QUOTE_NONE && (sh_is_space(ch) || sh_is_operator_start(ch))) break;

    sh_input_take(&lexer->input);
    if (quote == SH_QUOTE_SINGLE) {
      if (ch == '\'') {
        if (!sh_flush_literal(&token.word, literal_quote, &literal)) goto oom;
        quote = SH_QUOTE_NONE;
        literal_quote = quote;
      } else if (!sh_buffer_append(&literal, ch)) goto oom;
      word_started = true;
      continue;
    }

    if (quote == SH_QUOTE_DOUBLE) {
      if (ch == '"') {
        if (!sh_flush_literal(&token.word, literal_quote, &literal)) goto oom;
        quote = SH_QUOTE_NONE;
        literal_quote = quote;
        word_started = true;
        continue;
      }
      if (ch == '\\') {
        sh_input_event_t next = sh_input_peek(&lexer->input);
        if (next.kind == SH_INPUT_CHAR &&
            (next.ch == '$' || next.ch == '`' || next.ch == '"' || next.ch == '\\' || next.ch == '\n')) {
          sh_input_take(&lexer->input);
          if (next.ch != '\n' && !sh_buffer_append(&literal, next.ch)) goto oom;
          word_started = true;
          continue;
        }
      }
      if (!sh_buffer_append(&literal, ch)) goto oom;
      word_started = true;
      continue;
    }

    if (ch == '\'') {
      if (!sh_flush_literal(&token.word, literal_quote, &literal)) goto oom;
      quote = SH_QUOTE_SINGLE;
      literal_quote = quote;
      word_started = true;
      continue;
    }
    if (ch == '"') {
      if (!sh_flush_literal(&token.word, literal_quote, &literal)) goto oom;
      quote = SH_QUOTE_DOUBLE;
      literal_quote = quote;
      word_started = true;
      continue;
    }
    if (ch == '\\') {
      sh_input_event_t next = sh_input_take(&lexer->input);
      if (next.kind == SH_INPUT_EOF) {
        sh_set_error(lexer, "trailing backslash");
        goto fail;
      }
      if (next.kind == SH_INPUT_INTERPOLATION) {
        sh_set_error(lexer, "backslash cannot escape an interpolation boundary");
        goto fail;
      }
      if (next.ch == '\n') continue;
      if (!sh_buffer_append(&literal, next.ch)) goto oom;
      word_started = true;
      continue;
    }
    if (!sh_buffer_append(&literal, ch)) goto oom;
    word_started = true;
  }

  if (quote != SH_QUOTE_NONE) {
    sh_set_error(lexer, quote == SH_QUOTE_SINGLE
      ? "unterminated single quote" : "unterminated double quote");
    goto fail;
  }
  if (!sh_flush_literal(&token.word, literal_quote, &literal)) goto oom;
  if (word_started && token.word.part_count == 0 &&
      !sh_word_add_literal(&token.word, SH_QUOTE_NONE, "", 0)) goto oom;
  free(literal.data);
  return token;

oom:
  sh_set_error(lexer, "out of memory while parsing shell word");
fail:
  free(literal.data);
  sh_word_free(&token.word);
  token.kind = SH_TOKEN_ERROR;
  return token;
}

static sh_token_t sh_lex_next(sh_lexer_t *lexer) {
  for (;;) {
    sh_input_event_t event = sh_input_peek(&lexer->input);
    if (event.kind == SH_INPUT_EOF) return (sh_token_t){ .kind = SH_TOKEN_EOF };
    if (event.kind == SH_INPUT_INTERPOLATION) return sh_lex_word(lexer);
    if (sh_is_space(event.ch)) {
      sh_input_take(&lexer->input);
      continue;
    }
    if (event.ch == '#') {
      do event = sh_input_take(&lexer->input);
      while (event.kind != SH_INPUT_EOF && !sh_event_is_char(event, '\n'));
      if (sh_event_is_char(event, '\n')) return (sh_token_t){ .kind = SH_TOKEN_SEQUENCE };
      return (sh_token_t){ .kind = SH_TOKEN_EOF };
    }
    break;
  }

  sh_input_event_t event = sh_input_peek(&lexer->input);
  if (event.kind != SH_INPUT_CHAR) return sh_lex_word(lexer);

  if (event.ch == '\n' || event.ch == ';') {
    sh_input_take(&lexer->input);
    return (sh_token_t){ .kind = SH_TOKEN_SEQUENCE };
  }
  if (event.ch == '|') {
    sh_input_take(&lexer->input);
    if (sh_take_matching_char(&lexer->input, '|')) return (sh_token_t){ .kind = SH_TOKEN_OR };
    return (sh_token_t){ .kind = SH_TOKEN_PIPE };
  }
  if (event.ch == '&') {
    sh_input_take(&lexer->input);
    if (sh_take_matching_char(&lexer->input, '&')) return (sh_token_t){ .kind = SH_TOKEN_AND };
    sh_set_error(lexer, "background execution with '&' is not implemented");
    return (sh_token_t){ .kind = SH_TOKEN_ERROR };
  }
  if (event.ch == '<') {
    sh_input_take(&lexer->input);
    return (sh_token_t){ .kind = SH_TOKEN_REDIR_STDIN };
  }
  if (event.ch == '>') {
    sh_input_take(&lexer->input);
    if (sh_take_matching_char(&lexer->input, '>'))
      return (sh_token_t){ .kind = SH_TOKEN_REDIR_STDOUT_APPEND };
    return (sh_token_t){ .kind = SH_TOKEN_REDIR_STDOUT };
  }

  if (event.ch == '2') {
    sh_input_t saved = lexer->input;
    sh_input_take(&lexer->input);
    if (sh_take_matching_char(&lexer->input, '>') &&
        sh_take_matching_char(&lexer->input, '&') &&
        sh_take_matching_char(&lexer->input, '1')) {
      return (sh_token_t){ .kind = SH_TOKEN_REDIR_STDERR_TO_STDOUT };
    }
    lexer->input = saved;
  }

  return sh_lex_word(lexer);
}

static bool sh_command_add_word(sh_command_t *command, sh_word_t *word) {
  if (!sh_grow(
    (void **)&command->words, &command->word_capacity,
    command->word_count + 1, sizeof(*command->words)
  )) return false;
  command->words[command->word_count++] = *word;
  memset(word, 0, sizeof(*word));
  return true;
}

static bool sh_command_add_redir(sh_command_t *command, sh_redir_t *redir) {
  if (!sh_grow(
    (void **)&command->redirs, &command->redir_capacity,
    command->redir_count + 1, sizeof(*command->redirs)
  )) return false;
  command->redirs[command->redir_count++] = *redir;
  memset(redir, 0, sizeof(*redir));
  return true;
}

static void sh_command_free(sh_command_t *command) {
  for (size_t i = 0; i < command->word_count; i++) sh_word_free(&command->words[i]);
  for (size_t i = 0; i < command->redir_count; i++) sh_word_free(&command->redirs[i].target);
  free(command->words);
  free(command->redirs);
  memset(command, 0, sizeof(*command));
}

static bool sh_pipeline_add_command(sh_pipeline_t *pipeline, sh_command_t *command) {
  if (!sh_grow(
    (void **)&pipeline->commands, &pipeline->command_capacity,
    pipeline->command_count + 1, sizeof(*pipeline->commands)
  )) return false;
  pipeline->commands[pipeline->command_count++] = *command;
  memset(command, 0, sizeof(*command));
  return true;
}

static void sh_pipeline_free(sh_pipeline_t *pipeline) {
  for (size_t i = 0; i < pipeline->command_count; i++) sh_command_free(&pipeline->commands[i]);
  free(pipeline->commands);
  memset(pipeline, 0, sizeof(*pipeline));
}

static bool sh_program_add_clause(sh_program_t *program, sh_clause_t *clause) {
  if (!sh_grow(
    (void **)&program->clauses, &program->clause_capacity,
    program->clause_count + 1, sizeof(*program->clauses)
  )) return false;
  program->clauses[program->clause_count++] = *clause;
  memset(clause, 0, sizeof(*clause));
  return true;
}

void sh_program_free(sh_program_t *program) {
  if (!program) return;
  for (size_t i = 0; i < program->clause_count; i++)
    sh_pipeline_free(&program->clauses[i].pipeline);
  free(program->clauses);
  memset(program, 0, sizeof(*program));
}

static bool sh_parse_pipeline(
  sh_lexer_t *lexer,
  sh_pipeline_t *pipeline,
  sh_token_t *lookahead
) {
  sh_command_t command = {0};
  for (;;) {
    memset(&command, 0, sizeof(command));
    bool have_content = false;

    for (;;) {
      sh_token_t token = *lookahead;
      memset(lookahead, 0, sizeof(*lookahead));
      if (token.kind == SH_TOKEN_WORD) {
        if (!sh_command_add_word(&command, &token.word)) {
          sh_set_error(lexer, "out of memory while parsing command");
          sh_word_free(&token.word);
          goto fail;
        }
        have_content = true;
        *lookahead = sh_lex_next(lexer);
        continue;
      }
      if (token.kind == SH_TOKEN_REDIR_STDERR_TO_STDOUT) {
        sh_redir_t redir = { .kind = SH_REDIR_STDERR_TO_STDOUT };
        if (!sh_command_add_redir(&command, &redir)) {
          sh_set_error(lexer, "out of memory while parsing redirection");
          goto fail;
        }
        have_content = true;
        *lookahead = sh_lex_next(lexer);
        continue;
      }
      if (token.kind == SH_TOKEN_REDIR_STDIN ||
          token.kind == SH_TOKEN_REDIR_STDOUT ||
          token.kind == SH_TOKEN_REDIR_STDOUT_APPEND) {
        sh_redir_t redir = {
          .kind = token.kind == SH_TOKEN_REDIR_STDIN ? SH_REDIR_STDIN
            : token.kind == SH_TOKEN_REDIR_STDOUT ? SH_REDIR_STDOUT
            : SH_REDIR_STDOUT_APPEND,
        };
        sh_token_t target = sh_lex_next(lexer);
        if (target.kind != SH_TOKEN_WORD) {
          sh_set_error(lexer, "redirection requires a target word");
          sh_word_free(&target.word);
          goto fail;
        }
        redir.target = target.word;
        if (!sh_command_add_redir(&command, &redir)) {
          sh_set_error(lexer, "out of memory while parsing redirection");
          sh_word_free(&redir.target);
          goto fail;
        }
        have_content = true;
        *lookahead = sh_lex_next(lexer);
        continue;
      }
      *lookahead = token;
      break;
    }

    if (!have_content || command.word_count == 0) {
      sh_set_error(lexer, "expected a command");
      goto fail;
    }
    if (!sh_pipeline_add_command(pipeline, &command)) {
      sh_set_error(lexer, "out of memory while parsing pipeline");
      goto fail;
    }
    if (lookahead->kind != SH_TOKEN_PIPE) return true;
    *lookahead = sh_lex_next(lexer);
    if (lookahead->kind == SH_TOKEN_EOF || lookahead->kind == SH_TOKEN_SEQUENCE ||
        lookahead->kind == SH_TOKEN_AND || lookahead->kind == SH_TOKEN_OR ||
        lookahead->kind == SH_TOKEN_PIPE) {
      sh_set_error(lexer, "pipeline requires a command after '|'");
      return false;
    }
  }

fail:
  sh_command_free(&command);
  return false;
}

bool sh_parse_segments(
  const char *const *segments,
  const size_t *segment_lengths,
  size_t segment_count,
  sh_program_t *program,
  sh_parse_error_t *error
) {
  if (!program || !segments || !segment_lengths || segment_count == 0) return false;
  memset(program, 0, sizeof(*program));
  if (error) memset(error, 0, sizeof(*error));

  sh_lexer_t lexer = {
    .input = {
      .segments = segments,
      .lengths = segment_lengths,
      .count = segment_count,
    },
    .error = error,
  };

  sh_token_t token = sh_lex_next(&lexer);
  while (token.kind == SH_TOKEN_SEQUENCE) token = sh_lex_next(&lexer);
  sh_connector_t connector = SH_CONNECT_ALWAYS;

  while (token.kind != SH_TOKEN_EOF) {
    if (token.kind == SH_TOKEN_ERROR) goto fail;
    sh_clause_t clause = { .connector = connector };
    if (!sh_parse_pipeline(&lexer, &clause.pipeline, &token)) {
      sh_pipeline_free(&clause.pipeline);
      goto fail;
    }
    if (!sh_program_add_clause(program, &clause)) {
      sh_set_error(&lexer, "out of memory while parsing command list");
      sh_pipeline_free(&clause.pipeline);
      goto fail;
    }

    if (token.kind == SH_TOKEN_AND) connector = SH_CONNECT_AND;
    else if (token.kind == SH_TOKEN_OR) connector = SH_CONNECT_OR;
    else if (token.kind == SH_TOKEN_SEQUENCE) connector = SH_CONNECT_ALWAYS;
    else if (token.kind == SH_TOKEN_EOF) break;
    else {
      sh_set_error(&lexer, "unexpected token after command");
      goto fail;
    }

    token = sh_lex_next(&lexer);
    if (connector == SH_CONNECT_ALWAYS) {
      while (token.kind == SH_TOKEN_SEQUENCE) token = sh_lex_next(&lexer);
      if (token.kind == SH_TOKEN_EOF) break;
    } else if (token.kind == SH_TOKEN_EOF || token.kind == SH_TOKEN_SEQUENCE ||
               token.kind == SH_TOKEN_AND || token.kind == SH_TOKEN_OR) {
      sh_set_error(&lexer, "conditional operator requires a following command");
      goto fail;
    }
  }

  return true;

fail:
  sh_word_free(&token.word);
  sh_program_free(program);
  return false;
}
