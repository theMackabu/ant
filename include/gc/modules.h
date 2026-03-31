#ifndef ANT_GC_MODULES_H
#define ANT_GC_MODULES_H

#include "types.h"

typedef void (*gc_mark_fn)(ant_t *js, ant_value_t v);

void gc_mark_timers(ant_t *js, gc_mark_fn mark);
void gc_mark_ffi(ant_t *js, gc_mark_fn mark);
void gc_mark_fetch(ant_t *js, gc_mark_fn mark);
void gc_mark_fs(ant_t *js, gc_mark_fn mark);
void gc_mark_child_process(ant_t *js, gc_mark_fn mark);
void gc_mark_readline(ant_t *js, gc_mark_fn mark);
void gc_mark_process(ant_t *js, gc_mark_fn mark);
void gc_mark_navigator(ant_t *js, gc_mark_fn mark);
void gc_mark_net(ant_t *js, gc_mark_fn mark);
void gc_mark_server(ant_t *js, gc_mark_fn mark);
void gc_mark_events(ant_t *js, gc_mark_fn mark);
void gc_mark_lmdb(ant_t *js, gc_mark_fn mark);
void gc_mark_symbols(ant_t *js, gc_mark_fn mark);
void gc_mark_esm(ant_t *js, gc_mark_fn mark);
void gc_mark_worker_threads(ant_t *js, gc_mark_fn mark);
void gc_mark_abort(ant_t *js, gc_mark_fn mark);
void gc_mark_domexception(ant_t *js, gc_mark_fn mark);
void gc_mark_queuing_strategies(ant_t *js, gc_mark_fn mark);
void gc_mark_readable_streams(ant_t *js, gc_mark_fn mark);
void gc_mark_writable_streams(ant_t *js, gc_mark_fn mark);
void gc_mark_transform_streams(ant_t *js, gc_mark_fn mark);
void gc_mark_codec_streams(ant_t *js, gc_mark_fn mark);
void gc_mark_compression_streams(ant_t *js, gc_mark_fn mark);
void gc_mark_zlib(ant_t *js, gc_mark_fn mark);
void gc_mark_wasm(ant_t *js, gc_mark_fn mark);
void gc_mark_abort_signal_object(ant_t *js, ant_value_t signal, gc_mark_fn mark);

#endif
