#ifdef FS_STREAM
FS_STREAM(Read,  read,  readable, "_read",  fs_readstream__read,   1)
FS_STREAM(Write, write, writable, "_write", fs_writestream__write, 3)
#undef FS_STREAM
#endif
