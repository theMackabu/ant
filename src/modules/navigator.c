#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <uthash.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#endif

#include "ant.h"
#include "config.h"
#include "runtime.h"
#include "modules/navigator.h"
#include "modules/symbol.h"

typedef enum {
  LOCK_MODE_EXCLUSIVE,
  LOCK_MODE_SHARED
} lock_mode_t;

typedef struct lock_entry {
  char *name;
  lock_mode_t mode;
  int shared_count;
  UT_hash_handle hh;
} lock_entry_t;

typedef struct lock_request {
  struct js *js;
  char *name;
  lock_mode_t mode;
  jsval_t callback;
  jsval_t promise;
  struct lock_request *next;
} lock_request_t;

static lock_entry_t *locks = NULL;
static lock_request_t *pending_requests = NULL;

static int get_hardware_concurrency(void) {
#ifdef __APPLE__
  int count;
  size_t size = sizeof(count);
  if (sysctlbyname("hw.ncpu", &count, &size, NULL, 0) == 0) {
    return count;
  }
  return 1;
#elif defined(__linux__)
  int count = (int)sysconf(_SC_NPROCESSORS_ONLN);
  return count > 0 ? count : 1;
#elif defined(_WIN32) || defined(_WIN64)
  SYSTEM_INFO sysinfo;
  GetSystemInfo(&sysinfo);
  return (int)sysinfo.dwNumberOfProcessors;
#else
  return 1;
#endif
}

static const char *get_platform_string(void) {
#if defined(__APPLE__)
  return "MacIntel";
#elif defined(__linux__)
  return "Linux x86_64";
#elif defined(_WIN32) || defined(_WIN64)
  return "Win32";
#elif defined(__FreeBSD__)
  return "FreeBSD";
#else
  return "Unknown";
#endif
}

static lock_entry_t *find_lock(const char *name) {
  lock_entry_t *entry = NULL;
  HASH_FIND_STR(locks, name, entry);
  return entry;
}

static lock_entry_t *create_lock(const char *name, lock_mode_t mode) {
  lock_entry_t *entry = calloc(1, sizeof(lock_entry_t));
  if (!entry) return NULL;
  
  entry->name = strdup(name);
  entry->mode = mode;
  entry->shared_count = (mode == LOCK_MODE_SHARED) ? 1 : 0;
  
  HASH_ADD_STR(locks, name, entry);
  return entry;
}

static bool can_acquire_lock(const char *name, lock_mode_t mode) {
  lock_entry_t *entry = find_lock(name);
  if (!entry) return true;
  if (mode == LOCK_MODE_SHARED && entry->mode == LOCK_MODE_SHARED) return true;
  return false;
}

static void release_lock(const char *name) {
  lock_entry_t *entry = find_lock(name);
  if (!entry) return;
  
  if (entry->mode == LOCK_MODE_SHARED) {
    entry->shared_count--;
    if (entry->shared_count > 0) return;
  }
  
  HASH_DEL(locks, entry);
  free(entry->name);
  free(entry);
}

static void process_pending_requests(struct js *js);

static jsval_t lock_then_handler(struct js *js, jsval_t *args, int nargs) {
  jsval_t current_func = js_getcurrentfunc(js);
  jsval_t lock_name_val = js_get(js, current_func, "_lockName");
  jsval_t outer_promise = js_get(js, current_func, "_outerPromise");
  jsval_t result_val = (nargs > 0) ? args[0] : js_mkundef();
  
  size_t name_len;
  char *name = js_getstr(js, lock_name_val, &name_len);
  
  if (name) release_lock(name);
  
  js_resolve_promise(js, outer_promise, result_val);
  process_pending_requests(js);
  
  return js_mkundef();
}

static jsval_t lock_catch_handler(struct js *js, jsval_t *args, int nargs) {
  jsval_t current_func = js_getcurrentfunc(js);
  jsval_t lock_name_val = js_get(js, current_func, "_lockName");
  jsval_t outer_promise = js_get(js, current_func, "_outerPromise");
  jsval_t error_val = (nargs > 0) ? args[0] : js_mkundef();
  
  size_t name_len;
  char *name = js_getstr(js, lock_name_val, &name_len);
  
  if (name) release_lock(name);
  
  js_reject_promise(js, outer_promise, error_val);
  process_pending_requests(js);
  
  return js_mkundef();
}

