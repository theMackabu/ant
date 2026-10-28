#ifdef STREAM_OP
STREAM_OP(once,     fn, this_val)
STREAM_OP(destroy,  stream)
STREAM_OP(call,     stream, callback)
STREAM_OP(pipeline, callback)
STREAM_OP(dispose,  promise, error)
STREAM_OP(pipe,     source, dest, on_data, on_drain, on_end, on_close, on_error)
STREAM_OP(finished, stream, callback, on_finish, on_error)
STREAM_OP(from,     readable, source, iterator, reader, next)
STREAM_OP(web,      source, stream, controller, on_data, on_end, on_error)
STREAM_OP(iter,     stream, queue, pending, error, on_data, on_end, on_error)
#undef STREAM_OP
#endif
