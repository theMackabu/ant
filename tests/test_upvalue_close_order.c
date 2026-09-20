#include "silver/upvalues.h"
#include <assert.h>
#include <stdio.h>

static void ordered(sv_upvalue_t *head) {
  for (; head && head->next; head = head->next)
    assert((uintptr_t)head->location >= (uintptr_t)head->next->location);
}

static void mixed_cleanup(void) {
  ant_value_t slots[64];
  for (int i = 0; i < 64; i++) slots[i] = js_mknum(i);
  sv_upvalue_t low = {.location = &slots[4]};
  sv_upvalue_t outer = {.location = &slots[18], .next = &low};
  sv_upvalue_t a = {.location = &slots[25], .next = &outer};
  sv_upvalue_t b = {.location = &slots[29], .next = &a};
  sv_upvalue_t high = {.location = &slots[60], .next = &b};
  sv_vm_t vm = {.stack = &slots[16], .stack_size = 32, .open_upvalues = &high};

  sv_close_upvalues_from_slot(&vm, &slots[24]);
  assert(vm.open_upvalues == &high && high.next == &outer && outer.next == &low);
  assert(a.location == &a.closed && a.closed == js_mknum(25) && !a.next);
  assert(b.location == &b.closed && b.closed == js_mknum(29) && !b.next);
  assert(high.location == &slots[60] && low.location == &slots[4]);
  slots[18] = js_mknum(42);
  sv_close_upvalues_from_slot(&vm, &slots[24]);
  assert(*outer.location == js_mknum(42));
  sv_close_upvalues_from_slot(&vm, vm.stack);
  assert(outer.location == &outer.closed && outer.closed == js_mknum(42));
  assert(high.next == &low && !low.next);
  ordered(vm.open_upvalues);
}

static void rebasing(void) {
  ant_value_t slots[64];
  for (int i = 0; i < 64; i++) slots[i] = js_mknum(i);
  sv_upvalue_t low = {.location = &slots[4]};
  sv_upvalue_t outer = {.location = &slots[20], .next = &low};
  sv_upvalue_t a = {.location = &slots[25], .next = &outer};
  sv_upvalue_t b = {.location = &slots[28], .next = &a};
  sv_upvalue_t high = {.location = &slots[60], .next = &b};
  sv_upvalue_t *head = &high;

  memcpy(slots + 48, slots + 24, 8 * sizeof(*slots));
  sv_rebase_open_upvalues(&head, slots + 24, slots + 48, 8);
  assert(b.location == slots + 52 && a.location == slots + 49);
  assert(*b.location == js_mknum(28) && *a.location == js_mknum(25));
  ordered(head);

  memcpy(slots + 8, slots + 48, 8 * sizeof(*slots));
  sv_rebase_open_upvalues(&head, slots + 48, slots + 8, 8);
  assert(head == &high && high.next == &outer && outer.next == &b);
  assert(b.location == slots + 12 && a.location == slots + 9);
  assert(b.next == &a && a.next == &low);
  ordered(head);
  sv_rebase_open_upvalues(&head, NULL, slots, 8);
  sv_rebase_open_upvalues(&head, slots, slots, 64);
  ordered(head);
}

static void activation_order(void) {
  ant_value_t slots[64] = {0};
  ant_value_t saved[2] = {0};
  sv_frame_t frames[4] = {0};
  sv_frame_t saved_frame = {.bp = saved, .lp = saved + 1, .arguments_obj = js_mkundef()};
  sv_upvalue_t outer = {.location = slots + 17};
  sv_upvalue_t native = {.location = slots + 60, .next = &outer};
  sv_upvalue_t a = {.location = saved};
  sv_upvalue_t b = {.location = saved + 1, .next = &a};
  sv_vm_t vm = {
    .stack = slots + 16, .stack_size = 32, .sp = 3,
    .frames = frames, .max_frames = 4, .fp = 0, .open_upvalues = &native
  };
  sv_activation_t act = {
    .slots = saved, .stack_count = 2, .frames = &saved_frame,
    .frame_count = 1, .open_upvalues = &b
  };

  assert(sv_activation_install(&vm, &act));
  assert(vm.open_upvalues == &native && native.next == &b);
  assert(b.location == slots + 20 && a.location == slots + 19);
  assert(a.next == &outer && !act.open_upvalues);
  ordered(vm.open_upvalues);

  sv_activation_t *captured = sv_activation_capture(&vm, 1, NULL);
  assert(captured && captured->open_upvalues == &b && b.next == &a && !a.next);
  assert(vm.open_upvalues == &native && native.next == &outer);
  ordered(captured->open_upvalues);
  vm.sp = 8;
  assert(sv_activation_install(&vm, captured));
  ordered(vm.open_upvalues);
  assert(b.location == vm.stack + 9 && a.location == vm.stack + 8);
  sv_close_upvalues_from_slot(&vm, vm.stack + 8);
  assert(native.next == &outer && outer.location == slots + 17);
  assert(a.location == &a.closed && b.location == &b.closed);
  free(captured);
}

static void close_barrier(void) {
  ant_t *js = ant_create();
  assert(js);
  ant_value_t value = js_mkobj(js);
  sv_upvalue_t *uv = js_upvalue_alloc(js);
  assert(uv);
  uv->location = &value;
  uv->gc_epoch = 1;
  sv_vm_t vm = {.js = js, .stack = &value, .stack_size = 1, .open_upvalues = uv};
  sv_close_upvalues_from_slot(&vm, &value);
  assert(!vm.open_upvalues && uv->location == &uv->closed && uv->closed == value);
  assert(uv->in_remember_set);
  js_destroy(js);
}

int main(void) {
  mixed_cleanup();
  rebasing();
  activation_order();
  close_barrier();
  puts("PASS ordered capture cleanup, relocation, generator resume and write barrier");
}
