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
#include "modules/lmdb.h"
#include "modules/message_port.h"
#include "modules/napi.h"
#include "modules/navigator.h"
#include "modules/net.h"
#include "modules/performance.h"
#include "modules/process.h"
#include "modules/readline.h"
#include "modules/rpc.h"
#include "modules/server.h"
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

void Ant_Host_Close(ant_t *js) {
  cleanup_rpc_module(js);
  cleanup_lmdb_module(js);
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
  gc_mark_timers(js, mark);
  gc_mark_cron(js, mark);
  gc_mark_atomics(js, mark);
  gc_mark_fetch(js, mark);
  gc_mark_fs(js, mark);
  gc_mark_crypto(js, mark);
  gc_mark_child_process(js, mark);
  gc_mark_dns(js, mark);
  gc_mark_readline(js, mark);
  gc_mark_process(js, mark);
  gc_mark_navigator(js, mark);
  gc_mark_message_ports(js, mark);
  gc_mark_net(js, mark);
  gc_mark_tls(js, mark);
  gc_mark_server(js, mark);
  gc_mark_websocket(js, mark);
  gc_mark_eventsource(js, mark);
  gc_mark_events(js, mark);
  gc_mark_esm(js, mark);
  gc_mark_worker_threads(js, mark);
  gc_mark_isolates(js, mark);
  gc_mark_sandbox(js, mark);
  gc_mark_abort(js, mark);
  gc_mark_wasm(js, mark);
  gc_mark_napi(js, mark);
  gc_mark_rpc(js, mark);
}

void gc_mark_host_natives(ant_t *js, ant_value_t value, gc_mark_fn mark) {
  gc_mark_abort_signal_object(js, value, mark);
  gc_mark_eventemitter_object(js, value, mark);
  gc_mark_stream_object(js, value, mark);
}

void gc_finalize_host_natives(ant_t *js, ant_value_t value) {
  gc_finalize_stream_object(js, value);
  gc_finalize_events_object(js, value);
}

void gc_clear_host_weak_refs(ant_t *js, gc_weak_key_alive_fn alive) {
  gc_clear_napi_weak_refs(js, alive);
}
