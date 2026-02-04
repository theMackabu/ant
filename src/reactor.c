#include <internal.h>
#include <uv.h>

#include "gc.h"
#include "sugar.h"
#include "reactor.h"

#include "modules/fs.h"
#include "modules/timer.h"
#include "modules/fetch.h"
#include "modules/child_process.h"
#include "modules/readline.h"
#include "modules/process.h"

void js_poll_events(ant_t *js) {
  coros_this_tick = 0;
  
  fetch_poll_events();
  fs_poll_events();
  child_process_poll_events();
  
  process_immediates(js);
  process_microtasks(js);
  
  for (coroutine_t *temp = pending_coroutines.head, *next; temp; temp = next) {
    next = temp->next;
    if (!temp->is_ready || !temp->mco || mco_status(temp->mco) != MCO_SUSPENDED) continue;
    remove_coroutine(temp);
    
    coro_saved_state_t saved = coro_enter(temp->js, temp);
    mco_result res = mco_resume(temp->mco);
    coro_leave(temp->js, temp, saved);
    
    if (res == MCO_SUCCESS && mco_status(temp->mco) != MCO_DEAD) {
      temp->is_ready = false;
      enqueue_coroutine(temp);
    } else free_coroutine(temp);
  }
  
  if (js->needs_gc) {
    js->needs_gc = false;
    js_gc_compact(js);
    js->gc_alloc_since = 0;
  }
}

static inline work_flags_t get_pending_work(void) {
  work_flags_t flags = 0;
  if (has_pending_microtasks())          flags |= WORK_MICROTASKS;
  if (has_pending_timers())              flags |= WORK_TIMERS;
  if (has_pending_immediates())          flags |= WORK_IMMEDIATES;
  if (has_pending_coroutines())       flags |= WORK_COROUTINES;
  if (has_ready_coroutines())            flags |= WORK_COROUTINES_READY;
  if (has_pending_fetches())             flags |= WORK_FETCHES;
  if (has_pending_fs_ops())              flags |= WORK_FS_OPS;
  if (has_pending_child_processes())     flags |= WORK_CHILD_PROCS;
  if (has_active_readline_interfaces())  flags |= WORK_READLINE;
  if (has_active_stdin())                flags |= WORK_STDIN;
  return flags;
}

static void maybe_gc(ant_t *js) {
  jsoff_t thresh = js->brk / 2;
  if (thresh < 4 * 1024 * 1024) thresh = 4 * 1024 * 1024;

  if (js->gc_alloc_since > thresh || js->needs_gc) {
    js->needs_gc = false;
    js_gc_compact(js);
    js->gc_alloc_since = 0;
  }
}

void js_run_event_loop(ant_t *js) {
  work_flags_t work;
  
  while ((work = get_pending_work()) & WORK_PENDING) {
    js_poll_events(js);
    work = get_pending_work();
  
    if (work & WORK_BLOCKING) uv_run(uv_default_loop(), UV_RUN_NOWAIT);
    else if (work & WORK_ASYNC) { maybe_gc(js); uv_run(uv_default_loop(), UV_RUN_ONCE); }
    else if (work & WORK_COROUTINES) break;
  }
  
  js_poll_events(js);
}