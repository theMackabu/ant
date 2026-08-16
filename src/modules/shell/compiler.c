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
    sh_source_cstr(&source,
      "return {stdout:\"\",stderr:\"\",exitCode:0,signalCode:null};"
    );
    goto done;
  }
  
  if (program->clause_count == 1) {
    sh_source_cstr(&source, "return await __run(__ctx,__plan,0,__values);");
    goto done;
  }

  sh_source_cstr(&source, "let __out=\"\",__err=\"\",__result;");
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
      "__out+=__result.stdout;__err+=__result.stderr;"
      "if(__result.exited)return {stdout:__out,stderr:__err,"
      "exitCode:__result.exitCode,signalCode:__result.signalCode};"
    );

    if (clause->connector != SH_CONNECT_ALWAYS) sh_source_cstr(&source, "}");
  }

  sh_source_cstr(&source,
    "return {stdout:__out,stderr:__err,exitCode:__result.exitCode,"
    "signalCode:__result.signalCode};"
  );

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
