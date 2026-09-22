/**
 * Entry stub for calls into compiled code
 *
 * ant_jit_enter_clean() calls fn with every callee-saved general register
 * zeroed, then restores them. The callee-saved floating-point registers
 * (d8-d15 on AArch64) are left alone: they hold only doubles, never tagged
 * values, and the callee preserves them.
 *
 * The collector skips interpreter frames (see &gc_vm_seg_t) but scans compiled
 * frames conservatively. A compiled function's prologue saves the callee-saved
 * registers it uses, and those still hold its caller's values. If the caller
 * is the interpreter, a register it never touches in a long-running loop can
 * hold a stale object pointer, and every compiled frame would save a copy
 * where the scan finds it. Zeroing the registers here means compiled frames
 * only ever save zeros or their own values.
 *
 * The stub's own save area lies inside the compiled-frame segment. The
 * collector skips it when compiled frames are excluded and otherwise scans it
 * harmlessly.
 *
 * The stub keeps a normal frame record, so frame-pointer walks pass through it.
 */

#include "jit/entry_stub.h"

#if ANT_JIT_ENTER_STUB

#if defined(__APPLE__)
#define STUB_SYM "_ant_jit_enter_clean"
#else
#define STUB_SYM "ant_jit_enter_clean"
#endif

#if defined(__aarch64__)
__asm__(
  ".text\n"
  ".p2align 2\n"
  ".globl " STUB_SYM "\n"
#if !defined(__APPLE__)
  ".type " STUB_SYM ", %function\n"
#endif
  STUB_SYM ":\n"
  "  stp x29, x30, [sp, #-96]!\n"
  "  mov x29, sp\n"
  "  stp x19, x20, [sp, #16]\n"
  "  stp x21, x22, [sp, #32]\n"
  "  stp x23, x24, [sp, #48]\n"
  "  stp x25, x26, [sp, #64]\n"
  "  stp x27, x28, [sp, #80]\n"
  "  mov x16, x0\n"
  "  mov x0, x1\n"
  "  mov x1, x2\n"
  "  mov x2, x3\n"
  "  mov x3, x4\n"
  "  mov x4, x5\n"
  "  mov x5, x6\n"
  "  mov x6, x7\n"
  "  mov x19, xzr\n"
  "  mov x20, xzr\n"
  "  mov x21, xzr\n"
  "  mov x22, xzr\n"
  "  mov x23, xzr\n"
  "  mov x24, xzr\n"
  "  mov x25, xzr\n"
  "  mov x26, xzr\n"
  "  mov x27, xzr\n"
  "  mov x28, xzr\n"
  "  blr x16\n"
  "  ldp x27, x28, [sp, #80]\n"
  "  ldp x25, x26, [sp, #64]\n"
  "  ldp x23, x24, [sp, #48]\n"
  "  ldp x21, x22, [sp, #32]\n"
  "  ldp x19, x20, [sp, #16]\n"
  "  ldp x29, x30, [sp], #96\n"
  "  ret\n"
);
#elif defined(__x86_64__)
__asm__(
  ".text\n"
  ".p2align 4\n"
  ".globl " STUB_SYM "\n"
#if !defined(__APPLE__)
  ".type " STUB_SYM ", @function\n"
#endif
  STUB_SYM ":\n"
  "  push %rbp\n"
  "  mov %rsp, %rbp\n"
  "  push %rbx\n"
  "  push %r12\n"
  "  push %r13\n"
  "  push %r14\n"
  "  push %r15\n"
  "  mov %rdi, %rax\n"
  "  mov %rsi, %rdi\n"
  "  mov %rdx, %rsi\n"
  "  mov %rcx, %rdx\n"
  "  mov %r8, %rcx\n"
  "  mov %r9, %r8\n"
  "  movslq 16(%rbp), %r9\n"
  "  mov 24(%rbp), %r10\n"
  "  push %r10\n"
  "  xor %ebx, %ebx\n"
  "  xor %r12d, %r12d\n"
  "  xor %r13d, %r13d\n"
  "  xor %r14d, %r14d\n"
  "  xor %r15d, %r15d\n"
  "  call *%rax\n"
  "  add $8, %rsp\n"
  "  pop %r15\n"
  "  pop %r14\n"
  "  pop %r13\n"
  "  pop %r12\n"
  "  pop %rbx\n"
  "  pop %rbp\n"
  "  ret\n"
);
#endif

#endif