static void execute_lock_callback(struct js *js, const char *name, lock_mode_t mode, jsval_t callback, jsval_t outer_promise) {
  jsval_t lock_obj = js_mkobj(js);
  js_set(js, lock_obj, "name", js_mkstr(js, name, strlen(name)));
  js_set(js, lock_obj, "mode", js_mkstr(js, mode == LOCK_MODE_EXCLUSIVE ? "exclusive" : "shared", mode == LOCK_MODE_EXCLUSIVE ? 9 : 6));
  js_set(js, lock_obj, get_toStringTag_sym_key(), js_mkstr(js, "Lock", 4));
  
  jsval_t result = js_call(js, callback, &lock_obj, 1);
  
  if (js_type(result) == JS_ERR) {
    release_lock(name);
    js_reject_promise(js, outer_promise, result);
    process_pending_requests(js);
    return;
  }
  
  if (js_type(result) == JS_PROMISE) {
    jsval_t then_fn = js_get(js, result, "then");
    
    if (js_type(then_fn) == JS_FUNC) {
      jsval_t name_str = js_mkstr(js, name, strlen(name));
      
      jsval_t on_resolve = js_mkfun(lock_then_handler);
      js_set(js, on_resolve, "_lockName", name_str);
      js_set(js, on_resolve, "_outerPromise", outer_promise);
      
      jsval_t on_reject = js_mkfun(lock_catch_handler);
      js_set(js, on_reject, "_lockName", name_str);
      js_set(js, on_reject, "_outerPromise", outer_promise);
      
      jsval_t then_args[2] = { on_resolve, on_reject };
      js_call_with_this(js, then_fn, result, then_args, 2);
      return;
    }
  }
  
  release_lock(name);
  js_resolve_promise(js, outer_promise, result);
  process_pending_requests(js);
}

static void process_pending_requests(struct js *js) {
  lock_request_t *prev = NULL;
  lock_request_t *req = pending_requests;
  
  while (req) {
    if (can_acquire_lock(req->name, req->mode)) {
      lock_entry_t *entry = find_lock(req->name);
      
      if (entry && entry->mode == LOCK_MODE_SHARED && req->mode == LOCK_MODE_SHARED) {
        entry->shared_count++;
      } else {
        create_lock(req->name, req->mode);
      }
      
      lock_request_t *to_process = req;
      
      if (prev) {
        prev->next = req->next;
      } else {
        pending_requests = req->next;
      }
      req = req->next;
      
      execute_lock_callback(js, to_process->name, to_process->mode, to_process->callback, to_process->promise);
      free(to_process->name);
      free(to_process);
      return;
    } else {
      prev = req;
      req = req->next;
    }
  }
}

static jsval_t locks_request(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "locks.request requires at least 2 arguments");
  }
  
  size_t name_len;
  char *name = js_getstr(js, args[0], &name_len);
  if (!name) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "First argument must be a string");
  }
  
  jsval_t options = js_mkundef();
  jsval_t callback;
  lock_mode_t mode = LOCK_MODE_EXCLUSIVE;
  bool if_available = false;
  
  if (nargs == 2) {
    callback = args[1];
  } else {
    options = args[1];
    callback = args[2];
    
    if (js_type(options) == JS_OBJ) {
      jsval_t mode_val = js_get(js, options, "mode");
      if (js_type(mode_val) == JS_STR) {
        size_t mode_len;
        char *mode_str = js_getstr(js, mode_val, &mode_len);
        if (mode_str && strcmp(mode_str, "shared") == 0) mode = LOCK_MODE_SHARED;
      }
      
      jsval_t if_avail_val = js_get(js, options, "ifAvailable");
      if (js_type(if_avail_val) == JS_TRUE) if_available = true;
    }
  }
  
  if (js_type(callback) != JS_FUNC) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Callback must be a function");
  }
  
  jsval_t promise = js_mkpromise(js);
  
  if (if_available && !can_acquire_lock(name, mode)) {
    jsval_t null_val = js_mknull();
    jsval_t result = js_call(js, callback, &null_val, 1);
    
    if (js_type(result) == JS_PROMISE) {
      jsval_t then_fn = js_get(js, result, "then");
      if (js_type(then_fn) == JS_FUNC) {
        jsval_t on_resolve = js_mkfun(lock_then_handler);
        js_set(js, on_resolve, "_lockName", js_mkstr(js, "", 0));
        js_set(js, on_resolve, "_outerPromise", promise);
        js_call_with_this(js, then_fn, result, &on_resolve, 1);
        return promise;
      }
    }
    js_resolve_promise(js, promise, result);
    return promise;
  }
  
  if (can_acquire_lock(name, mode)) {
    lock_entry_t *entry = find_lock(name);
    
    if (entry && entry->mode == LOCK_MODE_SHARED && mode == LOCK_MODE_SHARED) {
      entry->shared_count++;
    } else {
      create_lock(name, mode);
    }
    
    execute_lock_callback(js, name, mode, callback, promise);
  } else {
    lock_request_t *req = calloc(1, sizeof(lock_request_t));
    req->js = js;
    req->name = strdup(name);
    req->mode = mode;
    req->callback = callback;
    req->promise = promise;
    req->next = NULL;
    
    lock_request_t *tail = pending_requests;
    if (!tail) {
      pending_requests = req;
    } else {
      while (tail->next) tail = tail->next;
      tail->next = req;
    }
  }
  
  return promise;
}

