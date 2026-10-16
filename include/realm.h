#ifndef ANT_REALM_H
#define ANT_REALM_H

#include "isolate.h"
#include "list.h"

struct ant_realm {
  ant_realm_t *next;
  ant_realm_t **pprev;
  
  bool is_main;
  bool initializing;
  bool gc_dirty;
  
  ant_ptr_pair_map_t builtin_records;
  uint64_t gc_epoch;
  ant_ptr_pair_map_t cfunc_promoted;
  ant_ptr_pair_map_t cfunc_named;
  ant_ptr_pair_map_t vm_scripts;
  microtask_entry_t *microtasks;
  microtask_entry_t *microtasks_tail;
  microtask_entry_t *microtasks_processing;

  #define ANT_REALM_FIELD(name) __typeof__(((ant_t *)0)->name) name;
  #include "realm/fields.h"

  struct {
    #define ANT_REALM_SYM_FIELD(name) ant_value_t name;
    #include "realm/sym_fields.h"
    ant_value_t math_fns[ANT_MATH_INTRINSIC_COUNT];
  } sym;

  struct {
    #define ANT_BUILTIN(name)
    #define ANT_BUILTIN_ARR(name, n)
    #define ANT_REALM_BUILTIN(name) ant_value_t name;
    #define ANT_REALM_BUILTIN_ARR(name, n) ant_value_t name[n];
    #include "isolate_values.h"
  } builtins;

  struct {
    #define ANT_MUTABLE_ROOT(name)
    #define ANT_MUTABLE_ROOT_ARR(name, n)
    #define ANT_REALM_MUTABLE_ROOT(name) ant_value_t name;
    #define ANT_REALM_MUTABLE_ROOT_ARR(name, n) ant_value_t name[n];
    #include "isolate_values.h"
  } mutable_roots;
};

static inline ant_realm_t *ant_realm_main(ant_t *js) {
  return js->main_realm;
}
ant_realm_t *ant_realm_create(ant_t *js);
ant_realm_t *ant_realm_enter(ant_t *js, ant_realm_t *realm);

static inline ant_realm_t *ant_realm_builtin_key(ant_realm_t *realm) {
  return realm && !realm->is_main ? realm : NULL;
}

static inline bool ant_realm_multiple(const ant_t *js) {
  return js->realms && js->realms->next;
}

static inline ant_realm_t *ant_realm_switch(ant_t *js, ant_realm_t *realm) {
  ant_realm_t *prev = js->realm;
  if (__builtin_expect(realm != prev && realm != NULL, 0)) ant_realm_enter(js, realm);
  return prev;
}

static inline void ant_realm_restore(ant_t *js, ant_realm_t *prev) {
  if (__builtin_expect(js->realm != prev, 0)) ant_realm_enter(js, prev);
}

ant_realm_t *ant_realm_of_builtin(ant_t *js, ant_value_t builtin);
ant_value_t ant_realm_function_proto(ant_t *js, ant_realm_t *realm);

bool ant_realm_foreign_array_ctor(ant_t *js, ant_value_t ctor);
void ant_realm_mark(ant_t *js, void (*mark)(ant_t *js, ant_value_t value));
void ant_realm_reach_slow(ant_t *js, ant_realm_t *realm, void (*mark)(ant_t *js, ant_value_t value));

static inline void ant_realm_reach(ant_t *js, ant_realm_t *realm, void (*mark)(ant_t *js, ant_value_t value)) {
  if (realm && realm->gc_epoch != js->gc.epoch) ant_realm_reach_slow(js, realm, mark);
}

void ant_realm_collect(ant_t *js);
void ant_realm_destroy_all(ant_t *js);
ant_value_t realm_internal_library(ant_t *js);
void ant_realm_install_console(ant_t *js);

#endif
