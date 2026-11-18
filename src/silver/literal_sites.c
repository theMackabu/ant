#include "internal.h"
#include "arrays.h"
#include "silver/literal_sites.h"

void Silver_Literal_InitElement(ant_t *js, ant_value_t arr, uint32_t index, ant_value_t value) {
  ant_object_t *ptr = (ant_object_t *)vptr(arr);
  if (ptr->flags.fast_array && ptr->u.array.data && index < ptr->u.array.len && index < ptr->u.array.cap) {
    ptr->u.array.data[index] = value;
    gc_write_barrier_elem(js, ptr, index, value);
  } else arr_set(js, arr, index, value);
}

static ant_value_t Literal_FromBlock(ant_t *js, const sv_literal_site_t *site) {
  return Ant_Array_CreateFromBlock(js, site->block, site->flags & SV_LITERAL_HAS_DYNAMIC);
}

ant_value_t Silver_Literal_Create(ant_t *js, sv_func_t *func, uint32_t index) {
  sv_literal_site_t *site = &func->debug->literal_sites[index];
  if (site->block) return Literal_FromBlock(js, site);
  
  return js_mkarr_dense_literal(js, func->constants + site->start, site->count);
}
