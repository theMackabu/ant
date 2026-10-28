#ifdef STREAM_METHOD
STREAM_METHOD(READ,      "_read")
STREAM_METHOD(WRITE,     "_write")
STREAM_METHOD(WRITEV,    "_writev")
STREAM_METHOD(FINAL,     "_final")
STREAM_METHOD(TRANSFORM, "_transform")
STREAM_METHOD(FLUSH,     "_flush")
STREAM_METHOD(DESTROY,   "_destroy")
#undef STREAM_METHOD
#endif
