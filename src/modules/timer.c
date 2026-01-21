#include <compat.h> // IWYU pragma: keep

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

#include "arena.h"
#include "runtime.h"
#include "modules/timer.h"

typedef struct timer_entry {
  jsval_t callback;
  uint64_t target_time_ms;
  uint64_t interval_ms;
  int timer_id;
  int active;
  int is_interval;
  struct timer_entry *next;
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
} timer_state = {NULL, NULL, NULL, NULL, NULL, NULL, 1, 1};

static uint64_t get_current_time_ms(void) {
#ifdef _WIN32
  LARGE_INTEGER freq, count;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&count);
  return (uint64_t)(count.QuadPart * 1000 / freq.QuadPart);
#else
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
#endif
}

// setTimeout(callback, delay)
static jsval_t js_set_timeout(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) {
    return js_mkerr(js, "setTimeout requires 2 arguments (callback, delay)");
  }
  
  jsval_t callback = args[0];
  double delay_ms = js_getnum(args[1]);
  
  if (delay_ms < 0) {
    return js_mkerr(js, "setTimeout delay must be non-negative");
  }
  
  timer_entry_t *entry = ANT_GC_MALLOC(sizeof(timer_entry_t));
  if (entry == NULL) {
    return js_mkerr(js, "failed to allocate timer");
  }
  
  entry->callback = callback;
  entry->target_time_ms = get_current_time_ms() + (uint64_t)delay_ms;
  entry->interval_ms = 0;
  entry->timer_id = timer_state.next_timer_id++;
  entry->active = 1;
  entry->is_interval = 0;
  entry->next = timer_state.timers;
  timer_state.timers = entry;
  
  return js_mknum((double)entry->timer_id);
}

// setInterval(callback, delay)
static jsval_t js_set_interval(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) {
    return js_mkerr(js, "setInterval requires 2 arguments (callback, delay)");
  }
  
  jsval_t callback = args[0];
  double delay_ms = js_getnum(args[1]);
  
  if (delay_ms < 0) {
    return js_mkerr(js, "setInterval delay must be non-negative");
  }
  
  timer_entry_t *entry = ANT_GC_MALLOC(sizeof(timer_entry_t));
  if (entry == NULL) {
    return js_mkerr(js, "failed to allocate timer");
  }
  
  entry->callback = callback;
  entry->target_time_ms = get_current_time_ms() + (uint64_t)delay_ms;
  entry->interval_ms = (uint64_t)delay_ms;
  entry->timer_id = timer_state.next_timer_id++;
  entry->active = 1;
  entry->is_interval = 1;
  entry->next = timer_state.timers;
  timer_state.timers = entry;
  
  return js_mknum((double)entry->timer_id);
}

// clearTimeout(timerId)
static jsval_t js_clear_timeout(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkundef();
  }
  
  int timer_id = (int)js_getnum(args[0]);
  
  for (timer_entry_t *entry = timer_state.timers; entry != NULL; entry = entry->next) {
    if (entry->timer_id == timer_id) {
      entry->active = 0;
      break;
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
  
  immediate_entry_t *entry = ANT_GC_MALLOC(sizeof(immediate_entry_t));
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
  microtask_entry_t *entry = ANT_GC_MALLOC(sizeof(microtask_entry_t));
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
  microtask_entry_t *entry = ANT_GC_MALLOC(sizeof(microtask_entry_t));
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
  js_set_gc_suppress(js, true);
  
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
    
    ANT_GC_FREE(entry);
  }
  
  js_set_gc_suppress(js, false);
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
    
    ANT_GC_FREE(entry);
  }
}

int has_pending_immediates(void) {
  for (
    immediate_entry_t *entry = timer_state.immediates;
    entry != NULL; entry = entry->next
  ) if (entry->active) return 1;
  return 0;
}

static void remove_timer(timer_entry_t *target) {
  timer_entry_t **ptr = &timer_state.timers;
  
scan:
  if (!*ptr) return;
  if (*ptr == target) { 
    *ptr = target->next; 
    ANT_GC_FREE(target); return; 
  }
  ptr = &(*ptr)->next;
  goto scan;
}

void process_timers(struct js *js) {
  uint64_t current_time = get_current_time_ms();
  timer_entry_t **ptr = &timer_state.timers;
  timer_entry_t *entry;

scan:
  if (!*ptr) return;
  entry = *ptr;
  
  if (!entry->active) {
    *ptr = entry->next;
    ANT_GC_FREE(entry);
    goto scan;
  }
  
  if (current_time < entry->target_time_ms) {
    ptr = &entry->next;
    goto scan;
  }
  
  jsval_t args[0];
  js_call(js, entry->callback, args, 0);
  process_microtasks(js);
  
  if (entry->is_interval && entry->active) {
    entry->target_time_ms = get_current_time_ms() + entry->interval_ms;
  } else remove_timer(entry);
  
  current_time = get_current_time_ms();
  ptr = &timer_state.timers;
  goto scan;
}

int has_pending_timers(void) {
  for (timer_entry_t *entry = timer_state.timers; entry != NULL; entry = entry->next) {
    if (entry->active) return 1;
  }
  return 0;
}

int has_pending_microtasks(void) {
  return timer_state.microtasks != NULL ? 1 : 0;
}

int64_t get_next_timer_timeout(void) {
  uint64_t current_time = get_current_time_ms();
  int64_t min_timeout = -1;
  
  for (timer_entry_t *entry = timer_state.timers; entry != NULL; entry = entry->next) {
    if (!entry->active) continue;
    
    int64_t timeout = (int64_t)entry->target_time_ms - (int64_t)current_time;
    if (timeout <= 0) return 0;
    if (min_timeout == -1 || timeout < min_timeout) min_timeout = timeout;
  }
  
  return min_timeout;
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

void timer_gc_update_roots(GC_FWD_ARGS) {
  for (timer_entry_t *t = timer_state.timers; t; t = t->next) {
    t->callback = fwd_val(ctx, t->callback);
  }
  for (microtask_entry_t *m = timer_state.microtasks; m; m = m->next) {
    if (m->promise_id == 0) m->callback = fwd_val(ctx, m->callback);
  }
  for (immediate_entry_t *i = timer_state.immediates; i; i = i->next) {
    i->callback = fwd_val(ctx, i->callback);
  }
}
