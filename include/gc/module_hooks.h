#ifdef GC_MODULE_MARK
GC_MODULE_MARK(timers)
GC_MODULE_MARK(cron)
GC_MODULE_MARK(atomics)
GC_MODULE_MARK(fetch)
GC_MODULE_MARK(fs)
GC_MODULE_MARK(crypto)
GC_MODULE_MARK(child_process)
GC_MODULE_MARK(dns)
GC_MODULE_MARK(readline)
GC_MODULE_MARK(process)
GC_MODULE_MARK(navigator)
GC_MODULE_MARK(message_ports)
GC_MODULE_MARK(net)
GC_MODULE_MARK(tls)
GC_MODULE_MARK(server)
GC_MODULE_MARK(websocket)
GC_MODULE_MARK(eventsource)
GC_MODULE_MARK(events)
GC_MODULE_MARK(esm)
GC_MODULE_MARK(worker_threads)
GC_MODULE_MARK(isolates)
#ifdef ANT_HAVE_SANDBOX
GC_MODULE_MARK(sandbox)
#endif
GC_MODULE_MARK(abort)
GC_MODULE_MARK(wasm)
GC_MODULE_MARK(napi)
GC_MODULE_MARK(rpc)
GC_MODULE_MARK(inspector)
#undef GC_MODULE_MARK
#endif

#ifdef GC_MODULE_WEAK
GC_MODULE_WEAK(napi)
GC_MODULE_WEAK(message_port)
GC_MODULE_WEAK(wasm)
#undef GC_MODULE_WEAK
#endif

#ifdef GC_CORE_MARK
GC_CORE_MARK(symbols)
GC_CORE_MARK(iterators)
GC_CORE_MARK(descriptors)
#undef GC_CORE_MARK
#endif

#ifdef GC_CORE_WEAK
GC_CORE_WEAK(descriptors)
#undef GC_CORE_WEAK
#endif

#ifdef GC_NATIVE_MARK
GC_NATIVE_MARK(abort_signal_object)
GC_NATIVE_MARK(eventemitter_object)
GC_NATIVE_MARK(stream_object)
#undef GC_NATIVE_MARK
#endif

#ifdef GC_NATIVE_FINALIZE
GC_NATIVE_FINALIZE(stream_object)
GC_NATIVE_FINALIZE(events_object)
#undef GC_NATIVE_FINALIZE
#endif
