#include "loop.h"
#include "wasm/wasm.h"

#include "modules/host.h"
#include "modules/abort.h"
#include "modules/atomics.h"
#include "modules/buffer.h"
#include "modules/child_process.h"
#include "modules/cron.h"
#include "modules/dns.h"
#include "modules/events.h"
#include "modules/eventsource.h"
#include "modules/fetch.h"
#include "modules/fs.h"
#include "modules/http.h"
#include "modules/isolate.h"
#include "modules/message_port.h"
#include "modules/napi.h"
#include "modules/navigator.h"
#include "modules/net.h"
#include "modules/performance.h"
#include "modules/process.h"
#include "modules/readline.h"
#include "modules/server.h"
#include "modules/stream.h"
#include "modules/timer.h"
#include "modules/tls.h"
#include "modules/websocket.h"
#include "modules/worker_threads.h"

bool Ant_Host_Init(ant_t *js) {
  return Ant_Loop_Init(js);
}

void Ant_Host_Stop(ant_t *js) {
  cleanup_isolate_module(js);
  cleanup_worker_threads_module(js);
  cleanup_timer_module(js);

  cleanup_cron_module(js);
  cleanup_abort_module(js);
  cleanup_eventsource_module(js);
  cleanup_websocket_module(js);
  cleanup_dns_module(js);
  cleanup_net_module(js);
  cleanup_tls_module(js);
  cleanup_server_module(js);
  cleanup_fetch_module(js);

  ant_http_pool_free(js->http_pool);
  js->http_pool = NULL;

  cleanup_child_process_module(js);
  cleanup_fs_module(js);
  cleanup_readline_module(js);
  cleanup_napi_module(js);
  cleanup_message_port_module(js);
  cleanup_navigator_module(js);
  cleanup_performance_module(js);
  cleanup_process_module(js);

  Ant_Loop_Drain(js);
  cleanup_timer_module(js);
}

void Ant_Host_Free(ant_t *js) {
  cleanup_buffer_module(js);
  cleanup_atomics_module(js);
  cleanup_events_module(js);
  free_process_module(js);
  free_napi_module(js);
  Ant_Loop_Destroy(js);
  wasm_isolate_destroy(js);
}

void gc_mark_host_modules(ant_t *js, gc_mark_fn mark) {
  #define GC_MODULE_MARK(name) gc_mark_##name(js, mark);
  #include "gc/module_hooks.h"
}

void gc_mark_host_natives(ant_t *js, ant_value_t value, gc_mark_fn mark) {
  #define GC_NATIVE_MARK(name) gc_mark_##name(js, value, mark);
  #include "gc/module_hooks.h"
}

void gc_finalize_host_natives(ant_t *js, ant_value_t value) {
  #define GC_NATIVE_FINALIZE(name) gc_finalize_##name(js, value);
  #include "gc/module_hooks.h"
}

bool gc_host_native_holds_resource(ant_t *js, ant_value_t value) {
  return stream_object_holds_attached_state(value);
}

void gc_clear_host_weak_refs(ant_t *js, gc_weak_key_alive_fn alive) {
  #define GC_MODULE_WEAK(name) gc_clear_##name##_weak_refs(js, alive);
  #include "gc/module_hooks.h"
}