static jsval_t locks_query(struct js *js, jsval_t *args, int nargs) {
  (void)args;
  (void)nargs;
  
  jsval_t result = js_mkobj(js);
  jsval_t held_arr = js_mkarr(js);
  jsval_t pending_arr = js_mkarr(js);
  
  lock_entry_t *entry, *tmp;
  HASH_ITER(hh, locks, entry, tmp) {
    jsval_t lock_info = js_mkobj(js);
    js_set(js, lock_info, "name", js_mkstr(js, entry->name, strlen(entry->name)));
    js_set(js, lock_info, "mode", js_mkstr(js, entry->mode == LOCK_MODE_EXCLUSIVE ? "exclusive" : "shared", entry->mode == LOCK_MODE_EXCLUSIVE ? 9 : 6));
    js_arr_push(js, held_arr, lock_info);
  }
  
  lock_request_t *req = pending_requests;
  while (req) {
    jsval_t req_info = js_mkobj(js);
    js_set(js, req_info, "name", js_mkstr(js, req->name, strlen(req->name)));
    js_set(js, req_info, "mode", js_mkstr(js, req->mode == LOCK_MODE_EXCLUSIVE ? "exclusive" : "shared", req->mode == LOCK_MODE_EXCLUSIVE ? 9 : 6));
    js_arr_push(js, pending_arr, req_info);
    req = req->next;
  }
  
  js_set(js, result, "held", held_arr);
  js_set(js, result, "pending", pending_arr);
  
  jsval_t promise = js_mkpromise(js);
  js_resolve_promise(js, promise, result);
  
  return promise;
}

void init_navigator_module(void) {
  struct js *js = rt->js;
  
  jsval_t navigator_obj = js_mkobj(js);
  
  js_set(js, navigator_obj, "hardwareConcurrency", js_mknum((double)get_hardware_concurrency()));
  js_set(js, navigator_obj, "language", js_mkstr(js, "en-US", 5));
  
  jsval_t languages_arr = js_mkarr(js);
  js_arr_push(js, languages_arr, js_mkstr(js, "en-US", 5));
  js_set(js, navigator_obj, "languages", languages_arr);
  
  const char *platform = get_platform_string();
  js_set(js, navigator_obj, "platform", js_mkstr(js, platform, strlen(platform)));
  
  char user_agent[64];
  snprintf(user_agent, sizeof(user_agent), "Ant/%s", ANT_VERSION);
  js_set(js, navigator_obj, "userAgent", js_mkstr(js, user_agent, strlen(user_agent)));
  
  jsval_t locks_obj = js_mkobj(js);
  js_set(js, locks_obj, "request", js_mkfun(locks_request));
  js_set(js, locks_obj, "query", js_mkfun(locks_query));
  js_set(js, locks_obj, get_toStringTag_sym_key(), js_mkstr(js, "LockManager", 11));
  js_set(js, navigator_obj, "locks", locks_obj);
  
  js_set(js, navigator_obj, get_toStringTag_sym_key(), js_mkstr(js, "Navigator", 9));
  js_set(js, js_glob(js), "navigator", navigator_obj);
}
