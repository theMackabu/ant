
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "errors.h"
#include "internal.h"
#include "runtime.h"
#include "utils.h"

#include "silver/ast.h"
#include "modules/syntax.h"
#include "silver/ast_export.h"

typedef enum {
  SYNTAX_PARSE_UNAMBIGUOUS,
  SYNTAX_PARSE_MODULE,
  SYNTAX_PARSE_SCRIPT,
} syntax_parse_mode_t;

typedef struct {
  const char *filename;
  syntax_parse_mode_t parse_mode;
  ant_ts_source_mode_t ts_mode;
  bool locations;
} syntax_options_t;

static ant_value_t syntax_type_error(ant_t *js, const char *function, const char *message) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "ant:syntax %s() %s", function, message);
}

static ant_value_t syntax_read_options(
  ant_t *js,
  ant_value_t options,
  const char *function,
  bool for_parse,
  syntax_options_t *out
) {
  *out = (syntax_options_t){
    .filename = for_parse ? "<input>" : "input.ts",
    .parse_mode = SYNTAX_PARSE_UNAMBIGUOUS,
    .ts_mode = ANT_TS_SOURCE_AUTO,
  };

  if (vtype(options) == T_UNDEF) return js_mkundef();
  if (!is_object_type(options))
    return syntax_type_error(js, function, "options must be an object");

  ant_value_t filename = js_get(js, options, "filename");
  if (is_err(filename)) return filename;
  if (vtype(filename) != T_UNDEF) {
    if (vtype(filename) != T_STR)
      return syntax_type_error(js, function, "options.filename must be a string");
    out->filename = js_getstr(js, filename, NULL);
  }

  ant_value_t source_type = js_get(js, options, "sourceType");
  if (is_err(source_type)) return source_type;
  if (vtype(source_type) != T_UNDEF) {
    if (vtype(source_type) != T_STR)
      return syntax_type_error(js, function, "options.sourceType must be a string");
    size_t len = 0;
    const char *value = js_getstr(js, source_type, &len);
    if (for_parse) {
      if (len == 11 && memcmp(value, "unambiguous", 11) == 0)
        out->parse_mode = SYNTAX_PARSE_UNAMBIGUOUS;
      else if (len == 6 && memcmp(value, "module", 6) == 0)
        out->parse_mode = SYNTAX_PARSE_MODULE;
      else if (len == 6 && memcmp(value, "script", 6) == 0)
        out->parse_mode = SYNTAX_PARSE_SCRIPT;
      else return syntax_type_error(
        js, function,
        "options.sourceType must be 'unambiguous', 'module', or 'script'"
      );
    } else {
      if (len == 4 && memcmp(value, "auto", 4) == 0)
        out->ts_mode = ANT_TS_SOURCE_AUTO;
      else if (len == 6 && memcmp(value, "module", 6) == 0)
        out->ts_mode = ANT_TS_SOURCE_MODULE;
      else if (len == 6 && memcmp(value, "script", 6) == 0)
        out->ts_mode = ANT_TS_SOURCE_SCRIPT;
      else if (len == 8 && memcmp(value, "commonjs", 8) == 0)
        out->ts_mode = ANT_TS_SOURCE_COMMONJS;
      else return syntax_type_error(
        js, function,
        "options.sourceType must be 'auto', 'module', 'script', or 'commonjs'"
      );
    }
  }

  if (for_parse) {
    ant_value_t locations = js_get(js, options, "locations");
    if (is_err(locations)) return locations;
    if (vtype(locations) != T_UNDEF) {
      if (vtype(locations) != T_BOOL)
        return syntax_type_error(js, function, "options.locations must be a boolean");
      out->locations = locations == js_true;
    }
  }

  return js_mkundef();
}

