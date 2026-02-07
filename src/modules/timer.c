#include <compat.h> // IWYU pragma: keep

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uv.h>

#include "arena.h"
#include "errors.h"
#include "internal.h"
#include "runtime.h"
#include "modules/timer.h"

typedef struct timer_entry {
  uv_timer_t handle;
  jsval_t callback;
  jsval_t *args;
  int nargs;
  int timer_id;
  int active;
  int is_interval;
  struct timer_entry *next;
  struct timer_entry *prev;
} timer_entry_t;

typedef struct microtask_entry {
  jsval_t callback;
  uint32_t promise_id;
  struct microtask_entry *next;
} microtask_entry_t;

typedef struct immediate_entry {
  jsval_t callback;
  int immediate_id;
  int active;
  struct immediate_entry *next;
} immediate_entry_t;

static struct {
  struct js *js;
  timer_entry_t *timers;
  microtask_entry_t *microtasks;
  microtask_entry_t *microtasks_tail;
  immediate_entry_t *immediates;
  immediate_entry_t *immediates_tail;
  int next_timer_id;
  int next_immediate_id;
  int active_timer_count;
} timer_state = {NULL, NULL, NULL, NULL, NULL, NULL, 1, 1, 0};

static void add_timer_entry(timer_entry_t *entry) {
  entry->next = timer_state.timers;
  entry->prev = NULL;
  if (timer_state.timers) {
    timer_state.timers->prev = entry;
  }
  timer_state.timers = entry;
}

static void remove_timer_entry(timer_entry_t *entry) {
  if (entry->prev) entry->prev->next = entry->next;
  else timer_state.timers = entry->next;
  if (entry->next) entry->next->prev = entry->prev;
}

static int timer_copy_args(timer_entry_t *entry, jsval_t *args, int nargs) {
  entry->nargs = nargs > 2 ? nargs - 2 : 0;
  if (entry->nargs > 0) {
    entry->args = ant_calloc(sizeof(jsval_t) * entry->nargs);
    if (!entry->args) return -1;
    memcpy(entry->args, args + 2, sizeof(jsval_t) * entry->nargs);
  } else entry->args = NULL;
  return 0;
}

static void timer_close_cb(uv_handle_t *h) {
  timer_entry_t *entry = (timer_entry_t *)h->data;
  if (!entry) return;
  
  remove_timer_entry(entry);
  entry->callback = 0;
  entry->next = NULL;
  entry->prev = NULL;
  h->data = NULL;
  
  if (entry->args) free(entry->args);
  free(entry);
}

static void timer_callback(uv_timer_t *handle) {
  timer_entry_t *entry = (timer_entry_t *)handle->data;
  if (!entry || !entry->active) return;
  
  struct js *js = timer_state.js;
  js_call(js, entry->callback, entry->args, entry->nargs);
  process_microtasks(js);
  
  if (!entry->is_interval) {
    entry->active = 0;
    timer_state.active_timer_count--;
    uv_close((uv_handle_t *)&entry->handle, timer_close_cb);
  }
}

// setTimeout(callback, delay, ...args)
static jsval_t js_set_timeout(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) {
    return js_mkerr(js, "setTimeout requires 2 arguments (callback, delay)");
  }
  
  jsval_t callback = args[0];
  double delay_ms = js_getnum(args[1]);
  uint64_t ms = delay_ms < 1 ? 0 : (uint64_t)delay_ms;
  
  timer_entry_t *entry = ant_calloc(sizeof(timer_entry_t));
  if (entry == NULL) return js_mkerr(js, "failed to allocate timer");
  
  if (timer_copy_args(entry, args, nargs) < 0) {
    free(entry);
    return js_mkerr(js, "failed to allocate timer args");
  }
  
  uv_timer_init(uv_default_loop(), &entry->handle);
  entry->handle.data = entry;
  entry->callback = callback;
  entry->timer_id = timer_state.next_timer_id++;
  entry->active = 1;
  entry->is_interval = 0;
  
  add_timer_entry(entry);
  timer_state.active_timer_count++;
  uv_timer_start(&entry->handle, timer_callback, ms, 0);
  
  return js_mknum((double)entry->timer_id);
}

// setInterval(callback, delay, ...args)
static jsval_t js_set_interval(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) {
    return js_mkerr(js, "setInterval requires 2 arguments (callback, delay)");
  }
  
  jsval_t callback = args[0];
  double delay_ms = js_getnum(args[1]);
  uint64_t ms = delay_ms < 1 ? 1 : (uint64_t)delay_ms;
  
  timer_entry_t *entry = ant_calloc(sizeof(timer_entry_t));
  if (entry == NULL) return js_mkerr(js, "failed to allocate timer");
  
  if (timer_copy_args(entry, args, nargs) < 0) {
    free(entry);
    return js_mkerr(js, "failed to allocate timer args");
  }
  
  uv_timer_init(uv_default_loop(), &entry->handle);
  entry->handle.data = entry;
  entry->callback = callback;
  entry->timer_id = timer_state.next_timer_id++;
  entry->active = 1;
  entry->is_interval = 1;
  
  add_timer_entry(entry);
  timer_state.active_timer_count++;
  uv_timer_start(&entry->handle, timer_callback, ms, ms);
  
  return js_mknum((double)entry->timer_id);
}

