#include <compat.h> // IWYU pragma: keep

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <uthash.h>

#include "ant.h"
#include "runtime.h"
#include "modules/process.h"
#include "modules/symbol.h"

extern char **environ;

#define DEFAULT_MAX_LISTENERS 10
#define INITIAL_LISTENER_CAPACITY 4

typedef struct {
  jsval_t listener;
  bool once;
} ProcessEventListener;

typedef struct {
  char *event_type;
  ProcessEventListener *listeners;
  int listener_count;
  int listener_capacity;
  UT_hash_handle hh;
} ProcessEventType;

static int max_listeners = DEFAULT_MAX_LISTENERS;
static ProcessEventType *process_events = NULL;

#define SIGNAL_LIST \
  X(SIGHUP) X(SIGINT) X(SIGQUIT) X(SIGILL) X(SIGTRAP) X(SIGABRT) \
  X(SIGBUS) X(SIGFPE) X(SIGUSR1) X(SIGUSR2) X(SIGSEGV) X(SIGPIPE) \
  X(SIGALRM) X(SIGTERM) X(SIGCHLD) X(SIGCONT) X(SIGTSTP) X(SIGTTIN) \
  X(SIGTTOU) X(SIGURG) X(SIGXCPU) X(SIGXFSZ) X(SIGVTALRM) X(SIGPROF) \
  X(SIGWINCH) X(SIGIO) X(SIGSYS)

typedef struct {
  const char *name;
  int signum;
  UT_hash_handle hh_name;
  UT_hash_handle hh_num;
} SignalEntry;

static SignalEntry *signals_by_name = NULL;
static SignalEntry *signals_by_num = NULL;

static void init_signal_map(void) {
  static bool initialized = false;
  if (initialized) return;
  
  static SignalEntry entries[] = {
#define X(sig) { #sig, sig, {0}, {0} },
    SIGNAL_LIST
#undef X
  };
  
  for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
    HASH_ADD_KEYPTR(hh_name, signals_by_name, entries[i].name, strlen(entries[i].name), &entries[i]);
    HASH_ADD(hh_num, signals_by_num, signum, sizeof(int), &entries[i]);
  }
  
  initialized = true;
}

static int get_signal_number(const char *name) {
  init_signal_map();
  SignalEntry *entry = NULL;
  HASH_FIND(hh_name, signals_by_name, name, strlen(name), entry);
  return entry ? entry->signum : -1;
}

static const char *get_signal_name(int signum) {
  init_signal_map();
  SignalEntry *entry = NULL;
  HASH_FIND(hh_num, signals_by_num, &signum, sizeof(int), entry);
  return entry ? entry->name : NULL;
}

static ProcessEventType *find_or_create_event_type(const char *event_type) {
  ProcessEventType *evt = NULL;
  HASH_FIND_STR(process_events, event_type, evt);
  
  if (evt == NULL) {
    evt = malloc(sizeof(ProcessEventType));
    evt->event_type = strdup(event_type);
    evt->listener_count = 0;
    evt->listener_capacity = INITIAL_LISTENER_CAPACITY;
    evt->listeners = malloc(sizeof(ProcessEventListener) * evt->listener_capacity);
    HASH_ADD_KEYPTR(hh, process_events, evt->event_type, strlen(evt->event_type), evt);
  }
  
  return evt;
}

static bool ensure_listener_capacity(ProcessEventType *evt) {
  if (evt->listener_count >= evt->listener_capacity) {
    int new_capacity = evt->listener_capacity * 2;
    ProcessEventListener *new_listeners = realloc(evt->listeners, sizeof(ProcessEventListener) * new_capacity);
    if (!new_listeners) return false;
    evt->listeners = new_listeners;
    evt->listener_capacity = new_capacity;
  }
  return true;
}

static void check_listener_warning(const char *event) {
  ProcessEventType *evt = NULL;
  HASH_FIND_STR(process_events, event, evt);
  if (evt && evt->listener_count == max_listeners) fprintf(stderr, 
    "Warning: Possible EventEmitter memory leak detected. "
    "%d '%s' listeners added. Use process.setMaxListeners() to increase limit.\n",
    evt->listener_count, event
  );
}

