#include <compat.h> // IWYU pragma: keep
#include <stdio.h>
#include <crprintf.h>

#include "ant.h"
#include "bootstrap.h"
#include "realm.h"
#include "snapshot.h"
#include "primordials.h"
#include "messages.h"
#include "esm/library.h"

#include "modules/builtin.h"
#include "modules/buffer.h"
#include "modules/atomics.h"
#include "modules/os.h"
#include "modules/io.h"
#include "modules/fs.h"
#include "modules/crypto.h"
#include "modules/timer.h"
#include "modules/cron.h"
#include "modules/json.h"
#include "modules/fetch.h"
#include "modules/request.h"
#include "modules/response.h"
#include "modules/shell.h"
#include "modules/syntax.h"
#include "modules/process.h"
#include "modules/tty.h"
#include "modules/ffi.h"
#include "modules/events.h"
#include "modules/lmdb.h"
#include "modules/performance.h"
#include "modules/uri.h"
#include "modules/url.h"
#include "modules/reflect.h"
#include "modules/symbol.h"
#include "modules/date.h"
#include "modules/temporal.h"
#include "modules/math.h"
#include "modules/bigint.h"
#include "modules/regex.h"
#include "modules/textcodec.h"
#include "modules/sessionstorage.h"
#include "modules/localstorage.h"
#include "modules/navigator.h"
#include "modules/child_process.h"
#include "modules/readline.h"
#include "modules/observable.h"
#include "modules/collections.h"
#include "modules/iterator.h"
#include "modules/generator.h"
#include "modules/module.h"
#include "modules/util.h"
#include "modules/async_hooks.h"
#include "modules/net.h"
#include "modules/tls.h"
#include "modules/http_metadata.h"
#include "modules/http_parser.h"
#include "modules/http_writer.h"
#include "modules/websocket.h"
#include "modules/eventsource.h"
#include "modules/dns.h"
#include "modules/assert.h"
#include "modules/domexception.h"
#include "modules/abort.h"
#include "modules/globals.h"
#include "modules/intl.h"
#include "wasm/wasm.h"
#include "modules/string_decoder.h"
#include "modules/stream.h"
#include "modules/structured-clone.h"
#include "modules/v8.h"
#include "modules/vm.h"
#include "modules/worker_threads.h"
#include "modules/isolate.h"
#include "modules/message_port.h"
#include "modules/headers.h"
#include "modules/blob.h"
#include "modules/formdata.h"
#include "modules/zlib.h"
#include "modules/rpc.h"
#include "streams/queuing.h"
#include "streams/readable.h"
#include "streams/writable.h"
#include "streams/transform.h"
#include "streams/codec.h"
#include "streams/compression.h"

void ant_bootstrap_realm_intrinsics(ant_t *js) {
  init_symbol_ctor(js);
  init_intrinsic_symbols(js);
  init_iterator_module(js);
  init_generator_module(js);
  init_intl_module(js);
  init_wasm_module(js);
  init_buffer_module(js);
  init_math_module(js);
  init_bigint_module(js);
  init_date_module(js);
  #ifdef ANT_HAVE_TEMPORAL
  init_temporal_module(js);
  #endif
  init_regex_module(js);
  init_collections_module(js);
  init_atomics_module(js);
  init_json_module(js);
  init_reflect_module(js);
  init_uri_module(js);
}

bool ant_bootstrap_bare(ant_t *js) {
  js->primordials = js_mkundef();
  for (size_t i = 0; i < ANT_PRIMORDIAL_COUNT; i++) js->primordial_values[i] = js_mkundef();
  js->Ant = Ant_Object_Create(js);

  init_well_known_symbols(js);
  js_init_function_has_instance(js);
  ant_bootstrap_realm_intrinsics(js);
  if (!js_realm_finish_intrinsics(js)) return false;

  init_console_module(js);
  return !Ant_Exception_Pending(js);
}