// clearTimeout(timerId)
static jsval_t js_clear_timeout(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  int timer_id = (int)js_getnum(args[0]);
  
  for (timer_entry_t *entry = timer_state.timers; entry != NULL; entry = entry->next) {
    if (entry->timer_id == timer_id && entry->active) {
      entry->active = 0; timer_state.active_timer_count--;
      uv_close((uv_handle_t *)&entry->handle, timer_close_cb); break;
    }
  }
  
  return js_mkundef();
}

// setImmediate(callback)
static jsval_t js_set_immediate(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "setImmediate requires 1 argument (callback)");
  }
  
  jsval_t callback = args[0];
  
  immediate_entry_t *entry = ant_calloc(sizeof(immediate_entry_t));
  if (entry == NULL) {
    return js_mkerr(js, "failed to allocate immediate");
  }
  
  entry->callback = callback;
  entry->immediate_id = timer_state.next_immediate_id++;
  entry->active = 1;
  entry->next = NULL;
  
  if (timer_state.immediates_tail == NULL) {
    timer_state.immediates = entry;
    timer_state.immediates_tail = entry;
  } else {
    timer_state.immediates_tail->next = entry;
    timer_state.immediates_tail = entry;
  }
  
  return js_mknum((double)entry->immediate_id);
}

// clearImmediate(immediateId)
static jsval_t js_clear_immediate(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();  
  int immediate_id = (int)js_getnum(args[0]);
  
  for (immediate_entry_t *entry = timer_state.immediates; entry != NULL; entry = entry->next) {
    if (entry->immediate_id == immediate_id) { entry->active = 0; break; }
  }
  
  return js_mkundef();
}

// queueMicrotask(callback)
static jsval_t js_queue_microtask(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "queueMicrotask requires 1 argument (callback)");
  }
  
  queue_microtask(js, args[0]);
  return js_mkundef();
}

void queue_microtask(struct js *js, jsval_t callback) {
  microtask_entry_t *entry = ant_calloc(sizeof(microtask_entry_t));
  if (entry == NULL) return;
  
  entry->callback = callback;
  entry->promise_id = 0;
  entry->next = NULL;
  
  if (timer_state.microtasks_tail == NULL) {
    timer_state.microtasks = entry;
    timer_state.microtasks_tail = entry;
  } else {
    timer_state.microtasks_tail->next = entry;
    timer_state.microtasks_tail = entry;
  }
}

void queue_promise_trigger(uint32_t promise_id) {
  microtask_entry_t *entry = ant_calloc(sizeof(microtask_entry_t));
  if (entry == NULL) return;
  
  entry->callback = 0;
  entry->promise_id = promise_id;
  entry->next = NULL;
  
  if (timer_state.microtasks_tail == NULL) {
    timer_state.microtasks = entry;
    timer_state.microtasks_tail = entry;
  } else {
    timer_state.microtasks_tail->next = entry;
    timer_state.microtasks_tail = entry;
  }
}

void process_microtasks(struct js *js) {
  while (timer_state.microtasks != NULL) {
    microtask_entry_t *entry = timer_state.microtasks;
    timer_state.microtasks = entry->next;
    
    if (timer_state.microtasks == NULL) {
      timer_state.microtasks_tail = NULL;
    }
    
    if (entry->promise_id != 0) {
      js_process_promise_handlers(js, entry->promise_id);
    } else {
      jsval_t args[0];
      js_call(js, entry->callback, args, 0);
    }
    
    free(entry);
  }
  
  js_check_unhandled_rejections(js);
}

void process_immediates(struct js *js) {
  while (timer_state.immediates != NULL) {
    immediate_entry_t *entry = timer_state.immediates;
    timer_state.immediates = entry->next;
    
    if (timer_state.immediates == NULL) {
      timer_state.immediates_tail = NULL;
    }
    
    if (entry->active) {
      jsval_t args[0];
      js_call(js, entry->callback, args, 0);
      process_microtasks(js);
    }
    
    free(entry);
  }
}

int has_pending_immediates(void) {
  for (
    immediate_entry_t *entry = timer_state.immediates;
    entry != NULL; entry = entry->next
  ) if (entry->active) return 1;
  return 0;
}

int has_pending_timers(void) {
  return timer_state.active_timer_count > 0;
}

int has_pending_microtasks(void) {
  return timer_state.microtasks != NULL ? 1 : 0;
}

void init_timer_module() {  
  struct js *js = rt->js;

  timer_state.js = js;
  jsval_t global = js_glob(js);
  
  js_set(js, global, "setTimeout", js_mkfun(js_set_timeout));
  js_set(js, global, "clearTimeout", js_mkfun(js_clear_timeout));
  js_set(js, global, "setInterval", js_mkfun(js_set_interval));
  js_set(js, global, "clearInterval", js_mkfun(js_clear_timeout));
  js_set(js, global, "setImmediate", js_mkfun(js_set_immediate));
  js_set(js, global, "clearImmediate", js_mkfun(js_clear_immediate));
  js_set(js, global, "queueMicrotask", js_mkfun(js_queue_microtask));
}

void timer_gc_update_roots(GC_OP_VAL_ARGS) {
  for (timer_entry_t *t = timer_state.timers; t; t = t->next) {
    op_val(ctx, &t->callback);
    for (int i = 0; i < t->nargs; i++) op_val(ctx, &t->args[i]);
  }
  for (microtask_entry_t *m = timer_state.microtasks; m; m = m->next) {
    if (m->promise_id == 0) op_val(ctx, &m->callback);
  }
  for (immediate_entry_t *i = timer_state.immediates; i; i = i->next) {
    op_val(ctx, &i->callback);
  }
}