static void emit_process_event(const char *event_type, jsval_t *args, int nargs) {
  if (!rt->js) return;
  
  ProcessEventType *evt = NULL;
  HASH_FIND_STR(process_events, event_type, evt);
  
  if (evt == NULL || evt->listener_count == 0) return;
  
  int i = 0;
  while (i < evt->listener_count) {
    ProcessEventListener *listener = &evt->listeners[i];
    js_call(rt->js, listener->listener, args, nargs);
    
    if (listener->once) {
      for (int j = i; j < evt->listener_count - 1; j++) {
        evt->listeners[j] = evt->listeners[j + 1];
      } evt->listener_count--;
    } else i++;
  }
}

static void process_signal_handler(int signum) {
  const char *name = get_signal_name(signum);
  if (name) {
    jsval_t sig_arg = js_mkstr(rt->js, name, strlen(name));
    emit_process_event(name, &sig_arg, 1);
  }
}

static jsval_t env_getter(ant_t *js, jsval_t obj, const char *key, size_t key_len) {  
  char *key_str = (char *)malloc(key_len + 1);
  if (!key_str) return js_mkundef();
  
  memcpy(key_str, key, key_len);
  key_str[key_len] = '\0';
  
  char *value = getenv(key_str);
  free(key_str);
  
  if (value == NULL) return js_mkundef();
  return js_mkstr(js, value, strlen(value));
}

static void load_dotenv_file(ant_t *js, jsval_t env_obj) {
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

static jsval_t process_exit(ant_t *js, jsval_t *args, int nargs) {
  int code = 0;
  
  if (nargs > 0 && js_type(args[0]) == JS_NUM) {
    code = (int)js_getnum(args[0]);
  }
  
  exit(code);
  return js_mkundef();
}

static jsval_t env_to_object(ant_t *js, jsval_t *args, int nargs) {
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

static jsval_t process_cwd(ant_t *js, jsval_t *args, int nargs) {
  char cwd[4096];
  if (getcwd(cwd, sizeof(cwd)) != NULL) {
    return js_mkstr(js, cwd, strlen(cwd));
  }
  return js_mkundef();
}

static jsval_t process_on(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "process.on requires 2 arguments");
  
  char *event = js_getstr(js, args[0], NULL);
  if (!event) return js_mkerr(js, "event must be a string");
  if (js_type(args[1]) != JS_FUNC) return js_mkerr(js, "listener must be a function");
  
  int signum = get_signal_number(event);
  if (signum > 0) {
    signal(signum, process_signal_handler);
  }
  
  ProcessEventType *evt = find_or_create_event_type(event);
  if (!ensure_listener_capacity(evt)) {
    return js_mkerr(js, "failed to allocate listener");
  }
  
  evt->listeners[evt->listener_count].listener = args[1];
  evt->listeners[evt->listener_count].once = false;
  evt->listener_count++;
  
  check_listener_warning(event);
  
  return js_get(js, js_glob(js), "process");
}

static jsval_t process_once(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "process.once requires 2 arguments");
  
  char *event = js_getstr(js, args[0], NULL);
  if (!event) return js_mkerr(js, "event must be a string");
  if (js_type(args[1]) != JS_FUNC) return js_mkerr(js, "listener must be a function");
  
  int signum = get_signal_number(event);
  if (signum > 0) {
    signal(signum, process_signal_handler);
  }
  
  ProcessEventType *evt = find_or_create_event_type(event);
  if (!ensure_listener_capacity(evt)) {
    return js_mkerr(js, "failed to allocate listener");
  }
  
  evt->listeners[evt->listener_count].listener = args[1];
  evt->listeners[evt->listener_count].once = true;
  evt->listener_count++;
  
  check_listener_warning(event);
  
  return js_get(js, js_glob(js), "process");
}

