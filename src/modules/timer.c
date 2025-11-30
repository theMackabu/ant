#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

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
  struct microtask_entry *next;
} microtask_entry_t;

static struct {
  struct js *js;
  timer_entry_t *timers;
  microtask_entry_t *microtasks;
  microtask_entry_t *microtasks_tail;
  int next_timer_id;
} timer_state = {NULL, NULL, NULL, NULL, 1};

static uint64_t get_current_time_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

// Ant.setTimeout(callback, delay)
static jsval_t js_set_timeout(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) {
    return js_mkerr(js, "setTimeout requires 2 arguments (callback, delay)");
  }
  
  jsval_t callback = args[0];
  double delay_ms = js_getnum(args[1]);
  
  if (delay_ms < 0) {
    return js_mkerr(js, "setTimeout delay must be non-negative");
  }
  
  timer_entry_t *entry = malloc(sizeof(timer_entry_t));
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

// Ant.setInterval(callback, delay)
static jsval_t js_set_interval(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) {
    return js_mkerr(js, "setInterval requires 2 arguments (callback, delay)");
  }
  
  jsval_t callback = args[0];
  double delay_ms = js_getnum(args[1]);
  
  if (delay_ms < 0) {
    return js_mkerr(js, "setInterval delay must be non-negative");
  }
  
  timer_entry_t *entry = malloc(sizeof(timer_entry_t));
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

// Ant.clearTimeout(timerId)
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

// Ant.queueMicrotask(callback)
static jsval_t js_queue_microtask(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "queueMicrotask requires 1 argument (callback)");
  }
  
  jsval_t callback = args[0];
  
  microtask_entry_t *entry = malloc(sizeof(microtask_entry_t));
  if (entry == NULL) {
    return js_mkerr(js, "failed to allocate microtask");
  }
  
  entry->callback = callback;
  entry->next = NULL;
  
  if (timer_state.microtasks_tail == NULL) {
    timer_state.microtasks = entry;
    timer_state.microtasks_tail = entry;
  } else {
    timer_state.microtasks_tail->next = entry;
    timer_state.microtasks_tail = entry;
  }
  
  return js_mkundef();
}

void process_microtasks(struct js *js) {
  while (timer_state.microtasks != NULL) {
    microtask_entry_t *entry = timer_state.microtasks;
    timer_state.microtasks = entry->next;
    
    if (timer_state.microtasks == NULL) {
      timer_state.microtasks_tail = NULL;
    }
    
    jsval_t args[0];
    js_call(js, entry->callback, args, 0);
    
    free(entry);
  }
}

void process_timers(struct js *js) {
  if (timer_state.timers == NULL) return;
  
  uint64_t current_time = get_current_time_ms();
  timer_entry_t **entry_ptr = &timer_state.timers;
  
  while (*entry_ptr != NULL) {
    timer_entry_t *entry = *entry_ptr;
    
    if (!entry->active) {
      *entry_ptr = entry->next;
      free(entry);
      continue;
    }
    
    if (entry->active && current_time >= entry->target_time_ms) {
      jsval_t args[0];
      js_call(js, entry->callback, args, 0);
      
      process_microtasks(js);
      
      if (entry->is_interval) {
        entry->target_time_ms = get_current_time_ms() + entry->interval_ms;
        entry_ptr = &entry->next;
      } else {
        *entry_ptr = entry->next;
        free(entry);
      }
      continue;
    }
    
    entry_ptr = &entry->next;
  }
}

int has_pending_timers(void) {
  for (timer_entry_t *entry = timer_state.timers; entry != NULL; entry = entry->next) {
    if (entry->active) return 1;
  }
  return 0;
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

void init_timer_module(struct js *js, jsval_t ant_obj) {
  timer_state.js = js;
  
  js_set(js, ant_obj, "setTimeout", js_mkfun(js_set_timeout));
  js_set(js, ant_obj, "clearTimeout", js_mkfun(js_clear_timeout));
  js_set(js, ant_obj, "setInterval", js_mkfun(js_set_interval));
  js_set(js, ant_obj, "clearInterval", js_mkfun(js_clear_timeout));
  js_set(js, ant_obj, "queueMicrotask", js_mkfun(js_queue_microtask));
}
