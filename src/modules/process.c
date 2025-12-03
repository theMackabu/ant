#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ant.h"
#include "runtime.h"
#include "modules/process.h"

extern char **environ;

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

void init_process_module(void) {
  struct js *js = rt->js;
  
  jsval_t process_obj = js_mkobj(js);
  jsval_t env_obj = js_mkobj(js);

  js_set(js, js_glob(js), "process", process_obj);
  js_set(js, process_obj, "env", env_obj);
  js_set(js, process_obj, "exit", js_mkfun(process_exit));
  
  for (char **env = environ; *env != NULL; env++) {
    char *env_copy = strdup(*env);
    if (env_copy == NULL) continue;
    
    char *equals = strchr(env_copy, '=');
    if (equals != NULL) {
      *equals = '\0';
      char *key = env_copy;
      char *value = equals + 1;
      
      js_set(js, env_obj, key, js_mkstr(js, value, strlen(value)));
    }
    
    free(env_copy);
  }
  
  load_dotenv_file(js, env_obj);
}