void ant_bootstrap_modules(ant_t *js) {
  init_symbol_module(js);
  js_init_function_has_instance(js);
  init_intrinsic_symbols(js);
  init_iterator_module(js);
  init_generator_module(js);
  init_timer_module(js);
  init_domexception_module(js);
  init_globals_module(js);
  init_intl_module(js);
  init_wasm_module(js);
  init_builtin_module(js);
  init_buffer_module(js);
  init_structured_clone_module(js);
  init_headers_module(js);
  init_blob_module(js);
  init_formdata_module(js);
  init_math_module(js);
  init_bigint_module(js);
  init_date_module(js);
  init_cron_module(js);
  #ifdef ANT_HAVE_TEMPORAL
  init_temporal_module(js);
  #endif
  init_regex_module(js);
  init_collections_module(js);
  init_queuing_strategies_module(js);
  init_readable_stream_module(js);
  init_writable_stream_module(js);
  init_transform_stream_module(js);
  init_codec_stream_module(js);
  init_compression_stream_module(js);
  init_fs_module(js);
  init_atomics_module(js);
  init_crypto_module(js);
  init_request_module(js);
  init_response_module(js);
  init_fetch_module(js);
  init_console_module(js);
  init_json_module(js);
  init_process_module(js);
  init_tty_module(js);
  init_events_module(js);
  init_abort_module(js);
  init_websocket_module(js);
  init_message_port_module(js);
  init_web_worker_module(js);
  init_performance_module(js);
  init_uri_module(js);
  init_url_module(js);
  init_reflect_module(js);
  init_textcodec_module(js);
  init_eventsource_module(js);
  init_sessionstorage_module(js);
  init_localstorage_module(js);
  init_navigator_module(js);
  init_observable_module(js);

  ant_register_library(ffi_library, "ant:ffi");
  ant_register_library(isolate_library, "ant:isolate");
  ant_register_library(lmdb_library, "ant:lmdb");
  ant_register_library(rpc_library, "ant:rpc");
  ant_register_library(syntax_library, "ant:syntax");

  ant_register_library(shell_ops_library, "ant:internal/shell_ops");
  ant_register_library(primordial_library, "ant:internal/primordials");
  ant_register_library(realm_internal_library, "ant:internal/realm");
  ant_register_library(internal_http_parser_library, "ant:internal/http_parser");
  ant_register_library(internal_http_writer_library, "ant:internal/http_writer");
  ant_register_library(internal_http_metadata_library, "ant:internal/http_metadata");
  ant_register_library(internal_url_library, "ant:internal/url");

  ant_standard_library("util", util_library);
  ant_standard_library("util/types", util_types_library);
  ant_standard_library("console", console_library);
  ant_standard_library("net", net_library);
  ant_standard_library("tls", tls_library);
  ant_standard_library("dns", dns_library);
  ant_standard_library("assert", assert_library);
  ant_standard_library("module", module_library);
  ant_standard_library("buffer", buffer_library);
  ant_standard_library("fs", fs_library);
  ant_standard_library("constants", fs_constants_library);
  ant_standard_library("os", os_library);
  ant_standard_library("perf_hooks", perf_hooks_library);
  ant_standard_library("process", process_library);
  ant_standard_library("crypto", crypto_library);
  ant_standard_library("events", events_library);
  ant_standard_library("tty", tty_library);
  ant_standard_library("readline", readline_library);
  ant_standard_library("child_process", child_process_library);
  ant_standard_library("worker_threads", worker_threads_library);
  ant_standard_library("async_hooks", async_hooks_library);
  ant_standard_library("v8", v8_library);
  ant_standard_library("vm", vm_library);
  ant_standard_library("zlib", zlib_library);
  ant_standard_library("string_decoder", string_decoder_library);
  ant_standard_library("stream", stream_library);
  ant_standard_library("timers", timers_library);

  ant_standard_library("assert/strict", assert_strict_library);
  ant_standard_library("dns/promises", dns_promises_library);
  ant_standard_library("fs/promises", fs_promises_library);
  ant_standard_library("timers/promises", timers_promises_library);
  ant_standard_library("readline/promises", readline_promises_library);
  ant_standard_library("stream/promises", stream_promises_library);
  ant_standard_library("stream/web", stream_web_library);

  ant_value_t snapshot_result = ant_load_snapshot(js);
  if (vtype(snapshot_result) == kTypeError) crfprintf(stderr, msg.snapshot_warn, js_str(js, snapshot_result));
  js_set_descriptor(js, js_as_obj(js->Ant), "cron", 4, JS_DESC_W | JS_DESC_E);
}
