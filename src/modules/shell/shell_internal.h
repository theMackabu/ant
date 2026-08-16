#ifndef ANT_MODULES_SHELL_INTERNAL_H
#define ANT_MODULES_SHELL_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "types.h"

typedef enum {
  SH_QUOTE_NONE = 0,
  SH_QUOTE_SINGLE,
  SH_QUOTE_DOUBLE,
} sh_quote_t;

typedef enum {
  SH_PART_LITERAL = 0,
  SH_PART_INTERPOLATION,
} sh_part_kind_t;

typedef struct {
  sh_part_kind_t kind;
  sh_quote_t quote;
  char *text;
  size_t text_len;
  size_t interpolation;
} sh_word_part_t;

typedef struct {
  sh_word_part_t *parts;
  size_t part_count;
  size_t part_capacity;
} sh_word_t;

typedef enum {
  SH_REDIR_STDIN = 0,
  SH_REDIR_STDOUT,
  SH_REDIR_STDOUT_APPEND,
  SH_REDIR_STDERR_TO_STDOUT,
} sh_redir_kind_t;

typedef struct {
  sh_redir_kind_t kind;
  sh_word_t target;
} sh_redir_t;

typedef struct {
  sh_word_t *words;
  size_t word_count;
  size_t word_capacity;
  sh_redir_t *redirs;
  size_t redir_count;
  size_t redir_capacity;
} sh_command_t;

typedef struct {
  sh_command_t *commands;
  size_t command_count;
  size_t command_capacity;
} sh_pipeline_t;

typedef enum {
  SH_CONNECT_ALWAYS = 0,
  SH_CONNECT_AND,
  SH_CONNECT_OR,
} sh_connector_t;

typedef struct {
  sh_connector_t connector;
  sh_pipeline_t pipeline;
} sh_clause_t;

typedef struct {
  sh_clause_t *clauses;
  size_t clause_count;
  size_t clause_capacity;
} sh_program_t;

typedef struct {
  sv_func_t *func;
  sh_program_t program;
} sh_compiled_program_t;


typedef struct {
  size_t segment;
  size_t offset;
  char message[160];
} sh_parse_error_t;

bool sh_parse_segments(
  const char *const *segments,
  const size_t *segment_lengths,
  size_t segment_count,
  sh_program_t *program,
  sh_parse_error_t *error
);

void sh_program_free(sh_program_t *program);
enum { SH_COMPILED_PROGRAM_TAG = 0x53484346u }; // SHCF

char *sh_compile_program_source(
  const sh_program_t *program,
  size_t *source_len,
  sh_parse_error_t *error
);

char *sh_debug_program_plan_source(
  const sh_program_t *program,
  size_t *source_len
);

ant_value_t sh_runtime_begin(ant_t *js, ant_value_t *args, int nargs);
ant_value_t sh_runtime_arg(ant_t *js, ant_value_t *args, int nargs);
ant_value_t sh_runtime_word(ant_t *js, ant_value_t *args, int nargs);
ant_value_t sh_runtime_command(ant_t *js, ant_value_t *args, int nargs);
ant_value_t sh_runtime_redirect(ant_t *js, ant_value_t *args, int nargs);
ant_value_t sh_runtime_redirect_word(ant_t *js, ant_value_t *args, int nargs);
ant_value_t sh_runtime_submit(ant_t *js, ant_value_t *args, int nargs);
ant_value_t sh_runtime_finish(ant_t *js, ant_value_t *args, int nargs);
ant_value_t sh_runtime_context(ant_t *js, bool needs_accumulator);

#endif
