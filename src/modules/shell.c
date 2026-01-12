#include <compat.h> // IWYU pragma: keep

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#else
#include <sys/wait.h>
#endif

#include "ant.h"
#include "modules/symbol.h"

static jsval_t builtin_shell_text(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_shell_lines(struct js *js, jsval_t *args, int nargs);

static jsval_t shell_exec(struct js *js, const char *cmd, size_t cmd_len) {
  jsval_t result = js_mkobj(js);
  
  FILE *fp = popen(cmd, "r");
  if (!fp) {
    js_set(js, result, "stdout", js_mkstr(js, "", 0));
    js_set(js, result, "stderr", js_mkstr(js, "Failed to execute command", 25));
    js_set(js, result, "exitCode", js_mknum(1));
    return result;
  }
  
  char *output = NULL;
  size_t output_size = 0;
  size_t output_capacity = 4096;
  output = malloc(output_capacity);
  
  if (!output) {
    pclose(fp);
    return js_mkerr(js, "Out of memory");
  }
  
  char buffer[4096];
  while (fgets(buffer, sizeof(buffer), fp) != NULL) {
    size_t len = strlen(buffer);
    if (output_size + len >= output_capacity) {
      output_capacity *= 2;
      char *new_output = realloc(output, output_capacity);
      if (!new_output) {
        free(output);
        pclose(fp);
        return js_mkerr(js, "Out of memory");
      }
      output = new_output;
    }
    memcpy(output + output_size, buffer, len);
    output_size += len;
  }
  
  int status = pclose(fp);
#ifdef _WIN32
  int exit_code = status;
#else
  int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
  if (output_size > 0 && output[output_size - 1] == '\n') output_size--;
  
  jsval_t stdout_val = js_mkstr(js, output, output_size);
  free(output);
  
  js_set(js, result, "exitCode", js_mknum(exit_code));
  js_set(js, result, "text", js_heavy_mkfun(js, builtin_shell_text, stdout_val));
  js_set(js, result, "lines", js_heavy_mkfun(js, builtin_shell_lines, stdout_val));
  
  return result;
}

static jsval_t builtin_shell_text(struct js *js, jsval_t *args, int nargs) {
  (void)args;
  (void)nargs;
  
  jsval_t fn = js_getcurrentfunc(js);
  return js_get_slot(js, fn, SLOT_DATA);
}

static jsval_t builtin_shell_lines(struct js *js, jsval_t *args, int nargs) {
  (void)args;
  (void)nargs;
  
  jsval_t fn = js_getcurrentfunc(js);
  jsval_t stdout_val = js_get_slot(js, fn, SLOT_DATA);
  
  size_t text_len;
  char *text = js_getstr(js, stdout_val, &text_len);
  if (!text) return js_mkarr(js);
  
  jsval_t lines_array = js_mkarr(js);
  size_t line_start = 0;
  
  for (size_t i = 0; i <= text_len; i++) {
    if (i == text_len || text[i] == '\n') {
      size_t line_len = i - line_start;
      jsval_t line_val = js_mkstr(js, text + line_start, line_len);
      js_arr_push(js, lines_array, line_val);
      line_start = i + 1;
    }
  }
  
  return lines_array;
}

static jsval_t builtin_shell_dollar(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "$() requires at least one argument");
    
  if (js_type(args[0]) != JS_OBJ) {
    if (js_type(args[0]) == JS_STR) {
      size_t cmd_len;
      char *cmd = js_getstr(js, args[0], &cmd_len);
      if (!cmd) return js_mkerr(js, "Failed to get command string");
      return shell_exec(js, cmd, cmd_len);
    }
    
    return js_mkerr(js, "$() requires a template string");
  }
  
  jsval_t strings_array = args[0];
  
  char *command = malloc(4096);
  if (!command) return js_mkerr(js, "Out of memory");
  
  size_t cmd_pos = 0;
  size_t cmd_capacity = 4096;
  
  jsval_t length_val = js_get(js, strings_array, "length");
  int length = (int)js_getnum(length_val);
  
  for (int i = 0; i < length; i++) {
    char idx_str[32];
    snprintf(idx_str, sizeof(idx_str), "%d", i);
    
    jsval_t str_val = js_get(js, strings_array, idx_str);
    if (js_type(str_val) == JS_STR) {
      size_t str_len;
      char *str = js_getstr(js, str_val, &str_len);
      
      if (cmd_pos + str_len >= cmd_capacity) {
        cmd_capacity *= 2;
        char *new_cmd = realloc(command, cmd_capacity);
        if (!new_cmd) {
          free(command);
          return js_mkerr(js, "Out of memory");
        }
        command = new_cmd;
      }
      
      memcpy(command + cmd_pos, str, str_len);
      cmd_pos += str_len;
    }
    
    if (i + 1 < nargs) {
      jsval_t val = args[i + 1];
      char val_str[256];
      size_t val_len = 0;
      
      if (js_type(val) == JS_STR) {
        size_t len;
        char *s = js_getstr(js, val, &len);
        val_len = len < sizeof(val_str) - 1 ? len : sizeof(val_str) - 1;
        memcpy(val_str, s, val_len);
      } else if (js_type(val) == JS_NUM) {
        val_len = snprintf(val_str, sizeof(val_str), "%g", js_getnum(val));
      }
      
      if (cmd_pos + val_len >= cmd_capacity) {
        cmd_capacity *= 2;
        char *new_cmd = realloc(command, cmd_capacity);
        if (!new_cmd) {
          free(command);
          return js_mkerr(js, "Out of memory");
        }
        command = new_cmd;
      }
      
      memcpy(command + cmd_pos, val_str, val_len);
      cmd_pos += val_len;
    }
  }
  
  command[cmd_pos] = '\0';
  
  jsval_t result = shell_exec(js, command, cmd_pos);
  free(command);
  
  return result;
}

jsval_t shell_library(struct js *js) {
  jsval_t lib = js_mkobj(js);
  
  js_set(js, lib, "$", js_mkfun(builtin_shell_dollar));
  js_set(js, lib, get_toStringTag_sym_key(), js_mkstr(js, "shell", 5));
  
  return lib;
}
