#include "internal.h"
#include "gc/refs.h"

#include <stdlib.h>

void gc_refs_shrink(ant_t *js) {
  if (!js || js->prop_refs_cap <= 512) return;
  if (js->prop_refs_len >= js->prop_refs_cap / 4) return;

  ant_offset_t shrunk = js->prop_refs_len < 256 ? 256 : js->prop_refs_len * 2;
  ant_prop_ref_t *next = realloc(js->prop_refs, sizeof(*next) * shrunk);
  
  if (next) {
    js->prop_refs = next;
    js->prop_refs_cap = shrunk;
  }
}
