#include "gc.h"
#include "sugar.h"
#include "reactor.h"
#include "internal.h" // IWYU pragma: keep

#include "modules/fs.h"
#include "modules/timer.h"
#include "modules/fetch.h"
#include "modules/child_process.h"
#include "modules/readline.h"
#include "modules/process.h"

static reactor_poll_hook_t g_poll_hook = NULL;
static void *g_poll_hook_data = NULL;

static inline work_flags_t get_pending_work(void) {
  work_flags_t flags = 0;
  if (has_pending_microtasks())         flags |= WORK_MICROTASKS;
  if (has_pending_timers())             flags |= WORK_TIMERS;
  if (has_pending_immediates())         flags |= WORK_IMMEDIATES;
  if (has_pending_coroutines())         flags |= WORK_COROUTINES;
  if (has_ready_coroutines())           flags |= WORK_COROUTINES_READY;
  if (has_pending_fetches())            flags |= WORK_FETCHES;
  if (has_pending_fs_ops())             flags |= WORK_FS_OPS;
  if (has_pending_child_processes())    flags |= WORK_CHILD_PROCS;
  if (has_active_readline_interfaces()) flags |= WORK_READLINE;
  if (has_active_stdin())               flags |= WORK_STDIN;
  return flags;
}

static inline bool event_loop_alive(void) {
  work_flags_t w = get_pending_work();
  if (w & (WORK_PENDING & ~WORK_COROUTINES)) return true;
  if ((w & WORK_COROUTINES) && (w & WORK_COROUTINES_READY)) return true;
  return UV_CHECK_ALIVE;
}

void js_reactor_set_poll_hook(reactor_poll_hook_t hook, void *data) {
  g_poll_hook = hook;
  g_poll_hook_data = data;
}

void js_poll_events(ant_t *js) {
  coros_this_tick = 0;
  gc_maybe(js);

  process_immediates(js);
  process_microtasks(js);
  
  for (;;) {
    coroutine_t *temp = NULL;
    for (coroutine_t *c = pending_coroutines.head; c; c = c->next) 
      if (c->is_ready && c->mco && mco_status(c->mco) == MCO_SUSPENDED) { temp = c; break; }
    
    if (!temp) break;
    temp->is_ready = false;
    coroutine_retain(temp);
    
    mco_result res;
    MCO_RESUME_SAVE(js, temp->mco, res);
    
    if (res != MCO_SUCCESS || mco_status(temp->mco) == MCO_DEAD) 
      remove_coroutine(temp);
    coroutine_release(temp);
  }
  
  if (g_poll_hook) g_poll_hook(g_poll_hook_data);
}

void js_run_event_loop(ant_t *js) {
drain:
  while (event_loop_alive()) {
    js_poll_events(js);
    reap_retired_coroutines();
    work_flags_t work = get_pending_work();
    
    if (work & WORK_BLOCKING) uv_run(uv_default_loop(), UV_RUN_NOWAIT);
    else if ((work & WORK_ASYNC) || UV_CHECK_ALIVE) {
      uv_run(uv_default_loop(), UV_RUN_ONCE);
    } else break;
  } 
  
  js_poll_events(js);
  reap_retired_coroutines();
  ant_value_t code = js_mknum(0);
  emit_process_event("beforeExit", &code, 1);
  
  if (event_loop_alive()) goto drain;
}

void js_reactor_pump_repl_nowait(ant_t *js) {
  js_poll_events(js);
  reap_retired_coroutines();
  uv_run(uv_default_loop(), UV_RUN_NOWAIT);
  js_poll_events(js);
  reap_retired_coroutines();
}
