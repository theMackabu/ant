#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <libgen.h>
#include <limits.h>

#include "runtime.h"
#include "modules/io.h"

#define ANSI_RED "\x1b[31m"
#define ANSI_YELLOW "\x1b[33m"
#define ANSI_RESET "\x1b[0m"

#define JSON_KEY "\x1b[36m"
#define JSON_STRING "\x1b[32m"
#define JSON_NUMBER "\x1b[33m"
#define JSON_BOOL "\x1b[35m"
#define JSON_NULL "\x1b[90m"
#define JSON_BRACE "\x1b[37m"

static void print_json_colored(const char *json, FILE *stream) {
  bool in_string = false;
  bool is_key = true;
  bool escape_next = false;
  
  for (const char *p = json; *p; p++) {
    if (escape_next) {
      fputc(*p, stream);
      escape_next = false;
      continue;
    }
    
    if (*p == '\\' && in_string) {
      fputc(*p, stream);
      escape_next = true;
      continue;
    }
    
    if (*p == '"') {
      if (!in_string) {
        fprintf(stream, "%s\"", is_key ? JSON_KEY : JSON_STRING);
        in_string = true;
      } else {
        fprintf(stream, "\"%s", ANSI_RESET);
        in_string = false;
      }
      continue;
    }
    
    if (in_string) {
      fputc(*p, stream);
      continue;
    }
    
    if (*p == ':') {
      fputc(*p, stream);
      is_key = false;
      continue;
    }
    
    if (*p == ',' || *p == '{' || *p == '[') {
      if (*p == '{' || *p == '[') {
        fprintf(stream, "%s%c%s", JSON_BRACE, *p, ANSI_RESET);
      } else {
        fputc(*p, stream);
      }
      is_key = (*p == ',' || *p == '{');
      continue;
    }
    
    if (*p == '}' || *p == ']') {
      fprintf(stream, "%s%c%s", JSON_BRACE, *p, ANSI_RESET);
      continue;
    }
    
    if (strncmp(p, "true", 4) == 0) {
      fprintf(stream, "%strue%s", JSON_BOOL, ANSI_RESET);
      p += 3;
      continue;
    }
    
    if (strncmp(p, "false", 5) == 0) {
      fprintf(stream, "%sfalse%s", JSON_BOOL, ANSI_RESET);
      p += 4;
      continue;
    }
    
    if (strncmp(p, "null", 4) == 0) {
      fprintf(stream, "%snull%s", JSON_NULL, ANSI_RESET);
      p += 3;
      continue;
    }
    
    if ((*p >= '0' && *p <= '9') || *p == '-') {
      fprintf(stream, "%s", JSON_NUMBER);
      while ((*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '+' || *p == 'e' || *p == 'E') {
        fputc(*p, stream);
        if (!(*(p + 1) >= '0' && *(p + 1) <= '9') && *(p + 1) != '.' && *(p + 1) != '-' && *(p + 1) != '+' && *(p + 1) != 'e' && *(p + 1) != 'E') break;
        p++;
      }
      fprintf(stream, "%s", ANSI_RESET);
      continue;
    }
    
    fputc(*p, stream);
  }
}

static void console_print(struct js *js, jsval_t *args, int nargs, const char *color, FILE *stream) {
  if (color) fprintf(stream, "%s", color);
  
  for (int i = 0; i < nargs; i++) {
    const char *space = i == 0 ? "" : " ";
    fprintf(stream, "%s", space);
    
    if (js_type(args[i]) == JS_STR) {
      char *str = js_getstr(js, args[i], NULL);
      fprintf(stream, "%s", str);
    } else {
      const char *str = js_str(js, args[i]);
      if (str[0] == '{' || str[0] == '[') {
        if (color) fprintf(stream, "%s", ANSI_RESET);
        print_json_colored(str, stream);
        if (color) fprintf(stream, "%s", color);
      } else {
        fprintf(stream, "%s", str);
      }
    }
  }
  
  if (color) fprintf(stream, "%s", ANSI_RESET);
  fputc('\n', stream);
}

static jsval_t js_console_log(struct js *js, jsval_t *args, int nargs) {
  console_print(js, args, nargs, NULL, stdout);
  return js_mkundef();
}

static jsval_t js_console_error(struct js *js, jsval_t *args, int nargs) {
  console_print(js, args, nargs, ANSI_RED, stderr);
  return js_mkundef();
}

static jsval_t js_console_warn(struct js *js, jsval_t *args, int nargs) {
  console_print(js, args, nargs, ANSI_YELLOW, stderr);
  return js_mkundef();
}

void init_console_module() {
  struct js *js = rt->js;
  jsval_t console_obj = js_mkobj(js);
  
  js_set(js, js_glob(js), "console", console_obj);
  js_set(js, console_obj, "log", js_mkfun(js_console_log));
  js_set(js, console_obj, "error", js_mkfun(js_console_error));
  js_set(js, console_obj, "warn", js_mkfun(js_console_warn));
}