static jsval_t process_off(ant_t *js, jsval_t *args, int nargs) {
  jsval_t process_obj = js_get(js, js_glob(js), "process");
  if (nargs < 2) return process_obj;
  
  char *event = js_getstr(js, args[0], NULL);
  if (!event) return process_obj;
  
  ProcessEventType *evt = NULL;
  HASH_FIND_STR(process_events, event, evt);
  if (!evt) return process_obj;
  
  for (int i = 0; i < evt->listener_count; i++) {
    if (evt->listeners[i].listener == args[1]) {
      for (int j = i; j < evt->listener_count - 1; j++) {
        evt->listeners[j] = evt->listeners[j + 1];
      } evt->listener_count--;
      break;
    }
  }
  
  if (evt->listener_count == 0) {
    int signum = get_signal_number(event);
    if (signum > 0) signal(signum, SIG_DFL);
  }
  
  return process_obj;
}

static jsval_t process_remove_all_listeners(ant_t *js, jsval_t *args, int nargs) {
  jsval_t process_obj = js_get(js, js_glob(js), "process");
  
  if (nargs > 0 && js_type(args[0]) == JS_STR) {
    char *event = js_getstr(js, args[0], NULL);
    if (event) {
      ProcessEventType *evt = NULL;
      HASH_FIND_STR(process_events, event, evt);
      if (evt) {
        evt->listener_count = 0;
        int signum = get_signal_number(event);
        if (signum > 0) signal(signum, SIG_DFL);
      }
    }
  } else {
    ProcessEventType *evt, *tmp;
    HASH_ITER(hh, process_events, evt, tmp) {
      int signum = get_signal_number(evt->event_type);
      if (signum > 0) signal(signum, SIG_DFL);
      evt->listener_count = 0;
    }
  }
  
  return process_obj;
}

static jsval_t process_emit(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkfalse();
  
  char *event = js_getstr(js, args[0], NULL);
  if (!event) return js_mkfalse();
  
  emit_process_event(event, nargs > 1 ? &args[1] : NULL, nargs - 1);
  return js_mktrue();
}

static jsval_t process_listener_count(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mknum(0);
  
  char *event = js_getstr(js, args[0], NULL);
  if (!event) return js_mknum(0);
  
  ProcessEventType *evt = NULL;
  HASH_FIND_STR(process_events, event, evt);
  
  return js_mknum(evt ? evt->listener_count : 0);
}

static jsval_t process_set_max_listeners(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "setMaxListeners requires 1 argument");
  if (js_type(args[0]) != JS_NUM) return js_mkerr(js, "n must be a number");
  
  int n = (int)js_getnum(args[0]);
  if (n < 0) return js_mkerr(js, "n must be non-negative");
  
  max_listeners = n;
  return js_get(js, js_glob(js), "process");
}

static jsval_t process_get_max_listeners(ant_t *js, jsval_t *args, int nargs) {
  return js_mknum(max_listeners);
}

void init_process_module() {
  ant_t *js = rt->js;
  
  jsval_t process_obj = js_mkobj(js);
  jsval_t env_obj = js_mkobj(js);
  jsval_t argv_arr = js_mkarr(js);

  js_set(js, process_obj, "env", env_obj);
  js_set(js, process_obj, "exit", js_mkfun(process_exit));
  
  js_set(js, process_obj, "on", js_mkfun(process_on));
  js_set(js, process_obj, "addListener", js_mkfun(process_on));
  js_set(js, process_obj, "once", js_mkfun(process_once));
  js_set(js, process_obj, "off", js_mkfun(process_off));
  js_set(js, process_obj, "removeListener", js_mkfun(process_off));
  js_set(js, process_obj, "removeAllListeners", js_mkfun(process_remove_all_listeners));
  js_set(js, process_obj, "emit", js_mkfun(process_emit));
  js_set(js, process_obj, "listenerCount", js_mkfun(process_listener_count));
  js_set(js, process_obj, "setMaxListeners", js_mkfun(process_set_max_listeners));
  js_set(js, process_obj, "getMaxListeners", js_mkfun(process_get_max_listeners));
  
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

void process_gc_update_roots(GC_FWD_ARGS) {
  ProcessEventType *evt, *tmp;
  HASH_ITER(hh, process_events, evt, tmp) {
    for (int i = 0; i < evt->listener_count; i++)
      evt->listeners[i].listener = fwd_val(ctx, evt->listeners[i].listener);
  }
}
