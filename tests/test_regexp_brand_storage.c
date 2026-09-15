// meson test -C build regexp-brand-storage
#include "internal.h"
#include "gc/roots.h"
#include <assert.h>
#include <stdio.h>

static_assert(sizeof(ant_object_flags_t) == 2, "object flags grew");
static_assert(sizeof(void *) != 8 || ANT_INOBJ_MAX_SLOTS != 4 || sizeof(ant_object_t) == 152,
              "ordinary 64-bit object layout grew");

int main(void) {
  ant_t *js = ant_create();
  assert(js);
  js_setstackbase(js, NULL);
  assert(ant_object_flag_masks_match_layout());
  GC_ROOT_SAVE(roots, js);
  ant_value_t object = js_mkobj(js);
  GC_ROOT_PIN(js, object);
  ant_value_t flags = js_mkstr(js, "g", 1);
  GC_ROOT_PIN(js, flags);
  ant_object_t *ptr = js_obj_ptr(object);
  assert(!ptr->flags.regexp_brand);
  js_set_slot(object, SLOT_REGEXP_FLAGS_STRING, flags);
  assert(ptr->flags.regexp_brand && js_get_slot(object, SLOT_REGEXP_FLAGS_STRING) == flags);
  js_set_slot_wb(js, object, SLOT_REGEXP_FLAGS_STRING, js_mknum(1));
  assert(!ptr->flags.regexp_brand && js_get_slot(object, SLOT_REGEXP_FLAGS_STRING) == js_mknum(1));
  js_set_slot_wb(js, object, SLOT_REGEXP_FLAGS_STRING, flags);
  assert(ptr->flags.regexp_brand);
  js_set_slot(object, SLOT_REGEXP_FLAGS_STRING, js_mkundef());
  assert(!ptr->flags.regexp_brand);
  js_set_slot(object, SLOT_STRICT_ARGS, js_true);
  assert(!ptr->flags.regexp_brand && js_get_slot(object, SLOT_STRICT_ARGS) == js_true);
  // Adding unrelated native storage must not change the cached brand.
  js_set_slot_wb(js, object, SLOT_REGEXP_FLAGS_STRING, flags);
  js_set_slot(object, SLOT_DATA, js_mknum(9));
  assert(ptr->flags.regexp_brand);
  GC_ROOT_RESTORE(js, roots);
  js_destroy(js);
  puts("PASS RegExp brand storage and native slot overwrites");
  return 0;
}