static ant_value_t js_syntax_strip_types(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_STR)
    return syntax_type_error(js, "stripTypes", "source must be a string");

  syntax_options_t options;
  ant_value_t options_result = syntax_read_options(
    js,
    nargs > 1 ? args[1] : js_mkundef(),
    "stripTypes",
    false,
    &options
  );
  if (is_err(options_result)) return options_result;

  size_t source_len = 0;
  const char *source = js_getstr(js, args[0], &source_len);
  char *buffer = malloc(source_len + 1);
  if (!buffer)
    return js_mkerr_typed(js, JS_ERR_INTERNAL, "ant:syntax stripTypes() ran out of memory");
  memcpy(buffer, source, source_len);
  buffer[source_len] = '\0';

  size_t output_len = source_len;
  const char *detail = NULL;
  int status = transform_typescript(
    &buffer,
    source_len,
    options.filename,
    options.ts_mode,
    &output_len,
    &detail
  );

  if (status < 0) {
    ant_value_t error = js_mkerr_typed(
      js,
      JS_ERR_SYNTAX,
      "TypeScript strip failed: %s",
      detail ? detail : "unknown error"
    );
    js_set(js, error, "code", js_mkstr(
      js,
      "ERR_ANT_TYPESCRIPT_STRIP",
      strlen("ERR_ANT_TYPESCRIPT_STRIP")
    ));
    js_set(js, error, "filename", js_mkstr(js, options.filename, strlen(options.filename)));
    free(buffer);
    return error;
  }

  ant_value_t result = js_mkstr(js, buffer, output_len);
  free(buffer);
  return result;
}

static ant_value_t js_syntax_parse_javascript(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_STR)
    return syntax_type_error(js, "parseJavaScript", "source must be a string");

  syntax_options_t options;
  ant_value_t options_result = syntax_read_options(
    js,
    nargs > 1 ? args[1] : js_mkundef(),
    "parseJavaScript",
    true,
    &options
  );
  if (is_err(options_result)) return options_result;

  size_t source_len = 0;
  const char *source = js_getstr(js, args[0], &source_len);
  if (source_len > UINT32_MAX)
    return js_mkerr_typed(js, JS_ERR_RANGE, "ant:syntax source is too large to parse");

  const char *saved_filename = js->filename;
  js->filename = options.filename;
  js_clear_error_site(js);

  code_arena_mark_t mark = parse_arena_mark();
  bool strict = options.parse_mode == SYNTAX_PARSE_MODULE;
  sv_ast_t *program = sv_parse(js, source, (ant_offset_t)source_len, strict);
  if (!program) {
    parse_arena_rewind(mark);
    js->filename = saved_filename;
    if (js->thrown_exists) return mkval(T_ERR, 0);
    return js_mkerr_typed(js, JS_ERR_INTERNAL, "ant:syntax parser failed without an error");
  }

  bool has_module_syntax = (program->flags & FN_MODULE_SYNTAX) != 0;
  if (options.parse_mode == SYNTAX_PARSE_SCRIPT && has_module_syntax) {
    parse_arena_rewind(mark);
    js_set_error_site(js, source, (ant_offset_t)source_len, options.filename, 0, 1);
    ant_value_t error = js_mkerr_typed(
      js, JS_ERR_SYNTAX,
      "import/export syntax is not allowed when sourceType is 'script'"
    );
    js->filename = saved_filename;
    return error;
  }

  if (options.parse_mode == SYNTAX_PARSE_UNAMBIGUOUS && has_module_syntax) {
    parse_arena_rewind(mark);
    js_clear_error_site(js);
    program = sv_parse(js, source, (ant_offset_t)source_len, true);
    if (!program) {
      parse_arena_rewind(mark);
      js->filename = saved_filename;
      if (js->thrown_exists) return mkval(T_ERR, 0);
      return js_mkerr_typed(js, JS_ERR_INTERNAL, "ant:syntax parser failed without an error");
    }
  }

  const char *actual_source_type =
    options.parse_mode == SYNTAX_PARSE_MODULE || has_module_syntax ? "module" : "script";
  ant_value_t result = sv_ast_export_public(
    js,
    program,
    source,
    source_len,
    actual_source_type,
    options.locations
  );

  parse_arena_rewind(mark);
  js->filename = saved_filename;
  return result;
}

ant_value_t syntax_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);
  js_set(js, lib, "stripTypes", js_mkfun_arity(js_syntax_strip_types, 1));
  js_set(js, lib, "parseJavaScript", js_mkfun_arity(js_syntax_parse_javascript, 1));
  return lib;
}
