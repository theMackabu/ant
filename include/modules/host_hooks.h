#ifdef HOST_HOOK
HOST_HOOK(bool, Ant_Host_Init,            (ant_t *js),                                           true)
HOST_HOOK(void, Ant_Host_Stop,            (ant_t *js),                                               )
HOST_HOOK(void, Ant_Host_Free,            (ant_t *js),                                               )
HOST_HOOK(void, gc_mark_host_modules,     (ant_t *js, gc_mark_fn mark),                              )
HOST_HOOK(void, gc_mark_host_natives,     (ant_t *js, ant_value_t value, gc_mark_fn mark),           )
HOST_HOOK(void, gc_finalize_host_natives, (ant_t *js, ant_value_t value),                            )
HOST_HOOK(void, gc_clear_host_weak_refs,  (ant_t *js, gc_weak_key_alive_fn alive),                   )
#undef HOST_HOOK
#endif
