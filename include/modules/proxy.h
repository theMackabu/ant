#ifndef ANT_MODULES_PROXY_H
#define ANT_MODULES_PROXY_H

#include "keys.h"
#include "internal.h"

void js_proxy_init_constructor(ant_t *js, ant_value_t proxy_ctor_obj);
ant_proxy_state_t *get_proxy_data(ant_value_t obj);

ant_value_t proxy_key_enumerable(ant_t *js, ant_value_t proxy, ant_value_t key);
ant_value_t proxy_own_keys_raw(ant_t *js, ant_value_t obj);
ant_value_t proxy_own_keys_filtered(ant_t *js, ant_value_t obj, bool include_symbols, bool enumerable_only);
ant_value_t proxy_not_prevented_error(ant_t *js, ant_value_t proxy, const char *host_message, const char *message);
ant_value_t proxy_set_integrity_level(ant_t *js, ant_value_t obj, bool freeze);
ant_value_t proxy_test_integrity_level(ant_t *js, ant_value_t obj, bool frozen);
ant_value_t proxy_enum(ant_t *js, ant_value_t obj, enum obj_enum_mode mode);
ant_value_t proxy_host_pairs(ant_t *js, ant_proxy_state_t *data, bool include_symbols);
ant_value_t proxy_get_with_receiver(ant_t *js, ant_value_t proxy, const char *key, size_t key_len, ant_value_t receiver);
ant_value_t proxy_get(ant_t *js, ant_value_t proxy, const char *key, size_t key_len);
ant_value_t proxy_set_with_receiver(ant_t *js, ant_value_t proxy, const char *key, size_t key_len, ant_value_t value, ant_value_t receiver);
ant_value_t proxy_receiver_set_error(ant_t *js, ant_value_t proxy, ant_value_t receiver, const char *key, size_t key_len);
ant_value_t proxy_set(ant_t *js, ant_value_t proxy, const char *key, size_t key_len, ant_value_t value);
ant_value_t proxy_set_rejected(ant_t *js, const char *key, size_t key_len, ant_value_t value, bool *declined);
ant_value_t proxy_has(ant_t *js, ant_value_t proxy, const char *key, size_t key_len);
ant_value_t proxy_delete(ant_t *js, ant_value_t proxy, const char *key, size_t key_len);
ant_value_t proxy_get_val(ant_t *js, ant_value_t proxy, ant_value_t key_val);
ant_value_t proxy_get_prototype_of(ant_t *js, ant_value_t proxy);
ant_value_t proxy_set_prototype_of(ant_t *js, ant_value_t proxy, ant_value_t proto);
ant_value_t proxy_is_extensible(ant_t *js, ant_value_t proxy);
ant_value_t proxy_prevent_extensions(ant_t *js, ant_value_t proxy);
ant_value_t proxy_has_val(ant_t *js, ant_value_t proxy, ant_value_t key_val);
ant_value_t proxy_get_own_property_descriptor(ant_t *js, ant_value_t proxy, ant_value_t key_val);
ant_value_t proxy_has_own(ant_t *js, ant_value_t proxy, ant_value_t key_val);
ant_value_t proxy_define_property(ant_t *js, ant_value_t proxy, ant_value_t key_val, ant_value_t descriptor);
ant_value_t proxy_delete_sym(ant_t *js, ant_value_t proxy, ant_value_t sym);

ant_value_t proxy_set_sym(
  ant_t *js, ant_value_t proxy, ant_value_t sym, 
  ant_value_t value, ant_value_t receiver, ant_value_t *target_out
);

ant_value_t proxy_set_with_receiver_reason(
  ant_t *js, ant_value_t proxy, const char *key, size_t key_len,
  ant_value_t value, ant_value_t receiver, bool *trapless
);

__attribute__((noinline, cold)) 
ant_value_t proxy_set_rejected_sym(ant_t *js, ant_value_t sym, ant_value_t value, bool *declined);

__attribute__((noinline, cold)) 
ant_value_t proxy_trap_error(ant_t *js, const char *trap, const char *reason);

#endif
