// meson test -C build keyed-store-policy
#include "internal.h"
#include "gc/roots.h"
#include "silver/glue.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  char stack_base;
  ant_t *js = ant_create();
  assert(js);
  js_setstackbase(js, &stack_base);

  // Direct calls guarantee coverage of both VM-facing stores and JIT fallback
  // routing; value-only JS assertions cannot detect excessive shape sharing.
  for (unsigned jit = 0; jit < 2; jit++) {
    for (unsigned symbols = 0; symbols < 2; symbols++) {
      GC_ROOT_SAVE(roots, js);
      ant_value_t obj = js_mkobj(js);
      ant_value_t key = js_mkundef();
      GC_ROOT_PIN(js, obj);
      GC_ROOT_PIN(js, key);
      for (unsigned i = 0; i < 65; i++) {
        char name[24];
        int len = snprintf(name, sizeof(name), "%u", 50000 + i);
        key = symbols ? js_mksym(js, name)
          : jit ? js_mknum(50000 + i) : js_mkstr(js, name, len);
        ant_value_t result = jit
          ? jit_helper_put_elem(js->vm, js, obj, key, js_mknum(i))
          : js_setprop_keyed(js, obj, key, js_mknum(i));
        assert(vtype(result) != kTypeError);
      }
      assert(ant_shape_count(js_obj_ptr(obj)->shape) == 65);
      assert(!ant_shape_is_shared(js_obj_ptr(obj)->shape));
      GC_ROOT_RESTORE(js, roots);
    }
  }
  js_destroy(js);
  puts("PASS numeric and symbol stores use keyed shape policy in VM and JIT helpers");
}
