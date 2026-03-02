#ifndef SILVER_AST_H
#define SILVER_AST_H

#include <stdint.h>
#include <stdbool.h>
#include "types.h"

typedef enum {
  N_NUMBER,
  N_STRING,
  N_BIGINT,
  N_BOOL,
  N_NULL,
  N_UNDEF,
  N_THIS,
  N_GLOBAL_THIS,
  N_TEMPLATE,
  N_REGEXP,
  N_IDENT,
  N_BINARY,
  N_UNARY,
  N_UPDATE,
  N_ASSIGN,
  N_TERNARY,
  N_CALL,
  N_NEW,
  N_MEMBER,
  N_OPTIONAL,
  N_ARRAY,
  N_OBJECT,
  N_PROPERTY,
  N_SPREAD,
  N_SEQUENCE,
  N_ARROW,
  N_YIELD,
  N_AWAIT,
  N_TYPEOF,
  N_DELETE,
  N_VOID,
  N_TAGGED_TEMPLATE,
  N_BLOCK,
  N_VAR,
  N_VARDECL,
  N_IF,
  N_WHILE,
  N_DO_WHILE,
  N_FOR,
  N_FOR_IN,
  N_FOR_OF,
  N_FOR_AWAIT_OF,
  N_RETURN,
  N_BREAK,
  N_CONTINUE,
  N_THROW,
  N_TRY,
  N_SWITCH,
  N_CASE,
  N_LABEL,
  N_DEBUGGER,
  N_EMPTY,
  N_WITH,
  N_FUNC,
  N_CLASS,
  N_METHOD,
  N_STATIC_BLOCK,
  N_ARRAY_PAT,
  N_OBJECT_PAT,
  N_REST,
  N_ASSIGN_PAT,
  N_NEW_TARGET,
  N_IMPORT,
  N_IMPORT_DECL,
  N_IMPORT_SPEC,
  N_EXPORT,
  N_PROGRAM,
  N__COUNT
} sv_node_type_t;

typedef enum {
  SV_VAR_VAR,
  SV_VAR_LET_ASN,
  SV_VAR_CONST,
} sv_var_kind_t;

enum {
  FN_ASYNC           = 1 << 0,
  FN_GENERATOR       = 1 << 1,
  FN_ARROW           = 1 << 2,
  FN_GETTER          = 1 << 3,
  FN_SETTER          = 1 << 4,
  FN_STATIC          = 1 << 5,
  FN_COMPUTED        = 1 << 6,
  FN_METHOD          = 1 << 7,
  FN_COLON           = 1 << 8,
  FN_PAREN           = 1 << 9,
  FN_USES_ARGS       = 1 << 10,
  FN_INVALID_COOKED  = 1 << 11,
  FN_PARSE_STRICT    = 1 << 12,
};

enum {
  EX_DEFAULT   = 1 << 0,
  EX_DECL      = 1 << 1,
  EX_NAMED     = 1 << 2,
  EX_FROM      = 1 << 3,
  EX_STAR      = 1 << 4,
  EX_NAMESPACE = 1 << 5,
};

typedef struct 
  sv_ast sv_ast_t;

typedef struct {
  sv_ast_t **items;
  int        count;
  int        cap;
} sv_ast_list_t;

struct sv_ast {
  sv_node_type_t  type;
  uint8_t         op;
  uint16_t        flags;
  sv_var_kind_t   var_kind;

  const char     *str;
  uint32_t        len;
  const char     *aux;
  uint32_t        aux_len;
  double          num;

  sv_ast_t       *left;
  sv_ast_t       *right;
  sv_ast_t       *cond;
  sv_ast_t       *body;
  sv_ast_list_t   args;

  sv_ast_t       *catch_param;
  sv_ast_t       *catch_body;
  sv_ast_t       *finally_body;

  sv_ast_t       *init;
  sv_ast_t       *update;

  uint32_t        line;
  uint32_t        col;
  uint32_t        src_off;
  uint32_t        src_end;
};

void sv_ast_list_push(sv_ast_list_t *list, sv_ast_t *node);
bool sv_ast_can_be_expression_statement(const sv_ast_t *node);
bool ast_references_arguments(const sv_ast_t *node);

sv_ast_t *sv_ast_new(sv_node_type_t type);
sv_ast_t *sv_parse(ant_t *js, const char *code, jsoff_t clen, bool strict);

#endif
