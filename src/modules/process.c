#include <compat.h> // IWYU pragma: keep

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ant.h"
#include "runtime.h"
#include "modules/process.h"
#include "modules/symbol.h"

extern char **environ;

static jsval_t env_getter(struct js *js, jsval_t obj, const char *key, size_t key_len) {
  (void)obj;
  
  char *key_str = (char *)malloc(key_len + 1);
  if (!key_str) return js_mkundef();
  
  memcpy(key_str, key, key_len);
  key_str[key_len] = '\0';
  
  char *value = getenv(key_str);
  free(key_str);
  
  if (value == NULL) return js_mkundef();
  return js_mkstr(js, value, strlen(value));
}

static void load_dotenv_file(struct js *js, jsval_t env_obj) {
  FILE *fp = fopen(".env", "r");
  if (fp == NULL) return;
  
  char line[1024];
  while (fgets(line, sizeof(line), fp) != NULL) {
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') {
      line[len - 1] = '\0';
      len--;
    }
    if (len > 0 && line[len - 1] == '\r') {
      line[len - 1] = '\0';
      len--;
    }
    
    if (len == 0 || line[0] == '#') continue;
    char *equals = strchr(line, '=');
    if (equals == NULL) continue;
    
    *equals = '\0';
    char *key = line;
    char *value = equals + 1;
    
    while (*key == ' ' || *key == '\t') key++;
    char *key_end = key + strlen(key) - 1;
    while (key_end > key && (*key_end == ' ' || *key_end == '\t')) {
      *key_end = '\0';
      key_end--;
    }
    
    while (*value == ' ' || *value == '\t') value++;
    char *value_end = value + strlen(value) - 1;
    while (value_end > value && (*value_end == ' ' || *value_end == '\t')) {
      *value_end = '\0';
      value_end--;
    }
    
    if (strlen(value) >= 2 && 
        ((value[0] == '"' && value[strlen(value) - 1] == '"') 
        || (value[0] == '\'' && value[strlen(value) - 1] == '\''))) {
      value[strlen(value) - 1] = '\0';
      value++;
    }
    
    js_set(js, env_obj, key, js_mkstr(js, value, strlen(value)));
  }
  
  fclose(fp);
}

static jsval_t process_exit(struct js *js, jsval_t *args, int nargs) {
  int code = 0;
  
  if (nargs > 0 && js_type(args[0]) == JS_NUM) {
    code = (int)js_getnum(args[0]);
  }
  
  exit(code);
  return js_mkundef();
}

static jsval_t env_to_object(struct js *js, jsval_t *args, int nargs) {
  (void)args;
  (void)nargs;
  
  jsval_t obj = js_mkobj(js);
  
  for (char **env = environ; *env != NULL; env++) {
    char *entry = *env;
    char *equals = strchr(entry, '=');
    if (equals == NULL) continue;
    
    size_t key_len = (size_t)(equals - entry);
    char *value = equals + 1;
    
    char *key = malloc(key_len + 1);
    if (!key) continue;
    memcpy(key, entry, key_len);
    key[key_len] = '\0';
    
    js_set(js, obj, key, js_mkstr(js, value, strlen(value)));
    free(key);
  }
  
  return obj;
}

static jsval_t process_cwd(struct js *js, jsval_t *args, int nargs) {
  (void)args;
  (void)nargs;
  
  char cwd[4096];
  if (getcwd(cwd, sizeof(cwd)) != NULL) {
    return js_mkstr(js, cwd, strlen(cwd));
  }
  return js_mkundef();
}

void init_process_module() {
  struct js *js = rt->js;
  
  jsval_t process_obj = js_mkobj(js);
  jsval_t env_obj = js_mkobj(js);
  jsval_t argv_arr = js_mkarr(js);

  js_set(js, process_obj, "env", env_obj);
  js_set(js, process_obj, "exit", js_mkfun(process_exit));
  
  // process.pid
  js_set(js, process_obj, "pid", js_mknum((double)getpid()));
  
  // process.platform
  #if defined(__APPLE__)
    js_set(js, process_obj, "platform", js_mkstr(js, "darwin", 6));
  #elif defined(__linux__)
    js_set(js, process_obj, "platform", js_mkstr(js, "linux", 5));
  #elif defined(_WIN32) || defined(_WIN64)
    js_set(js, process_obj, "platform", js_mkstr(js, "win32", 5));
  #elif defined(__FreeBSD__)
    js_set(js, process_obj, "platform", js_mkstr(js, "freebsd", 7));
  #else
    js_set(js, process_obj, "platform", js_mkstr(js, "unknown", 7));
  #endif
  
  // process.arch
  #if defined(__x86_64__) || defined(_M_X64)
    js_set(js, process_obj, "arch", js_mkstr(js, "x64", 3));
  #elif defined(__i386__) || defined(_M_IX86)
    js_set(js, process_obj, "arch", js_mkstr(js, "ia32", 4));
  #elif defined(__aarch64__) || defined(_M_ARM64)
    js_set(js, process_obj, "arch", js_mkstr(js, "arm64", 5));
  #elif defined(__arm__) || defined(_M_ARM)
    js_set(js, process_obj, "arch", js_mkstr(js, "arm", 3));
  #else
    js_set(js, process_obj, "arch", js_mkstr(js, "unknown", 7));
  #endif
  
  load_dotenv_file(js, env_obj);
  js_set_getter(js, env_obj, env_getter);
  js_set(js, env_obj, "toObject", js_mkfun(env_to_object));
  
  for (int i = 0; i < rt->argc; i++) {
    js_arr_push(js, argv_arr, js_mkstr(js, rt->argv[i], strlen(rt->argv[i])));
  }
  
  js_set(js, process_obj, "argv", argv_arr);
  js_set(js, process_obj, "cwd", js_mkfun(process_cwd));
  
  js_set(js, process_obj, get_toStringTag_sym_key(), js_mkstr(js, "process", 7));
  js_set(js, js_glob(js), "process", process_obj);
}
