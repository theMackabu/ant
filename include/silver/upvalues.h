#ifndef SILVER_UPVALUES_H
#define SILVER_UPVALUES_H

#include "silver/engine.h"

static inline void sv_merge_open_upvalues(sv_upvalue_t **head, sv_upvalue_t *incoming) {
  while (*head && incoming) {
    if ((uintptr_t)(*head)->location < (uintptr_t)incoming->location) {
      sv_upvalue_t *uv = incoming;
      incoming = uv->next;
      uv->next = *head;
      *head = uv;
    }
    
    head = &(*head)->next;
  }

  if (incoming) *head = incoming;
}

static inline void sv_rebase_open_upvalues(
  sv_upvalue_t **head, const ant_value_t *from, ant_value_t *to, size_t count
) {
  if (!from || !to || from == to || count == 0) return;
  
  uintptr_t lo = (uintptr_t)from;
  uintptr_t hi = lo + count * sizeof(*from);
  
  sv_upvalue_t *moved = NULL;
  sv_upvalue_t **tail = &moved;
  sv_upvalue_t **pp = head;

  while (*pp && (uintptr_t)(*pp)->location >= lo) {
    sv_upvalue_t *uv = *pp;
    uintptr_t addr = (uintptr_t)uv->location;
    
    if (addr >= hi) { 
      pp = &uv->next;
      continue;
    }
    
    *pp = uv->next;
    size_t index = (addr - lo) / sizeof(*from);
    
    uv->location = &to[index];
    uv->next = NULL;
    
    *tail = uv;
    tail = &uv->next;
  }
  
  sv_merge_open_upvalues(head, moved);
}

static inline void sv_close_upvalues_from_slot(sv_vm_t *vm, ant_value_t *slot) {
  sv_upvalue_t **pp = &vm->open_upvalues;
  
  while (*pp) {
    sv_upvalue_t *uv = *pp;
    ant_value_t *loc = uv->location;
    if ((uintptr_t)loc < (uintptr_t)slot) break;
    
    if (sv_slot_in_vm_stack(vm, loc)) {
      uv->closed = *loc;
      uv->location = &uv->closed;
      *pp = uv->next;
      uv->next = NULL;
      gc_upvalue_write_barrier(vm->js, uv, uv->closed);
    } 
    
    else pp = &uv->next;
  }
}

#endif
