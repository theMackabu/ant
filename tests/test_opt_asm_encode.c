#include "a64.h"
#include "disasm.h"
#include "x64.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static int failures;

#define CHECK(cond, ...) do {                           \
  if (!(cond)) {                                        \
    if (failures < 40) {                                \
      fprintf(stderr, "  %s:%d: ", __FILE__, __LINE__); \
      fprintf(stderr, __VA_ARGS__);                     \
      fputc('\n', stderr);                              \
    }                                                   \
    failures++;                                         \
  }                                                     \
} while (0)

static constexpr asm_arch_t arch_x64 = ASM_ARCH_X64;
static constexpr asm_arch_t arch_a64 = ASM_ARCH_A64;
static constexpr uint32_t NOP = 0xd503201f;

static size_t parse_hex(const char *hex, uint8_t *out, size_t cap) {
  size_t n = 0;
  while (*hex) {
    if (*hex == ' ') {
      hex++;
      continue;
    }
    unsigned value;
    if (sscanf(hex, "%2x", &value) != 1 || n == cap) break;
    out[n++] = (uint8_t)value;
    hex += 2;
  }
  return n;
}

static void join_listing(asm_arch_t arch, const uint8_t *code, size_t start, size_t end, char *out, size_t cap) {
  size_t used = 0;
  out[0] = '\0';
  for (size_t offset = start; offset < end;) {
    asm_insn_t insn;
    size_t length = arch == ASM_ARCH_X64
      ? asm_disasm_x64(code, end, offset, &insn)
      : asm_disasm_a64(code, end, offset, &insn);
    int written = snprintf(out + used, cap - used, "%s%s", offset == start ? "" : "; ", insn.text);
    if (written < 0 || (size_t)written >= cap - used) break;
    used += (size_t)written;
    offset += length ? length : (arch == ASM_ARCH_X64 ? 1 : 4);
  }
}

static void check_encoding(
  asm_arch_t arch, const char *body, const char *expected, const char *hex,
  asm_error_t error, asm_code_t *code, size_t expected_data_offset
) {
  CHECK(error == ASM_OK, "%s: assembling failed (%d)", body, error);
  if (error != ASM_OK) return;
  uint8_t bytes[256];
  size_t len = parse_hex(hex, bytes, sizeof(bytes));
  bool same = len == code->len && memcmp(bytes, code->bytes, len) == 0;
  if (!same) {
    char actual[1024] = "";
    for (size_t i = 0, used = 0; i < code->len && used + 4 < sizeof(actual); i++)
      used += (size_t)snprintf(actual + used, sizeof(actual) - used, "%s%02x", i ? " " : "", code->bytes[i]);
    CHECK(same, "%s\n    expected bytes: %s\n    actual bytes:   %s", body, hex, actual);
  }
  if (expected_data_offset != SIZE_MAX)
    CHECK(code->data_offset == expected_data_offset, "%s: data offset %zu, expected %zu", body, code->data_offset, expected_data_offset);
  char text[2048];
  join_listing(arch, code->bytes, 0, code->data_offset, text, sizeof(text));
  CHECK(strcmp(text, expected) == 0, "%s\n    expected: %s\n    actual:   %s", body, expected, text);
  asm_code_free(code);
}

#define ENC(arch, expected, hex, ...) do {                                         \
  arch##_asm_t enc_asm;                                                            \
  arch##_asm_t *a = &enc_asm;                                                      \
  arch##_init(a);                                                                  \
  __VA_ARGS__;                                                                     \
  asm_code_t code;                                                                 \
  asm_error_t error = arch##_finish(a, &code);                                     \
  check_encoding(arch_##arch, #__VA_ARGS__, expected, hex, error, &code, SIZE_MAX); \
} while (0)

#define MASM_ENC(arch, expected, data_offset, hex, ...) do {                         \
  masm_##arch##_t enc_masm;                                                          \
  masm_##arch##_t *m = &enc_masm;                                                    \
  masm_##arch##_init(m);                                                             \
  __VA_ARGS__;                                                                       \
  asm_code_t code;                                                                   \
  asm_error_t error = masm_##arch##_finish(m, &code);                                \
  check_encoding(arch_##arch, #__VA_ARGS__, expected, hex, error, &code, data_offset); \
} while (0)

static asm_code_t finish_x64(x64_asm_t *a) {
  asm_code_t code;
  asm_error_t error = x64_finish(a, &code);
  CHECK(error == ASM_OK, "assembling failed (%d)", error);
  return code;
}

static asm_code_t finish_a64(a64_asm_t *a) {
  asm_code_t code;
  asm_error_t error = a64_finish(a, &code);
  CHECK(error == ASM_OK, "assembling failed (%d)", error);
  return code;
}

static bool bytes_equal(const asm_code_t *code, size_t at, const uint8_t *expected, size_t n) {
  return code->len >= at + n && memcmp(code->bytes + at, expected, n) == 0;
}

static uint32_t word_at(const uint8_t *code, size_t offset) {
  return (uint32_t)code[offset] | (uint32_t)code[offset + 1] << 8 | (uint32_t)code[offset + 2] << 16 | (uint32_t)code[offset + 3] << 24;
}

#if !defined(_WIN32)
static bool panics_with(void (*fn)(void), const char *needle) {
  int fds[2];
  if (pipe(fds) != 0) return false;
  fflush(stdout);
  fflush(stderr);
  pid_t pid = fork();
  if (pid == 0) {
    dup2(fds[1], STDERR_FILENO);
    close(fds[0]);
    signal(SIGABRT, SIG_DFL);
    fn();
    _exit(0);
  }
  close(fds[1]);
  char out[1024];
  size_t n = 0;
  ssize_t r;
  while (n < sizeof(out) - 1 && (r = read(fds[0], out + n, sizeof(out) - 1 - n)) > 0) n += (size_t)r;
  out[n] = '\0';
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT && strstr(out, needle) != NULL;
}
#endif

static void buffer_forward_and_backward_references_are_patched(void) {
  asm_buffer_t b;
  asm_buffer_init(&b);
  asm_label_t label = asm_new_label(&b);
  asm_emit_u8(&b, 0xeb);
  asm_emit_u8(&b, 0);
  CHECK(asm_add_fixup(&b, label, 1, ASM_FIXUP_X86_REL8) != ASM_NO_FIXUP, "forward fixup should be pending");
  asm_emit_bytes(&b, (const uint8_t[]){ 0x90, 0x90, 0x90 }, 3);
  asm_bind(&b, label);
  asm_emit_u8(&b, 0xe9);
  asm_emit_u32(&b, 0);
  CHECK(asm_add_fixup(&b, label, 6, ASM_FIXUP_X86_REL32) == ASM_NO_FIXUP, "backward fixup should resolve");
  asm_code_t code;
  CHECK(asm_buffer_finish(&b, &code) == ASM_OK, "finish failed");
  static const uint8_t expected[] = { 0xeb, 3, 0x90, 0x90, 0x90, 0xe9, 0xfb, 0xff, 0xff, 0xff };
  CHECK(code.len == sizeof(expected) && bytes_equal(&code, 0, expected, sizeof(expected)), "bad patch");
  asm_code_free(&code);
}

static void buffer_unbound_label_fails(void) {
  asm_buffer_t b;
  asm_buffer_init(&b);
  asm_label_t label = asm_new_label(&b);
  asm_emit_u32(&b, 0x14000000);
  asm_add_fixup(&b, label, 0, ASM_FIXUP_A64_BRANCH26);
  asm_code_t code;
  CHECK(asm_buffer_finish(&b, &code) == ASM_ERR_UNBOUND_LABEL, "expected unbound label");
}

static void buffer_unreferenced_unbound_label_is_fine(void) {
  asm_buffer_t b;
  asm_buffer_init(&b);
  asm_new_label(&b);
  asm_emit_u8(&b, 0xc3);
  asm_code_t code;
  CHECK(asm_buffer_finish(&b, &code) == ASM_OK && code.len == 1 && code.bytes[0] == 0xc3, "expected [c3]");
  asm_code_free(&code);
}

static void buffer_out_of_range_reference_fails(void) {
  asm_buffer_t b;
  asm_buffer_init(&b);
  asm_label_t label = asm_new_label(&b);
  asm_emit_bytes(&b, (const uint8_t[]){ 0xeb, 0 }, 2);
  asm_add_fixup(&b, label, 1, ASM_FIXUP_X86_REL8);
  for (int i = 0; i < 128; i++) asm_emit_u8(&b, 0x90);
  asm_bind(&b, label);
  asm_code_t code;
  CHECK(asm_buffer_finish(&b, &code) == ASM_ERR_BRANCH_OUT_OF_RANGE, "expected out of range");
}

static void buffer_rel8_reaches_exactly_its_range(void) {
  CHECK(asm_fixup_reaches(ASM_FIXUP_X86_REL8, 1, 2 + 127), "rel8 +127");
  CHECK(!asm_fixup_reaches(ASM_FIXUP_X86_REL8, 1, 2 + 128), "rel8 +128");
  CHECK(asm_fixup_reaches(ASM_FIXUP_X86_REL8, 200, 201 - 128), "rel8 -128");
  CHECK(!asm_fixup_reaches(ASM_FIXUP_X86_REL8, 200, 201 - 129), "rel8 -129");
  CHECK(asm_fixup_reaches(ASM_FIXUP_A64_BRANCH14, 0, 32764), "branch14 forward");
  CHECK(!asm_fixup_reaches(ASM_FIXUP_A64_BRANCH14, 0, 32768), "branch14 forward out");
  CHECK(asm_fixup_reaches(ASM_FIXUP_A64_BRANCH14, 32768, 0), "branch14 backward");
  CHECK(!asm_fixup_reaches(ASM_FIXUP_A64_BRANCH14, 32772, 0), "branch14 backward out");
}

static void buffer_alignment_pads_with_filler(void) {
  asm_buffer_t b;
  asm_buffer_init(&b);
  asm_emit_u8(&b, 0xc3);
  asm_align_with(&b, 8, 0xcc);
  asm_code_t code;
  CHECK(asm_buffer_finish(&b, &code) == ASM_OK, "finish failed");
  static const uint8_t expected[] = { 0xc3, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc };
  CHECK(code.len == 8 && bytes_equal(&code, 0, expected, 8), "bad padding");
  asm_code_free(&code);
}

static void x64_register_moves_table(void) {
  ENC(x64, "mov rax, rbx", "48 89 d8",
    x64_mov_rr(a, ASM_W64, X64_RAX, X64_RBX));
  ENC(x64, "mov r8d, r15d", "45 89 f8",
    x64_mov_rr(a, ASM_W32, X64_R8, X64_R15));
  ENC(x64, "mov rsp, rbp", "48 89 ec",
    x64_mov_rr(a, ASM_W64, X64_RSP, X64_RBP));
  ENC(x64, "xchg rcx, r9", "4c 87 c9",
    x64_xchg_rr(a, ASM_W64, X64_RCX, X64_R9));
  ENC(x64, "movzx eax, sil", "40 0f b6 c6",
    x64_movzx8_rr(a, X64_RAX, X64_RSI));
  ENC(x64, "movzx r10d, r11b", "45 0f b6 d3",
    x64_movzx8_rr(a, X64_R10, X64_R11));
  ENC(x64, "movzx eax, cx", "0f b7 c1",
    x64_movzx16_rr(a, X64_RAX, X64_RCX));
  ENC(x64, "movsx rax, bl", "48 0f be c3",
    x64_movsx8_rr(a, ASM_W64, X64_RAX, X64_RBX));
  ENC(x64, "movsx ecx, dil", "40 0f be cf",
    x64_movsx8_rr(a, ASM_W32, X64_RCX, X64_RDI));
  ENC(x64, "movsx r12, r13w", "4d 0f bf e5",
    x64_movsx16_rr(a, ASM_W64, X64_R12, X64_R13));
  ENC(x64, "movsxd rax, ecx", "48 63 c1",
    x64_movsxd_rr(a, X64_RAX, X64_RCX));
  ENC(x64, "movsxd r15, r8d", "4d 63 f8",
    x64_movsxd_rr(a, X64_R15, X64_R8));
}

static void x64_immediate_moves_pick_the_shortest_form_table(void) {
  ENC(x64, "mov eax, 0", "b8 00 00 00 00",
    x64_mov_ri(a, X64_RAX, 0));
  ENC(x64, "mov r9d, 0x7f", "41 b9 7f 00 00 00",
    x64_mov_ri(a, X64_R9, 0x7f));
  ENC(x64, "mov eax, 0xffffffff", "b8 ff ff ff ff",
    x64_mov_ri(a, X64_RAX, 0xffffffff));
  ENC(x64, "mov rax, 0xffffffffffffffff", "48 c7 c0 ff ff ff ff",
    x64_mov_ri(a, X64_RAX, UINT64_MAX));
  ENC(x64, "mov r12, 0xffffffff80000000", "49 c7 c4 00 00 00 80",
    x64_mov_ri(a, X64_R12, 0xffffffff80000000));
  ENC(x64, "mov rax, 0x123456789abc", "48 b8 bc 9a 78 56 34 12 00 00",
    x64_mov_ri(a, X64_RAX, 0x123456789abc));
  ENC(x64, "mov r15, 0x8000000000000000", "49 bf 00 00 00 00 00 00 00 80",
    x64_mov_ri(a, X64_R15, (UINT64_C(1) << 63)));
  ENC(x64, "mov r11, 0x100000000", "49 bb 00 00 00 00 01 00 00 00",
    x64_movabs(a, X64_R11, 0x100000000));
  ENC(x64, "mov ecx, 0x12345678", "b9 78 56 34 12",
    x64_mov_ri32(a, X64_RCX, 0x12345678));
  ENC(x64, "mov rdx, 0xfffffffffffffffe", "48 c7 c2 fe ff ff ff",
    x64_mov_ri_sign_extended(a, X64_RDX, -2));
}

static void x64_memory_operands_table(void) {
  ENC(x64, "mov rax, qword ptr [rbx]", "48 8b 03",
    x64_mov_rm(a, ASM_W64, X64_RAX, asm_addr(X64_RBX, 0)));
  ENC(x64, "mov rax, qword ptr [rsp]", "48 8b 04 24",
    x64_mov_rm(a, ASM_W64, X64_RAX, asm_addr(X64_RSP, 0)));
  ENC(x64, "mov rax, qword ptr [r12]", "49 8b 04 24",
    x64_mov_rm(a, ASM_W64, X64_RAX, asm_addr(X64_R12, 0)));
  ENC(x64, "mov rax, qword ptr [rbp]", "48 8b 45 00",
    x64_mov_rm(a, ASM_W64, X64_RAX, asm_addr(X64_RBP, 0)));
  ENC(x64, "mov rax, qword ptr [r13]", "49 8b 45 00",
    x64_mov_rm(a, ASM_W64, X64_RAX, asm_addr(X64_R13, 0)));
  ENC(x64, "mov rax, qword ptr [rsp+8]", "48 8b 44 24 08",
    x64_mov_rm(a, ASM_W64, X64_RAX, asm_addr(X64_RSP, 8)));
  ENC(x64, "mov r9, qword ptr [r12-0x80]", "4d 8b 4c 24 80",
    x64_mov_rm(a, ASM_W64, X64_R9, asm_addr(X64_R12, -0x80)));
  ENC(x64, "mov rax, qword ptr [rbx+0x80]", "48 8b 83 80 00 00 00",
    x64_mov_rm(a, ASM_W64, X64_RAX, asm_addr(X64_RBX, 0x80)));
  ENC(x64, "mov rax, qword ptr [rbx-0x81]", "48 8b 83 7f ff ff ff",
    x64_mov_rm(a, ASM_W64, X64_RAX, asm_addr(X64_RBX, -0x81)));
  ENC(x64, "mov eax, dword ptr [r13+0x7fffffff]", "41 8b 85 ff ff ff 7f",
    x64_mov_rm(a, ASM_W32, X64_RAX, asm_addr(X64_R13, INT32_MAX)));
  ENC(x64, "mov rax, qword ptr [rax+rcx*4+0x10]", "48 8b 44 88 10",
    x64_mov_rm(a, ASM_W64, X64_RAX, asm_addr_indexed(X64_RAX, X64_RCX, ASM_SCALE_4, 0x10)));
  ENC(x64, "mov rax, qword ptr [rax+rcx]", "48 8b 04 08",
    x64_mov_rm(a, ASM_W64, X64_RAX, asm_addr_indexed(X64_RAX, X64_RCX, ASM_SCALE_1, 0)));
  ENC(x64, "mov r13d, dword ptr [r13+r12*8+0x100]", "47 8b ac e5 00 01 00 00",
    x64_mov_rm(a, ASM_W32, X64_R13, asm_addr_indexed(X64_R13, X64_R12, ASM_SCALE_8, 0x100)));
  ENC(x64, "mov rax, qword ptr [r13+r12*8]", "4b 8b 44 e5 00",
    x64_mov_rm(a, ASM_W64, X64_RAX, asm_addr_indexed(X64_R13, X64_R12, ASM_SCALE_8, 0)));
  ENC(x64, "mov rax, qword ptr [rbp+r15*2-4]", "4a 8b 44 7d fc",
    x64_mov_rm(a, ASM_W64, X64_RAX, asm_addr_indexed(X64_RBP, X64_R15, ASM_SCALE_2, -4)));
  ENC(x64, "mov rax, qword ptr [rsp+r9]", "4a 8b 04 0c",
    x64_mov_rm(a, ASM_W64, X64_RAX, asm_addr_indexed(X64_RSP, X64_R9, ASM_SCALE_1, 0)));
  ENC(x64, "mov rax, qword ptr [r12+r13*2]", "4b 8b 04 6c",
    x64_mov_rm(a, ASM_W64, X64_RAX, asm_addr_indexed(X64_R12, X64_R13, ASM_SCALE_2, 0)));
  ENC(x64, "mov qword ptr [rdi+8], rsi", "48 89 77 08",
    x64_mov_mr(a, ASM_W64, asm_addr(X64_RDI, 8), X64_RSI));
  ENC(x64, "mov dword ptr [rdi], r9d", "44 89 0f",
    x64_mov_mr(a, ASM_W32, asm_addr(X64_RDI, 0), X64_R9));
  ENC(x64, "mov qword ptr [rax], 0xffffffffffffffff", "48 c7 00 ff ff ff ff",
    x64_mov_mi(a, ASM_W64, asm_addr(X64_RAX, 0), -1));
  ENC(x64, "mov dword ptr [rax+4], 7", "c7 40 04 07 00 00 00",
    x64_mov_mi(a, ASM_W32, asm_addr(X64_RAX, 4), 7));
  ENC(x64, "mov qword ptr [rsp+0x10], 0x12345678", "48 c7 44 24 10 78 56 34 12",
    x64_mov_mi(a, ASM_W64, asm_addr(X64_RSP, 0x10), 0x12345678));
  ENC(x64, "mov byte ptr [rax], sil", "40 88 30",
    x64_mov8_mr(a, asm_addr(X64_RAX, 0), X64_RSI));
  ENC(x64, "mov byte ptr [rax], bl", "88 18",
    x64_mov8_mr(a, asm_addr(X64_RAX, 0), X64_RBX));
  ENC(x64, "mov byte ptr [r8+1], r9b", "45 88 48 01",
    x64_mov8_mr(a, asm_addr(X64_R8, 1), X64_R9));
  ENC(x64, "mov word ptr [rax], cx", "66 89 08",
    x64_mov16_mr(a, asm_addr(X64_RAX, 0), X64_RCX));
  ENC(x64, "mov word ptr [r10], r11w", "66 45 89 1a",
    x64_mov16_mr(a, asm_addr(X64_R10, 0), X64_R11));
  ENC(x64, "mov byte ptr [rax], 0x80", "c6 00 80",
    x64_mov8_mi(a, asm_addr(X64_RAX, 0), 0x80));
  ENC(x64, "mov word ptr [rax+2], 0x1234", "66 c7 40 02 34 12",
    x64_mov16_mi(a, asm_addr(X64_RAX, 2), 0x1234));
  ENC(x64, "movzx r10d, byte ptr [r11]", "45 0f b6 13",
    x64_movzx8_rm(a, X64_R10, asm_addr(X64_R11, 0)));
  ENC(x64, "movzx eax, word ptr [rcx+2]", "0f b7 41 02",
    x64_movzx16_rm(a, X64_RAX, asm_addr(X64_RCX, 2)));
  ENC(x64, "movsx eax, byte ptr [rcx]", "0f be 01",
    x64_movsx8_rm(a, ASM_W32, X64_RAX, asm_addr(X64_RCX, 0)));
  ENC(x64, "movsx rax, byte ptr [rcx]", "48 0f be 01",
    x64_movsx8_rm(a, ASM_W64, X64_RAX, asm_addr(X64_RCX, 0)));
  ENC(x64, "movsx rax, word ptr [rcx]", "48 0f bf 01",
    x64_movsx16_rm(a, ASM_W64, X64_RAX, asm_addr(X64_RCX, 0)));
  ENC(x64, "movsxd rax, dword ptr [rcx]", "48 63 01",
    x64_movsxd_rm(a, X64_RAX, asm_addr(X64_RCX, 0)));
  ENC(x64, "lea rax, [rbx+rcx*8+0x10]", "48 8d 44 cb 10",
    x64_lea(a, ASM_W64, X64_RAX, asm_addr_indexed(X64_RBX, X64_RCX, ASM_SCALE_8, 0x10)));
  ENC(x64, "lea eax, [rbx+5]", "8d 43 05",
    x64_lea(a, ASM_W32, X64_RAX, asm_addr(X64_RBX, 5)));
  ENC(x64, "lea r12, [rsp-8]", "4c 8d 64 24 f8",
    x64_lea(a, ASM_W64, X64_R12, asm_addr(X64_RSP, -8)));
}

static void x64_arithmetic_and_logic_table(void) {
  ENC(x64, "add rax, rbx", "48 01 d8",
    x64_add_rr(a, ASM_W64, X64_RAX, X64_RBX));
  ENC(x64, "sub r8d, r9d", "45 29 c8",
    x64_sub_rr(a, ASM_W32, X64_R8, X64_R9));
  ENC(x64, "and rcx, r15", "4c 21 f9",
    x64_and_rr(a, ASM_W64, X64_RCX, X64_R15));
  ENC(x64, "or esi, edi", "09 fe",
    x64_or_rr(a, ASM_W32, X64_RSI, X64_RDI));
  ENC(x64, "xor r11, r11", "4d 31 db",
    x64_xor_rr(a, ASM_W64, X64_R11, X64_R11));
  ENC(x64, "cmp rax, rdx", "48 39 d0",
    x64_cmp_rr(a, ASM_W64, X64_RAX, X64_RDX));
  ENC(x64, "adc rax, rbx", "48 11 d8",
    x64_alu_rr(a, X64_ALU_ADC, ASM_W64, X64_RAX, X64_RBX));
  ENC(x64, "sbb eax, ebx", "19 d8",
    x64_alu_rr(a, X64_ALU_SBB, ASM_W32, X64_RAX, X64_RBX));
  ENC(x64, "add rax, 1", "48 83 c0 01",
    x64_add_ri(a, ASM_W64, X64_RAX, 1));
  ENC(x64, "add rax, 0x1000", "48 81 c0 00 10 00 00",
    x64_add_ri(a, ASM_W64, X64_RAX, 0x1000));
  ENC(x64, "add rax, 0xffffffffffffffff", "48 83 c0 ff",
    x64_add_ri(a, ASM_W64, X64_RAX, -1));
  ENC(x64, "sub r15d, 0xffffff80", "41 83 ef 80",
    x64_sub_ri(a, ASM_W32, X64_R15, -128));
  ENC(x64, "and eax, 0xff", "81 e0 ff 00 00 00",
    x64_and_ri(a, ASM_W32, X64_RAX, 0xff));
  ENC(x64, "or r9, 0x7f", "49 83 c9 7f",
    x64_or_ri(a, ASM_W64, X64_R9, 0x7f));
  ENC(x64, "xor esp, 0x80000000", "81 f4 00 00 00 80",
    x64_xor_ri(a, ASM_W32, X64_RSP, INT32_MIN));
  ENC(x64, "cmp r12, 0xffffffffffff8000", "49 81 fc 00 80 ff ff",
    x64_cmp_ri(a, ASM_W64, X64_R12, -0x8000));
  ENC(x64, "add rax, qword ptr [rbx]", "48 03 03",
    x64_alu_rm(a, X64_ALU_ADD, ASM_W64, X64_RAX, asm_addr(X64_RBX, 0)));
  ENC(x64, "add qword ptr [rbx], rax", "48 01 03",
    x64_alu_mr(a, X64_ALU_ADD, ASM_W64, asm_addr(X64_RBX, 0), X64_RAX));
  ENC(x64, "sub dword ptr [rbx+8], 1", "83 6b 08 01",
    x64_alu_mi(a, X64_ALU_SUB, ASM_W32, asm_addr(X64_RBX, 8), 1));
  ENC(x64, "cmp dword ptr [rax], 0x12345678", "81 38 78 56 34 12",
    x64_cmp_mi(a, ASM_W32, asm_addr(X64_RAX, 0), 0x12345678));
  ENC(x64, "cmp word ptr [r13+0xe], 0xfffc", "66 41 83 7d 0e fc",
    x64_cmp16_mi8(a, asm_addr(X64_R13, 14), -4));
  ENC(x64, "cmp qword ptr [r12], r13", "4d 39 2c 24",
    x64_cmp_mr(a, ASM_W64, asm_addr(X64_R12, 0), X64_R13));
  ENC(x64, "test rax, rax", "48 85 c0",
    x64_test_rr(a, ASM_W64, X64_RAX, X64_RAX));
  ENC(x64, "test r8d, ecx", "41 85 c8",
    x64_test_rr(a, ASM_W32, X64_R8, X64_RCX));
  ENC(x64, "test eax, 0x100", "f7 c0 00 01 00 00",
    x64_test_ri(a, ASM_W32, X64_RAX, 0x100));
  ENC(x64, "test r14, 0x100", "49 f7 c6 00 01 00 00",
    x64_test_ri(a, ASM_W64, X64_R14, 0x100));
  ENC(x64, "test qword ptr [rax], 1", "48 f7 00 01 00 00 00",
    x64_test_mi(a, ASM_W64, asm_addr(X64_RAX, 0), 1));
  ENC(x64, "test byte ptr [rax+1], 0x80", "f6 40 01 80",
    x64_test8_mi(a, asm_addr(X64_RAX, 1), 0x80));
  ENC(x64, "neg rax", "48 f7 d8",
    x64_neg(a, ASM_W64, X64_RAX));
  ENC(x64, "not r9d", "41 f7 d1",
    x64_not(a, ASM_W32, X64_R9));
  ENC(x64, "shl rax, 1", "48 d1 e0",
    x64_shl_ri(a, ASM_W64, X64_RAX, 1));
  ENC(x64, "shl ecx, 5", "c1 e1 05",
    x64_shl_ri(a, ASM_W32, X64_RCX, 5));
  ENC(x64, "shr r12, 0x3f", "49 c1 ec 3f",
    x64_shr_ri(a, ASM_W64, X64_R12, 63));
  ENC(x64, "sar r9d, 0x1f", "41 c1 f9 1f",
    x64_sar_ri(a, ASM_W32, X64_R9, 31));
  ENC(x64, "rol rax, 3", "48 c1 c0 03",
    x64_shift_ri(a, X64_SHIFT_ROL, ASM_W64, X64_RAX, 3));
  ENC(x64, "ror eax, 1", "d1 c8",
    x64_shift_ri(a, X64_SHIFT_ROR, ASM_W32, X64_RAX, 1));
  ENC(x64, "shl rdx, cl", "48 d3 e2",
    x64_shift_rcl(a, X64_SHIFT_SHL, ASM_W64, X64_RDX));
  ENC(x64, "shr r10, cl", "49 d3 ea",
    x64_shift_rcl(a, X64_SHIFT_SHR, ASM_W64, X64_R10));
  ENC(x64, "sar edx, cl", "d3 fa",
    x64_shift_rcl(a, X64_SHIFT_SAR, ASM_W32, X64_RDX));
  ENC(x64, "imul eax, ecx", "0f af c1",
    x64_imul_rr(a, ASM_W32, X64_RAX, X64_RCX));
  ENC(x64, "imul r8, r9", "4d 0f af c1",
    x64_imul_rr(a, ASM_W64, X64_R8, X64_R9));
  ENC(x64, "imul rax, rbx, 0xa", "48 6b c3 0a",
    x64_imul_rri(a, ASM_W64, X64_RAX, X64_RBX, 10));
  ENC(x64, "imul eax, r13d, 0x3e8", "41 69 c5 e8 03 00 00",
    x64_imul_rri(a, ASM_W32, X64_RAX, X64_R13, 1000));
  ENC(x64, "cdq", "99",
    x64_sign_extend_rax_into_rdx(a, ASM_W32));
  ENC(x64, "cqo", "48 99",
    x64_sign_extend_rax_into_rdx(a, ASM_W64));
  ENC(x64, "idiv ecx", "f7 f9",
    x64_idiv(a, ASM_W32, X64_RCX));
  ENC(x64, "idiv r11", "49 f7 fb",
    x64_idiv(a, ASM_W64, X64_R11));
}

static void x64_condition_codes_table(void) {
  ENC(x64, "sete sil", "40 0f 94 c6",
    x64_setcc(a, X64_COND_EQUAL, X64_RSI));
  ENC(x64, "setl r9b", "41 0f 9c c1",
    x64_setcc(a, X64_COND_LESS, X64_R9));
  ENC(x64, "setg bl", "0f 9f c3",
    x64_setcc(a, X64_COND_GREATER, X64_RBX));
  ENC(x64, "cmovl eax, r8d", "41 0f 4c c0",
    x64_cmovcc(a, X64_COND_LESS, ASM_W32, X64_RAX, X64_R8));
}

static void x64_branches_to_labels_table(void) {
  ENC(x64, "nop; je short 0", "90 74 fd",
    asm_label_t label = x64_new_label(a); x64_bind(a, label); x64_nop(a); x64_jcc(a, X64_COND_EQUAL, label));
  ENC(x64, "jmp short 0", "eb fe",
    asm_label_t label = x64_new_label(a); x64_bind(a, label); x64_jmp(a, label));
  ENC(x64, "jne 6; nop", "0f 85 00 00 00 00 90",
    asm_label_t label = x64_new_label(a); x64_jcc(a, X64_COND_NOT_EQUAL, label); x64_bind(a, label); x64_nop(a));
  ENC(x64, "jmp 5", "e9 00 00 00 00",
    asm_label_t label = x64_new_label(a); x64_jmp(a, label); x64_bind(a, label));
  ENC(x64, "jne short 2", "75 00",
    asm_label_t label = x64_new_label(a); x64_jcc_short(a, X64_COND_NOT_EQUAL, label); x64_bind(a, label));
  ENC(x64, "jmp short 2", "eb 00",
    asm_label_t label = x64_new_label(a); x64_jmp_short(a, label); x64_bind(a, label));
  ENC(x64, "call 5", "e8 00 00 00 00",
    asm_label_t label = x64_new_label(a); x64_call(a, label); x64_bind(a, label));
  ENC(x64, "lea rax, [7]", "48 8d 05 00 00 00 00",
    asm_label_t label = x64_new_label(a); x64_lea_label(a, X64_RAX, label); x64_bind(a, label));
  ENC(x64, "movsd xmm9, qword ptr [9]", "f2 44 0f 10 0d 00 00 00 00",
    asm_label_t label = x64_new_label(a); x64_movsd_r_label(a, X64_XMM9, label); x64_bind(a, label));
  ENC(x64, "movups xmm9, xmmword ptr [8]", "44 0f 10 0d 00 00 00 00",
    asm_label_t label = x64_new_label(a); x64_movups_r_label(a, X64_XMM9, label); x64_bind(a, label));
  ENC(x64, "call rax", "ff d0",
    x64_call_r(a, X64_RAX));
  ENC(x64, "call r11", "41 ff d3",
    x64_call_r(a, X64_R11));
  ENC(x64, "call qword ptr [rax+8]", "ff 50 08",
    x64_call_m(a, asm_addr(X64_RAX, 8)));
  ENC(x64, "jmp r12", "41 ff e4",
    x64_jmp_r(a, X64_R12));
  ENC(x64, "jmp qword ptr [rsp+8]", "ff 64 24 08",
    x64_jmp_m(a, asm_addr(X64_RSP, 8)));
  ENC(x64, "push rbx", "53",
    x64_push(a, X64_RBX));
  ENC(x64, "push r12", "41 54",
    x64_push(a, X64_R12));
  ENC(x64, "pop r15", "41 5f",
    x64_pop(a, X64_R15));
  ENC(x64, "pop rbp", "5d",
    x64_pop(a, X64_RBP));
  ENC(x64, "ret", "c3",
    x64_ret(a));
  ENC(x64, "int3", "cc",
    x64_int3(a));
  ENC(x64, "ud2", "0f 0b",
    x64_ud2(a));
}

static void x64_scalar_doubles_table(void) {
  ENC(x64, "movsd xmm0, xmm1", "f2 0f 10 c1",
    x64_movsd_rr(a, X64_XMM0, X64_XMM1));
  ENC(x64, "movapd xmm8, xmm15", "66 45 0f 28 c7",
    x64_movapd_rr(a, X64_XMM8, X64_XMM15));
  ENC(x64, "movsd xmm0, qword ptr [rax+8]", "f2 0f 10 40 08",
    x64_movsd_rm(a, X64_XMM0, asm_addr(X64_RAX, 8)));
  ENC(x64, "movsd xmm11, qword ptr [r12+r13*8]", "f2 47 0f 10 1c ec",
    x64_movsd_rm(a, X64_XMM11, asm_addr_indexed(X64_R12, X64_R13, ASM_SCALE_8, 0)));
  ENC(x64, "movsd qword ptr [rsp], xmm9", "f2 44 0f 11 0c 24",
    x64_movsd_mr(a, asm_addr(X64_RSP, 0), X64_XMM9));
  ENC(x64, "movups xmmword ptr [rdx+0x10], xmm15", "44 0f 11 7a 10",
    x64_movups_mr(a, asm_addr(X64_RDX, 16), X64_XMM15));
  ENC(x64, "movq xmm0, rax", "66 48 0f 6e c0",
    x64_movq_xr(a, X64_XMM0, X64_RAX));
  ENC(x64, "movq xmm12, r13", "66 4d 0f 6e e5",
    x64_movq_xr(a, X64_XMM12, X64_R13));
  ENC(x64, "movq rax, xmm1", "66 48 0f 7e c8",
    x64_movq_rx(a, X64_RAX, X64_XMM1));
  ENC(x64, "movq r10, xmm10", "66 4d 0f 7e d2",
    x64_movq_rx(a, X64_R10, X64_XMM10));
  ENC(x64, "addsd xmm0, xmm1", "f2 0f 58 c1",
    x64_addsd(a, X64_XMM0, X64_XMM1));
  ENC(x64, "subsd xmm2, xmm3", "f2 0f 5c d3",
    x64_subsd(a, X64_XMM2, X64_XMM3));
  ENC(x64, "mulsd xmm14, xmm4", "f2 44 0f 59 f4",
    x64_mulsd(a, X64_XMM14, X64_XMM4));
  ENC(x64, "divsd xmm9, xmm10", "f2 45 0f 5e ca",
    x64_divsd(a, X64_XMM9, X64_XMM10));
  ENC(x64, "sqrtsd xmm5, xmm13", "f2 41 0f 51 ed",
    x64_sqrtsd(a, X64_XMM5, X64_XMM13));
  ENC(x64, "roundsd xmm5, xmm13, 9", "66 41 0f 3a 0b ed 09",
    x64_roundsd(a, X64_XMM5, X64_XMM13, 9));
  ENC(x64, "roundsd xmm0, xmm1, 0xa", "66 0f 3a 0b c1 0a",
    x64_roundsd(a, X64_XMM0, X64_XMM1, 10));
  ENC(x64, "ucomisd xmm0, xmm1", "66 0f 2e c1",
    x64_ucomisd(a, X64_XMM0, X64_XMM1));
  ENC(x64, "ucomisd xmm15, xmm8", "66 45 0f 2e f8",
    x64_ucomisd(a, X64_XMM15, X64_XMM8));
  ENC(x64, "cvtsi2sd xmm0, eax", "f2 0f 2a c0",
    x64_cvtsi2sd(a, ASM_W32, X64_XMM0, X64_RAX));
  ENC(x64, "cvtsi2sd xmm1, r8", "f2 49 0f 2a c8",
    x64_cvtsi2sd(a, ASM_W64, X64_XMM1, X64_R8));
  ENC(x64, "cvttsd2si eax, xmm0", "f2 0f 2c c0",
    x64_cvttsd2si(a, ASM_W32, X64_RAX, X64_XMM0));
  ENC(x64, "cvttsd2si r9, xmm15", "f2 4d 0f 2c cf",
    x64_cvttsd2si(a, ASM_W64, X64_R9, X64_XMM15));
  ENC(x64, "xorpd xmm3, xmm3", "66 0f 57 db",
    x64_xorpd(a, X64_XMM3, X64_XMM3));
  ENC(x64, "andpd xmm6, xmm11", "66 41 0f 54 f3",
    x64_andpd(a, X64_XMM6, X64_XMM11));
  ENC(x64, "orpd xmm6, xmm11", "66 41 0f 56 f3",
    x64_orpd(a, X64_XMM6, X64_XMM11));
  ENC(x64, "minsd xmm0, xmm9", "f2 41 0f 5d c1",
    x64_minsd(a, X64_XMM0, X64_XMM9));
  ENC(x64, "maxsd xmm12, xmm1", "f2 44 0f 5f e1",
    x64_maxsd(a, X64_XMM12, X64_XMM1));
  ENC(x64, "movmskpd r11d, xmm7", "66 44 0f 50 df",
    x64_movmskpd(a, X64_R11, X64_XMM7));
  ENC(x64, "movss xmm0, dword ptr [rax+4]", "f3 0f 10 40 04",
    x64_movss_rm(a, X64_XMM0, asm_addr(X64_RAX, 4)));
  ENC(x64, "movss xmm9, dword ptr [r12+r13*4]", "f3 47 0f 10 0c ac",
    x64_movss_rm(a, X64_XMM9, asm_addr_indexed(X64_R12, X64_R13, ASM_SCALE_4, 0)));
  ENC(x64, "movss dword ptr [rsp], xmm15", "f3 44 0f 11 3c 24",
    x64_movss_mr(a, asm_addr(X64_RSP, 0), X64_XMM15));
  ENC(x64, "cvtss2sd xmm1, xmm2", "f3 0f 5a ca",
    x64_cvtss2sd(a, X64_XMM1, X64_XMM2));
  ENC(x64, "cvtsd2ss xmm15, xmm8", "f2 45 0f 5a f8",
    x64_cvtsd2ss(a, X64_XMM15, X64_XMM8));
}

static void x64_masm_operations_are_legalized_table(void) {
  MASM_ENC(x64, "add rax, rbx", 3, "48 01 d8",
    masm_x64_add64(m, X64_RAX, X64_RBX, X64_RAX));
  MASM_ENC(x64, "mov r11, rbx; sub r11, rax; mov rax, r11", 9, "49 89 db 49 29 c3 4c 89 d8",
    masm_x64_sub64(m, X64_RAX, X64_RBX, X64_RAX));
  MASM_ENC(x64, "mov ecx, edx; sub ecx, esi", 4, "89 d1 29 f1",
    masm_x64_sub32(m, X64_RCX, X64_RDX, X64_RSI));
  MASM_ENC(x64, "lea rax, [rbx+8]", 4, "48 8d 43 08",
    masm_x64_add64_imm(m, X64_RAX, X64_RBX, 8));
  MASM_ENC(x64, "add rax, qword ptr [8]", 7, "48 03 05 01 00 00 00 00 00 00 00 00 00 01 00 00",
    masm_x64_add64_imm(m, X64_RAX, X64_RAX, (UINT64_C(1) << 40)));
  MASM_ENC(x64, "mov eax, ebx", 2, "89 d8",
    masm_x64_and64_imm(m, X64_RAX, X64_RBX, 0xffffffff));
  MASM_ENC(x64, "and rax, qword ptr [8]", 7, "48 23 05 01 00 00 00 00 00 00 00 00 00 00 ff ff",
    masm_x64_and64_imm(m, X64_RAX, X64_RAX, 0xffff000000000000));
  MASM_ENC(x64, "mov rax, rbx; or rax, qword ptr [0x10]", 10, "48 89 d8 48 0b 05 06 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ff ff",
    masm_x64_or64_imm(m, X64_RAX, X64_RBX, 0xffff000000000000));
  MASM_ENC(x64, "mov rax, rbx; sub rax, qword ptr [0x10]", 10, "48 89 d8 48 2b 05 06 00 00 00 00 00 00 00 00 00 00 00 00 00 00 01 00 00",
    masm_x64_sub64_imm(m, X64_RAX, X64_RBX, (UINT64_C(1) << 40)));
  MASM_ENC(x64, "mov r11, rbx; mov r10, rcx; mov rcx, rdx; shl r11, cl; mov rcx, r10; mov rax, r11", 18, "49 89 db 49 89 ca 48 89 d1 49 d3 e3 4c 89 d1 4c 89 d8",
    masm_x64_shl64(m, X64_RAX, X64_RBX, X64_RDX));
  MASM_ENC(x64, "mov eax, ebx; shl eax, cl", 4, "89 d8 d3 e0",
    masm_x64_shl32(m, X64_RAX, X64_RBX, X64_RCX));
  MASM_ENC(x64, "mov eax, eax", 2, "89 c0",
    masm_x64_shl32_imm(m, X64_RAX, X64_RAX, 32));
  MASM_ENC(x64, "cmp rbx, rcx; setl al; movzx eax, al", 9, "48 39 cb 0f 9c c0 0f b6 c0",
    masm_x64_compare64_set(m, ASM_COND_LESS_THAN, X64_RAX, X64_RBX, X64_RCX));
  MASM_ENC(x64, "cmp rdi, rsi; mov rax, rcx; cmovl rax, rdx", 10, "48 39 f7 48 89 c8 48 0f 4c c2",
    masm_x64_select64(m, ASM_COND_LESS_THAN, X64_RDI, X64_RSI, X64_RAX, X64_RDX, X64_RCX));
  MASM_ENC(x64, "cmp rdi, rsi; cmovge rdx, rcx", 7, "48 39 f7 48 0f 4d d1",
    masm_x64_select64(m, ASM_COND_LESS_THAN, X64_RDI, X64_RSI, X64_RDX, X64_RDX, X64_RCX));
  MASM_ENC(x64, "call qword ptr [8]", 6, "ff 15 02 00 00 00 00 00 bc 9a 78 56 34 12 00 00",
    masm_x64_call_absolute(m, 0x123456789abc));
  MASM_ENC(x64, "mov r11, qword ptr [0x10]; jmp r11", 10, "4c 8b 1d 09 00 00 00 41 ff e3 00 00 00 00 00 00 bc 9a 78 56 34 12 00 00",
    masm_x64_jump_absolute(m, 0x123456789abc));
  MASM_ENC(x64, "mov r11, qword ptr [0x10]; cmp qword ptr [rdi+8], r11; je short 0", 13, "4c 8b 1d 09 00 00 00 4c 39 5f 08 74 f3 00 00 00 00 00 00 00 00 01 00 00",
    asm_label_t label = masm_x64_new_label(m); masm_x64_bind(m, label); masm_x64_branch64_memory_imm(m, ASM_COND_EQUAL, asm_addr(X64_RDI, 8), (UINT64_C(1) << 40), label));
  MASM_ENC(x64, "test rax, rax; je short 0", 5, "48 85 c0 74 fb",
    asm_label_t label = masm_x64_new_label(m); masm_x64_bind(m, label); masm_x64_branch64_imm(m, ASM_COND_EQUAL, X64_RAX, 0, label));
  MASM_ENC(x64, "ucomisd xmm0, xmm1; jp 0xc; je short 0", 12, "66 0f 2e c1 0f 8a 02 00 00 00 74 f4",
    asm_label_t label = masm_x64_new_label(m); masm_x64_bind(m, label); masm_x64_branch_double(m, ASM_DCOND_EQUAL, X64_XMM0, X64_XMM1, label));
  MASM_ENC(x64, "ucomisd xmm1, xmm0; ja short 0", 6, "66 0f 2e c8 77 fa",
    asm_label_t label = masm_x64_new_label(m); masm_x64_bind(m, label); masm_x64_branch_double(m, ASM_DCOND_LESS_THAN, X64_XMM0, X64_XMM1, label));
  MASM_ENC(x64, "ucomisd xmm0, xmm1; setne al; setp r11b; or eax, r11d; movzx eax, al", 17, "66 0f 2e c1 0f 95 c0 41 0f 9a c3 44 09 d8 0f b6 c0",
    masm_x64_compare_double_set(m, ASM_DCOND_NOT_EQUAL_OR_UNORDERED, X64_RAX, X64_XMM0, X64_XMM1));
  MASM_ENC(x64, "movapd xmm15, xmm1; subsd xmm15, xmm0; movapd xmm0, xmm15", 15, "66 44 0f 28 f9 f2 44 0f 5c f8 66 41 0f 28 c7",
    masm_x64_sub_double(m, X64_XMM0, X64_XMM1, X64_XMM0));
  MASM_ENC(x64, "movsd xmm15, qword ptr [0x18]; movapd xmm2, xmm3; xorpd xmm2, xmm15", 18, "f2 44 0f 10 3d 0f 00 00 00 66 0f 28 d3 66 41 0f 57 d7 00 00 00 00 00 00 00 00 00 00 00 00 00 80",
    masm_x64_neg_double(m, X64_XMM2, X64_XMM3));
  MASM_ENC(x64, "cvttsd2si eax, xmm0; xorpd xmm15, xmm15; cvtsi2sd xmm15, eax; ucomisd xmm15, xmm0; jne short 0; jp short 0; test eax, eax; jne 0x2d; movmskpd r11d, xmm0; test r11d, 1; jne short 0", 45, "f2 0f 2c c0 66 45 0f 57 ff f2 44 0f 2a f8 66 44 0f 2e f8 75 eb 7a e9 85 c0 0f 85 0e 00 00 00 66 44 0f 50 d8 41 f7 c3 01 00 00 00 75 d3",
    asm_label_t fail = masm_x64_new_label(m); masm_x64_bind(m, fail); masm_x64_branch_convert_double_to_int32(m, X64_RAX, X64_XMM0, fail, ASM_NEGATIVE_ZERO_FAIL));
}

static void x64_masm_frames_table(void) {
  MASM_ENC(x64, "push rbp; mov rbp, rsp; push rbx; push r12; sub rsp, 0x20; movsd qword ptr [rsp+0x18], xmm8; movsd xmm8, qword ptr [rsp+0x18]; add rsp, 0x20; pop r12; pop rbx; pop rbp", 33, "55 48 89 e5 53 41 54 48 83 ec 20 f2 44 0f 11 44 24 18 f2 44 0f 10 44 24 18 48 83 c4 20 41 5c 5b 5d",
    asm_frame_t frame = masm_x64_frame(ASM_REG(X64_RBX) | ASM_REG(X64_R12) | ASM_REG(X64_RBP) | ASM_REG(X64_RSP), ASM_REG(X64_XMM8), 20); masm_x64_emit_prologue(m, &frame); masm_x64_emit_epilogue(m, &frame));
  MASM_ENC(x64, "push rbp; mov rbp, rsp; pop rbp", 5, "55 48 89 e5 5d",
    asm_frame_t frame = masm_x64_frame(0, 0, 0); masm_x64_emit_prologue(m, &frame); masm_x64_emit_epilogue(m, &frame));
}

static void a64_move_wide_immediates_table(void) {
  ENC(a64, "mov x0, #0x1234", "80 46 82 d2",
    a64_movz(a, ASM_W64, A64_X0, 0x1234, 0));
  ENC(a64, "mov w1, #-0x10000", "e1 ff bf 52",
    a64_movz(a, ASM_W32, A64_X1, 0xffff, 16));
  ENC(a64, "mov x2, #0x123400000000", "82 46 c2 d2",
    a64_movz(a, ASM_W64, A64_X2, 0x1234, 32));
  ENC(a64, "mov x2, #-0x1", "02 00 80 92",
    a64_movn(a, ASM_W64, A64_X2, 0, 0));
  ENC(a64, "mov w3, #-0x1235", "83 46 82 12",
    a64_movn(a, ASM_W32, A64_X3, 0x1234, 0));
  ENC(a64, "movk x4, #0xbeef, lsl #0x30", "e4 dd f7 f2",
    a64_movk(a, ASM_W64, A64_X4, 0xbeef, 48));
  ENC(a64, "movk w5, #0x1, lsl #0x10", "25 00 a0 72",
    a64_movk(a, ASM_W32, A64_X5, 1, 16));
  ENC(a64, "mov x0, x1", "e0 03 01 aa",
    a64_mov(a, ASM_W64, A64_X0, A64_X1));
  ENC(a64, "mov w0, w30", "e0 03 1e 2a",
    a64_mov(a, ASM_W32, A64_X0, A64_X30));
  ENC(a64, "mov sp, x29", "bf 03 00 91",
    a64_mov_sp(a, A64_SP, A64_X29));
  ENC(a64, "mov x29, sp", "fd 03 00 91",
    a64_mov_sp(a, A64_X29, A64_SP));
  ENC(a64, "mov x0, #0x0", "00 00 80 d2",
    a64_mov_imm(a, ASM_W64, A64_X0, 0));
  ENC(a64, "mov x0, #-0x1", "00 00 80 92",
    a64_mov_imm(a, ASM_W64, A64_X0, UINT64_MAX));
  ENC(a64, "mov w0, #-0x1", "00 00 80 12",
    a64_mov_imm(a, ASM_W32, A64_X0, UINT64_MAX));
  ENC(a64, "mov x0, #0x9abc; movk x0, #0x5678, lsl #0x10; movk x0, #0x1234, lsl #0x20", "80 57 93 d2 00 cf aa f2 80 46 c2 f2",
    a64_mov_imm(a, ASM_W64, A64_X0, 0x123456789abc));
  ENC(a64, "mov x0, #-0x1235; movk x0, #0x0, lsl #0x30", "80 46 82 92 00 00 e0 f2",
    a64_mov_imm(a, ASM_W64, A64_X0, 0x0000ffffffffedcb));
  ENC(a64, "mov x7, #-0xff00ff00ff0100", "e7 9f 08 b2",
    a64_mov_imm(a, ASM_W64, A64_X7, 0xff00ff00ff00ff00));
}

static void a64_add_and_subtract_table(void) {
  ENC(a64, "add x0, x1, #0xfff", "20 fc 3f 91",
    a64_add_imm(a, ASM_W64, A64_X0, A64_X1, 4095, false));
  ENC(a64, "add x0, x1, #0x1, lsl #0xc", "20 04 40 91",
    a64_add_imm(a, ASM_W64, A64_X0, A64_X1, 1, true));
  ENC(a64, "add sp, sp, #0x10", "ff 43 00 91",
    a64_add_imm(a, ASM_W64, A64_SP, A64_SP, 16, false));
  ENC(a64, "sub w0, w1, #0x1", "20 04 00 51",
    a64_sub_imm(a, ASM_W32, A64_X0, A64_X1, 1, false));
  ENC(a64, "sub sp, sp, #0x20", "ff 83 00 d1",
    a64_sub_imm(a, ASM_W64, A64_SP, A64_SP, 32, false));
  ENC(a64, "adds x0, x1, #0x1", "20 04 00 b1",
    a64_adds_imm(a, ASM_W64, A64_X0, A64_X1, 1, false));
  ENC(a64, "subs w2, w3, #0x0", "62 00 00 71",
    a64_subs_imm(a, ASM_W32, A64_X2, A64_X3, 0, false));
  ENC(a64, "cmp x0, #0x2a", "1f a8 00 f1",
    a64_cmp_imm(a, ASM_W64, A64_X0, 42, false));
  ENC(a64, "cmp sp, #0x0", "ff 03 00 f1",
    a64_cmp_imm(a, ASM_W64, A64_SP, 0, false));
  ENC(a64, "cmn w5, #0x7", "bf 1c 00 31",
    a64_cmn_imm(a, ASM_W32, A64_X5, 7, false));
  ENC(a64, "add x0, x1, x2", "20 00 02 8b",
    a64_add(a, ASM_W64, A64_X0, A64_X1, A64_X2, A64_SHIFT_LSL, 0));
  ENC(a64, "add x0, x1, x2, lsl #0x3", "20 0c 02 8b",
    a64_add(a, ASM_W64, A64_X0, A64_X1, A64_X2, A64_SHIFT_LSL, 3));
  ENC(a64, "sub w0, w1, w2, asr #0x1f", "20 7c 82 4b",
    a64_sub(a, ASM_W32, A64_X0, A64_X1, A64_X2, A64_SHIFT_ASR, 31));
  ENC(a64, "adds x0, x1, x2", "20 00 02 ab",
    a64_adds(a, ASM_W64, A64_X0, A64_X1, A64_X2, A64_SHIFT_LSL, 0));
  ENC(a64, "subs x0, x1, x2, lsr #0x1", "20 04 42 eb",
    a64_subs(a, ASM_W64, A64_X0, A64_X1, A64_X2, A64_SHIFT_LSR, 1));
  ENC(a64, "cmp x0, x1", "1f 00 01 eb",
    a64_cmp(a, ASM_W64, A64_X0, A64_X1));
  ENC(a64, "cmn w0, w1", "1f 00 01 2b",
    a64_cmn(a, ASM_W32, A64_X0, A64_X1));
  ENC(a64, "neg x0, x1", "e0 03 01 cb",
    a64_neg(a, ASM_W64, A64_X0, A64_X1));
  ENC(a64, "negs w0, w1", "e0 03 01 6b",
    a64_negs(a, ASM_W32, A64_X0, A64_X1));
  ENC(a64, "add x16, sp, x16", "f0 63 30 8b",
    a64_add_extended(a, ASM_W64, A64_X16, A64_SP, A64_X16, A64_EXTEND_UXTX, 0));
  ENC(a64, "add x0, x1, w2, sxtw #0x2", "20 c8 22 8b",
    a64_add_extended(a, ASM_W64, A64_X0, A64_X1, A64_X2, A64_EXTEND_SXTW, 2));
  ENC(a64, "sub sp, sp, x17", "ff 63 31 cb",
    a64_sub_extended(a, ASM_W64, A64_SP, A64_SP, A64_X17, A64_EXTEND_UXTX, 0));
  ENC(a64, "cmp x17, w17, sxtw", "3f c2 31 eb",
    a64_subs_extended(a, ASM_W64, A64_XZR, A64_X17, A64_X17, A64_EXTEND_SXTW, 0));
}

static void a64_logical_operations_table(void) {
  ENC(a64, "and x1, x2, #0xff00", "41 1c 78 92",
    a64_and_imm(a, ASM_W64, A64_X1, A64_X2, 0xff00));
  ENC(a64, "orr w0, w1, #0x55555555", "20 f0 00 32",
    a64_orr_imm(a, ASM_W32, A64_X0, A64_X1, 0x55555555));
  ENC(a64, "eor x3, x4, #0x8000000000000000", "83 00 41 d2",
    a64_eor_imm(a, ASM_W64, A64_X3, A64_X4, (UINT64_C(1) << 63)));
  ENC(a64, "ands x0, x1, #0x1", "20 00 40 f2",
    a64_ands_imm(a, ASM_W64, A64_X0, A64_X1, 1));
  ENC(a64, "tst x0, #0xffff000000000000", "1f 3c 50 f2",
    a64_tst_imm(a, ASM_W64, A64_X0, 0xffff000000000000));
  ENC(a64, "tst w0, #0x7", "1f 08 00 72",
    a64_tst_imm(a, ASM_W32, A64_X0, 7));
  ENC(a64, "and sp, x0, #0xfffffffffffffff0", "1f ec 7c 92",
    a64_and_imm(a, ASM_W64, A64_SP, A64_X0, ~UINT64_C(0xf)));
  ENC(a64, "and x0, x1, x2", "20 00 02 8a",
    a64_and(a, ASM_W64, A64_X0, A64_X1, A64_X2, A64_SHIFT_LSL, 0));
  ENC(a64, "bic x0, x1, x2", "20 00 22 8a",
    a64_bic(a, ASM_W64, A64_X0, A64_X1, A64_X2, A64_SHIFT_LSL, 0));
  ENC(a64, "orr w0, w1, w2", "20 00 02 2a",
    a64_orr(a, ASM_W32, A64_X0, A64_X1, A64_X2, A64_SHIFT_LSL, 0));
  ENC(a64, "orn x0, x1, x2", "20 00 22 aa",
    a64_orn(a, ASM_W64, A64_X0, A64_X1, A64_X2, A64_SHIFT_LSL, 0));
  ENC(a64, "eor x0, x1, x2, ror #0x7", "20 1c c2 ca",
    a64_eor(a, ASM_W64, A64_X0, A64_X1, A64_X2, A64_SHIFT_ROR, 7));
  ENC(a64, "ands w0, w1, w2", "20 00 02 6a",
    a64_ands(a, ASM_W32, A64_X0, A64_X1, A64_X2, A64_SHIFT_LSL, 0));
  ENC(a64, "tst x0, x1", "1f 00 01 ea",
    a64_tst(a, ASM_W64, A64_X0, A64_X1));
  ENC(a64, "mvn x0, x1", "e0 03 21 aa",
    a64_mvn(a, ASM_W64, A64_X0, A64_X1));
}

static void a64_shifts_and_bitfields_table(void) {
  ENC(a64, "lsl x0, x1, #0x3", "20 f0 7d d3",
    a64_lsl_imm(a, ASM_W64, A64_X0, A64_X1, 3));
  ENC(a64, "lsl w0, w1, #0x1f", "20 00 01 53",
    a64_lsl_imm(a, ASM_W32, A64_X0, A64_X1, 31));
  ENC(a64, "lsr x0, x1, #0x3f", "20 fc 7f d3",
    a64_lsr_imm(a, ASM_W64, A64_X0, A64_X1, 63));
  ENC(a64, "asr w2, w3, #0x1", "62 7c 01 13",
    a64_asr_imm(a, ASM_W32, A64_X2, A64_X3, 1));
  ENC(a64, "lsl x0, x1, x2", "20 20 c2 9a",
    a64_lsl(a, ASM_W64, A64_X0, A64_X1, A64_X2));
  ENC(a64, "lsr w0, w1, w2", "20 24 c2 1a",
    a64_lsr(a, ASM_W32, A64_X0, A64_X1, A64_X2));
  ENC(a64, "asr x0, x1, x2", "20 28 c2 9a",
    a64_asr(a, ASM_W64, A64_X0, A64_X1, A64_X2));
  ENC(a64, "ror w0, w1, w2", "20 2c c2 1a",
    a64_ror(a, ASM_W32, A64_X0, A64_X1, A64_X2));
  ENC(a64, "sdiv w0, w1, w2", "20 0c c2 1a",
    a64_sdiv(a, ASM_W32, A64_X0, A64_X1, A64_X2));
  ENC(a64, "udiv x0, x1, x2", "20 08 c2 9a",
    a64_udiv(a, ASM_W64, A64_X0, A64_X1, A64_X2));
  ENC(a64, "sxtw x0, w1", "20 7c 40 93",
    a64_sxtw(a, A64_X0, A64_X1));
  ENC(a64, "sxtb x0, w1", "20 1c 40 93",
    a64_sxtb(a, ASM_W64, A64_X0, A64_X1));
  ENC(a64, "sxth w0, w1", "20 3c 00 13",
    a64_sxth(a, ASM_W32, A64_X0, A64_X1));
  ENC(a64, "uxtb w0, w1", "20 1c 00 53",
    a64_uxtb(a, A64_X0, A64_X1));
  ENC(a64, "uxth w0, w1", "20 3c 00 53",
    a64_uxth(a, A64_X0, A64_X1));
  ENC(a64, "ubfx x0, x1, #0x4, #0x8", "20 2c 44 d3",
    a64_ubfm(a, ASM_W64, A64_X0, A64_X1, 4, 11));
  ENC(a64, "sbfiz x0, x1, #0x4, #0x4", "20 0c 7c 93",
    a64_sbfm(a, ASM_W64, A64_X0, A64_X1, 60, 3));
}

static void a64_multiplication_table(void) {
  ENC(a64, "mul w0, w1, w2", "20 7c 02 1b",
    a64_mul(a, ASM_W32, A64_X0, A64_X1, A64_X2));
  ENC(a64, "mul x0, x1, x2", "20 7c 02 9b",
    a64_mul(a, ASM_W64, A64_X0, A64_X1, A64_X2));
  ENC(a64, "madd x0, x1, x2, x3", "20 0c 02 9b",
    a64_madd(a, ASM_W64, A64_X0, A64_X1, A64_X2, A64_X3));
  ENC(a64, "msub w0, w1, w2, w3", "20 8c 02 1b",
    a64_msub(a, ASM_W32, A64_X0, A64_X1, A64_X2, A64_X3));
  ENC(a64, "smull x0, w1, w2", "20 7c 22 9b",
    a64_smull(a, A64_X0, A64_X1, A64_X2));
  ENC(a64, "umull x0, w1, w2", "20 7c a2 9b",
    a64_umull(a, A64_X0, A64_X1, A64_X2));
  ENC(a64, "smulh x0, x1, x2", "20 7c 42 9b",
    a64_smulh(a, A64_X0, A64_X1, A64_X2));
  ENC(a64, "umulh x0, x1, x2", "20 7c c2 9b",
    a64_umulh(a, A64_X0, A64_X1, A64_X2));
  ENC(a64, "cmp x17, w17, sxtw", "3f c2 31 eb",
    a64_check_smull_overflow(a, A64_X17));
  ENC(a64, "cmp x1, x0, asr #0x3f", "3f fc 80 eb",
    a64_check_smulh_overflow(a, A64_X1, A64_X0));
}

static void a64_conditional_select_and_condition_codes_table(void) {
  ENC(a64, "csel x0, x1, x2, eq", "20 00 82 9a",
    a64_csel(a, ASM_W64, A64_X0, A64_X1, A64_X2, A64_COND_EQ));
  ENC(a64, "csinc w0, w1, w2, ne", "20 14 82 1a",
    a64_csinc(a, ASM_W32, A64_X0, A64_X1, A64_X2, A64_COND_NE));
  ENC(a64, "csinv x0, x1, x2, lt", "20 b0 82 da",
    a64_csinv(a, ASM_W64, A64_X0, A64_X1, A64_X2, A64_COND_LT));
  ENC(a64, "csneg x0, x1, x2, ge", "20 a4 82 da",
    a64_csneg(a, ASM_W64, A64_X0, A64_X1, A64_X2, A64_COND_GE));
  ENC(a64, "cset w0, lt", "e0 a7 9f 1a",
    a64_cset(a, ASM_W32, A64_X0, A64_COND_LT));
  ENC(a64, "csetm x0, hi", "e0 93 9f da",
    a64_csetm(a, ASM_W64, A64_X0, A64_COND_HI));
}

static void a64_loads_and_stores_table(void) {
  ENC(a64, "ldr x0, [x1, #0x8]", "20 04 40 f9",
    a64_ldr(a, ASM_W64, A64_X0, a64_mem_offset(A64_X1, 8)));
  ENC(a64, "ldr x0, [x1]", "20 00 40 f9",
    a64_ldr(a, ASM_W64, A64_X0, a64_mem_offset(A64_X1, 0)));
  ENC(a64, "ldr x0, [x1, #0x7ff8]", "20 fc 7f f9",
    a64_ldr(a, ASM_W64, A64_X0, a64_mem_offset(A64_X1, 32760)));
  ENC(a64, "ldur x0, [sp, #-0x8]", "e0 83 5f f8",
    a64_ldr(a, ASM_W64, A64_X0, a64_mem_offset(A64_SP, -8)));
  ENC(a64, "ldur x0, [x1, #0x4]", "20 40 40 f8",
    a64_ldr(a, ASM_W64, A64_X0, a64_mem_offset(A64_X1, 4)));
  ENC(a64, "ldur x0, [x1, #0xff]", "20 f0 4f f8",
    a64_ldr(a, ASM_W64, A64_X0, a64_mem_offset(A64_X1, 255)));
  ENC(a64, "ldur x0, [x1, #-0x100]", "20 00 50 f8",
    a64_ldr(a, ASM_W64, A64_X0, a64_mem_offset(A64_X1, -256)));
  ENC(a64, "ldr w0, [x1, #0x3ffc]", "20 fc 7f b9",
    a64_ldr(a, ASM_W32, A64_X0, a64_mem_offset(A64_X1, 16380)));
  ENC(a64, "str xzr, [sp, #0x10]", "ff 0b 00 f9",
    a64_str(a, ASM_W64, A64_XZR, a64_mem_offset(A64_SP, 16)));
  ENC(a64, "str w3, [x28, #0x4]", "83 07 00 b9",
    a64_str(a, ASM_W32, A64_X3, a64_mem_offset(A64_X28, 4)));
  ENC(a64, "ldrb w0, [x1, #0xfff]", "20 fc 7f 39",
    a64_ldrb(a, A64_X0, a64_mem_offset(A64_X1, 4095)));
  ENC(a64, "ldrh w0, [x1, #0x2]", "20 04 40 79",
    a64_ldrh(a, A64_X0, a64_mem_offset(A64_X1, 2)));
  ENC(a64, "ldurh w0, [x1, #0x1]", "20 10 40 78",
    a64_ldrh(a, A64_X0, a64_mem_offset(A64_X1, 1)));
  ENC(a64, "ldursb x0, [x1, #-0x1]", "20 f0 9f 38",
    a64_ldrsb(a, ASM_W64, A64_X0, a64_mem_offset(A64_X1, -1)));
  ENC(a64, "ldrsb w0, [x1]", "20 00 c0 39",
    a64_ldrsb(a, ASM_W32, A64_X0, a64_mem_offset(A64_X1, 0)));
  ENC(a64, "ldrsh x0, [x1, #0x2]", "20 04 80 79",
    a64_ldrsh(a, ASM_W64, A64_X0, a64_mem_offset(A64_X1, 2)));
  ENC(a64, "ldrsh w0, [x1, #0x2]", "20 04 c0 79",
    a64_ldrsh(a, ASM_W32, A64_X0, a64_mem_offset(A64_X1, 2)));
  ENC(a64, "ldrsw x0, [x1, #0x4]", "20 04 80 b9",
    a64_ldrsw(a, A64_X0, a64_mem_offset(A64_X1, 4)));
  ENC(a64, "strb w0, [x1, #0x1]", "20 04 00 39",
    a64_strb(a, A64_X0, a64_mem_offset(A64_X1, 1)));
  ENC(a64, "sturh w0, [x1, #-0x2]", "20 e0 1f 78",
    a64_strh(a, A64_X0, a64_mem_offset(A64_X1, -2)));
  ENC(a64, "ldr d0, [x1, #0x8]", "20 04 40 fd",
    a64_ldr_d(a, A64_D0, a64_mem_offset(A64_X1, 8)));
  ENC(a64, "str d31, [sp]", "ff 03 00 fd",
    a64_str_d(a, A64_D31, a64_mem_offset(A64_SP, 0)));
  ENC(a64, "stur d8, [x29, #-0x8]", "a8 83 1f fc",
    a64_str_d(a, A64_D8, a64_mem_offset(A64_X29, -8)));
  ENC(a64, "ldr x0, [x1, x2, lsl #0x3]", "20 78 62 f8",
    a64_ldr(a, ASM_W64, A64_X0, a64_mem_register(A64_X1, A64_X2, A64_EXTEND_UXTX, true)));
  ENC(a64, "ldr x0, [sp, x16]", "e0 6b 70 f8",
    a64_ldr(a, ASM_W64, A64_X0, a64_mem_register(A64_SP, A64_X16, A64_EXTEND_UXTX, false)));
  ENC(a64, "ldr w0, [x1, w2, sxtw #0x2]", "20 d8 62 b8",
    a64_ldr(a, ASM_W32, A64_X0, a64_mem_register(A64_X1, A64_X2, A64_EXTEND_SXTW, true)));
  ENC(a64, "ldrb w0, [x1, w2, uxtw]", "20 48 62 38",
    a64_ldrb(a, A64_X0, a64_mem_register(A64_X1, A64_X2, A64_EXTEND_UXTW, false)));
  ENC(a64, "strh w0, [x1, x2, lsl #0x1]", "20 78 22 78",
    a64_strh(a, A64_X0, a64_mem_register(A64_X1, A64_X2, A64_EXTEND_UXTX, true)));
  ENC(a64, "str d1, [x1, x2, lsl #0x3]", "21 78 22 fc",
    a64_str_d(a, A64_D1, a64_mem_register(A64_X1, A64_X2, A64_EXTEND_UXTX, true)));
  ENC(a64, "str x0, [sp, #-0x10]!", "e0 0f 1f f8",
    a64_str(a, ASM_W64, A64_X0, a64_mem_pre_index(A64_SP, -16)));
  ENC(a64, "ldr x0, [sp], #0x10", "e0 07 41 f8",
    a64_ldr(a, ASM_W64, A64_X0, a64_mem_post_index(A64_SP, 16)));
  ENC(a64, "str d8, [sp, #-0x10]!", "e8 0f 1f fc",
    a64_str_d(a, A64_D8, a64_mem_pre_index(A64_SP, -16)));
  ENC(a64, "ldr d8, [sp], #0x10", "e8 07 41 fc",
    a64_ldr_d(a, A64_D8, a64_mem_post_index(A64_SP, 16)));
  ENC(a64, "ldr s0, [x1, #0x4]", "20 04 40 bd",
    a64_load_store(a, A64_MEM_LDR_S, 0, a64_mem_offset(A64_X1, 4)));
  ENC(a64, "str s31, [x1, x2, lsl #0x2]", "3f 78 22 bc",
    a64_load_store(a, A64_MEM_STR_S, 31, a64_mem_register(A64_X1, A64_X2, A64_EXTEND_UXTX, true)));
  ENC(a64, "str q0, [x1, #0x10]", "20 04 80 3d",
    a64_load_store(a, A64_MEM_STR_Q, 0, a64_mem_offset(A64_X1, 16)));
  ENC(a64, "stur q31, [x1, #0x8]", "3f 80 80 3c",
    a64_load_store(a, A64_MEM_STR_Q, 31, a64_mem_offset(A64_X1, 8)));
  ENC(a64, "ldr q2, [x3, #0xfff0]", "62 fc ff 3d",
    a64_load_store(a, A64_MEM_LDR_Q, 2, a64_mem_offset(A64_X3, 0xfff0)));
  ENC(a64, "stp x29, x30, [sp, #-0x10]!", "fd 7b bf a9",
    a64_stp(a, A64_FP, A64_LR, a64_pair_pre_index(A64_SP, -16)));
  ENC(a64, "ldp x29, x30, [sp], #0x10", "fd 7b c1 a8",
    a64_ldp(a, A64_FP, A64_LR, a64_pair_post_index(A64_SP, 16)));
  ENC(a64, "stp x19, x20, [sp, #0x10]", "f3 53 01 a9",
    a64_stp(a, A64_X19, A64_X20, a64_pair_offset(A64_SP, 16)));
  ENC(a64, "ldp x0, x1, [x2, #-0x200]", "40 04 60 a9",
    a64_ldp(a, A64_X0, A64_X1, a64_pair_offset(A64_X2, -512)));
  ENC(a64, "stp d8, d9, [sp, #-0x10]!", "e8 27 bf 6d",
    a64_stp_d(a, A64_D8, A64_D9, a64_pair_pre_index(A64_SP, -16)));
  ENC(a64, "ldp d8, d9, [sp, #0x1f8]", "e8 a7 5f 6d",
    a64_ldp_d(a, A64_D8, A64_D9, a64_pair_offset(A64_SP, 504)));
}

static void a64_branches_and_misc_table(void) {
  ENC(a64, "b 0x4", "01 00 00 14",
    asm_label_t label = a64_new_label(a); a64_b(a, label); a64_bind(a, label));
  ENC(a64, "bl 0x4", "01 00 00 94",
    asm_label_t label = a64_new_label(a); a64_bl(a, label); a64_bind(a, label));
  ENC(a64, "nop; b.eq 0x0", "1f 20 03 d5 e0 ff ff 54",
    asm_label_t label = a64_new_label(a); a64_bind(a, label); a64_nop(a); a64_b_cond(a, A64_COND_EQ, label));
  ENC(a64, "cbz x3, 0x0", "03 00 00 b4",
    asm_label_t label = a64_new_label(a); a64_bind(a, label); a64_cbz(a, ASM_W64, A64_X3, label));
  ENC(a64, "cbnz w3, 0x8; nop", "43 00 00 35 1f 20 03 d5",
    asm_label_t label = a64_new_label(a); a64_cbnz(a, ASM_W32, A64_X3, label); a64_nop(a); a64_bind(a, label));
  ENC(a64, "tbz x3, #0x28, 0x4", "23 00 40 b6",
    asm_label_t label = a64_new_label(a); a64_tbz(a, A64_X3, 40, label); a64_bind(a, label));
  ENC(a64, "tbnz w3, #0x3, 0x0", "03 00 18 37",
    asm_label_t label = a64_new_label(a); a64_bind(a, label); a64_tbnz(a, A64_X3, 3, label));
  ENC(a64, "adr x0, 0x4", "20 00 00 10",
    asm_label_t label = a64_new_label(a); a64_adr(a, A64_X0, label); a64_bind(a, label));
  ENC(a64, "nop; adr x5, 0x0", "1f 20 03 d5 e5 ff ff 10",
    asm_label_t label = a64_new_label(a); a64_bind(a, label); a64_nop(a); a64_adr(a, A64_X5, label));
  ENC(a64, "br x16", "00 02 1f d6",
    a64_br(a, A64_X16));
  ENC(a64, "blr x17", "20 02 3f d6",
    a64_blr(a, A64_X17));
  ENC(a64, "ret", "c0 03 5f d6",
    a64_ret(a));
  ENC(a64, "nop", "1f 20 03 d5",
    a64_nop(a));
  ENC(a64, "brk #0x0", "00 00 20 d4",
    a64_brk(a, 0));
  ENC(a64, "brk #0xf000", "00 00 3e d4",
    a64_brk(a, 0xf000));
  ENC(a64, "udf #0x0", "00 00 00 00",
    a64_udf(a, 0));
}

static void a64_scalar_doubles_table(void) {
  ENC(a64, "fmov d0, d1", "20 40 60 1e",
    a64_fmov(a, A64_D0, A64_D1));
  ENC(a64, "fmov d0, x1", "20 00 67 9e",
    a64_fmov_from_gpr(a, A64_D0, A64_X1));
  ENC(a64, "fmov d0, xzr", "e0 03 67 9e",
    a64_fmov_from_gpr(a, A64_D0, A64_XZR));
  ENC(a64, "fmov x0, d1", "20 00 66 9e",
    a64_fmov_to_gpr(a, A64_X0, A64_D1));
  ENC(a64, "fmov d0, #1.5", "00 10 6f 1e",
    a64_fmov_imm(a, A64_D0, 1.5));
  ENC(a64, "fmov d1, #-2.0", "01 10 70 1e",
    a64_fmov_imm(a, A64_D1, -2.0));
  ENC(a64, "fmov d2, #0.125", "02 10 68 1e",
    a64_fmov_imm(a, A64_D2, 0.125));
  ENC(a64, "fmov d3, #31.0", "03 f0 67 1e",
    a64_fmov_imm(a, A64_D3, 31.0));
  ENC(a64, "fadd d0, d1, d2", "20 28 62 1e",
    a64_fadd(a, A64_D0, A64_D1, A64_D2));
  ENC(a64, "fsub d3, d4, d5", "83 38 65 1e",
    a64_fsub(a, A64_D3, A64_D4, A64_D5));
  ENC(a64, "fmul d29, d30, d31", "dd 0b 7f 1e",
    a64_fmul(a, A64_D29, A64_D30, A64_D31));
  ENC(a64, "fdiv d16, d17, d8", "30 1a 68 1e",
    a64_fdiv(a, A64_D16, A64_D17, A64_D8));
  ENC(a64, "fmin d0, d1, d2", "20 58 62 1e",
    a64_fmin(a, A64_D0, A64_D1, A64_D2));
  ENC(a64, "fmax d29, d30, d31", "dd 4b 7f 1e",
    a64_fmax(a, A64_D29, A64_D30, A64_D31));
  ENC(a64, "fsqrt d0, d1", "20 c0 61 1e",
    a64_fsqrt(a, A64_D0, A64_D1));
  ENC(a64, "frintm d0, d1", "20 40 65 1e",
    a64_frintm(a, A64_D0, A64_D1));
  ENC(a64, "frintp d30, d2", "5e c0 64 1e",
    a64_frintp(a, A64_D30, A64_D2));
  ENC(a64, "fneg d0, d1", "20 40 61 1e",
    a64_fneg(a, A64_D0, A64_D1));
  ENC(a64, "fabs d0, d1", "20 c0 60 1e",
    a64_fabs(a, A64_D0, A64_D1));
  ENC(a64, "fcmp d0, d1", "00 20 61 1e",
    a64_fcmp(a, A64_D0, A64_D1));
  ENC(a64, "fcmp d7, #0.0", "e8 20 60 1e",
    a64_fcmp_zero(a, A64_D7));
  ENC(a64, "scvtf d0, w1", "20 00 62 1e",
    a64_scvtf(a, ASM_W32, A64_D0, A64_X1));
  ENC(a64, "scvtf d0, x1", "20 00 62 9e",
    a64_scvtf(a, ASM_W64, A64_D0, A64_X1));
  ENC(a64, "fcvtzs w0, d1", "20 00 78 1e",
    a64_fcvtzs(a, ASM_W32, A64_X0, A64_D1));
  ENC(a64, "fcvtzs x0, d1", "20 00 78 9e",
    a64_fcvtzs(a, ASM_W64, A64_X0, A64_D1));
  ENC(a64, "fjcvtzs w0, d1", "20 00 7e 1e",
    a64_fjcvtzs(a, A64_X0, A64_D1));
  ENC(a64, "fcvt d0, s1", "20 c0 22 1e",
    a64_fcvt_double_from_single(a, A64_D0, A64_D1));
  ENC(a64, "fcvt s31, d8", "1f 41 62 1e",
    a64_fcvt_single_from_double(a, A64_D31, A64_D8));
}

static void a64_masm_addresses_are_legalized_table(void) {
  MASM_ENC(a64, "ldr x0, [x1, #0x8]", 4, "20 04 40 f9",
    masm_a64_load64(m, A64_X0, asm_addr(A64_X1, 8)));
  MASM_ENC(a64, "ldur x0, [x1, #-0x8]", 4, "20 80 5f f8",
    masm_a64_load64(m, A64_X0, asm_addr(A64_X1, -8)));
  MASM_ENC(a64, "mov x16, #0x10000; ldr x0, [x1, x16]", 8, "30 00 a0 d2 20 68 70 f8",
    masm_a64_load64(m, A64_X0, asm_addr(A64_X1, 0x10000)));
  MASM_ENC(a64, "mov x16, #0x8000; ldr d0, [sp, x16]", 8, "10 00 90 d2 e0 6b 70 fc",
    masm_a64_load_double(m, A64_D0, asm_addr(A64_SP, 32768)));
  MASM_ENC(a64, "ldr x0, [x1, x2, lsl #0x3]", 4, "20 78 62 f8",
    masm_a64_load64(m, A64_X0, asm_addr_indexed(A64_X1, A64_X2, ASM_SCALE_8, 0)));
  MASM_ENC(a64, "ldrb w0, [x1, x2]", 4, "20 68 62 38",
    masm_a64_load8(m, A64_X0, asm_addr_indexed(A64_X1, A64_X2, ASM_SCALE_1, 0)));
  MASM_ENC(a64, "add x16, x1, x2, uxtx #0x3; ldr w0, [x16]", 8, "30 6c 22 8b 00 02 40 b9",
    masm_a64_load32(m, A64_X0, asm_addr_indexed(A64_X1, A64_X2, ASM_SCALE_8, 0)));
  MASM_ENC(a64, "mov x16, #0x10; add x16, sp, x16; ldr x0, [x16, x2, lsl #0x3]", 12, "10 02 80 d2 f0 63 30 8b 00 7a 62 f8",
    masm_a64_load64(m, A64_X0, asm_addr_indexed(A64_SP, A64_X2, ASM_SCALE_8, 16)));
  MASM_ENC(a64, "mov x16, #0x3; add x16, x1, x16, uxtx; add x16, x16, x2, lsl #0x2; ldrb w0, [x16]", 16, "70 00 80 d2 30 60 30 8b 10 0a 02 8b 00 02 40 39",
    masm_a64_load8(m, A64_X0, asm_addr_indexed(A64_X1, A64_X2, ASM_SCALE_4, 3)));
  MASM_ENC(a64, "ldrsh x3, [x4, #0x2]", 4, "83 04 80 79",
    masm_a64_load16_sign_extend(m, A64_X3, asm_addr(A64_X4, 2)));
  MASM_ENC(a64, "ldrsw x3, [x4, #0x4]", 4, "83 04 80 b9",
    masm_a64_load32_sign_extend(m, A64_X3, asm_addr(A64_X4, 4)));
  MASM_ENC(a64, "str xzr, [sp, #0x8]", 4, "ff 07 00 f9",
    masm_a64_store_imm64(m, asm_addr(A64_SP, 8), 0));
  MASM_ENC(a64, "mov w17, #0x5; str w17, [x0]", 8, "b1 00 80 52 11 00 00 b9",
    masm_a64_store_imm32(m, asm_addr(A64_X0, 0), 5));
  MASM_ENC(a64, "strh w5, [x6, #0x6]", 4, "c5 0c 00 79",
    masm_a64_store16(m, asm_addr(A64_X6, 6), A64_X5));
  MASM_ENC(a64, "add x0, sp, x1, lsl #0x3; add x0, x0, #0x18", 8, "e0 6f 21 8b 00 60 00 91",
    masm_a64_load_effective_address(m, A64_X0, asm_addr_indexed(A64_SP, A64_X1, ASM_SCALE_8, 24)));
  MASM_ENC(a64, "mov x0, sp", 4, "e0 03 00 91",
    masm_a64_move64(m, A64_X0, A64_SP));
}

static void a64_masm_operations_are_legalized_table(void) {
  MASM_ENC(a64, "sub x0, x1, #0x5", 4, "20 14 00 d1",
    masm_a64_add64_imm(m, A64_X0, A64_X1, -5));
  MASM_ENC(a64, "mov x17, #0x3456; movk x17, #0x12, lsl #0x10; add x0, x1, x17", 12, "d1 8a 86 d2 51 02 a0 f2 20 00 11 8b",
    masm_a64_add64_imm(m, A64_X0, A64_X1, 0x123456));
  MASM_ENC(a64, "add sp, sp, #0x10, lsl #0xc", 4, "ff 43 40 91",
    masm_a64_add64_imm(m, A64_SP, A64_SP, 0x10000));
  MASM_ENC(a64, "mov x17, #0x3456; movk x17, #0x12, lsl #0x10; sub sp, sp, x17", 12, "d1 8a 86 d2 51 02 a0 f2 ff 63 31 cb",
    masm_a64_sub64_imm(m, A64_SP, A64_SP, 0x123456));
  MASM_ENC(a64, "and x0, x1, #0xff", 4, "20 1c 40 92",
    masm_a64_and64_imm(m, A64_X0, A64_X1, 0xff));
  MASM_ENC(a64, "mov x17, #0x1234; and x0, x1, x17", 8, "91 46 82 d2 20 00 11 8a",
    masm_a64_and64_imm(m, A64_X0, A64_X1, 0x1234));
  MASM_ENC(a64, "mov x0, x1", 4, "e0 03 01 aa",
    masm_a64_or64_imm(m, A64_X0, A64_X1, 0));
  MASM_ENC(a64, "mov w0, w0", 4, "e0 03 00 2a",
    masm_a64_and32_imm(m, A64_X0, A64_X0, 0xffffffff));
  MASM_ENC(a64, "mvn x0, x1", 4, "e0 03 21 aa",
    masm_a64_xor64_imm(m, A64_X0, A64_X1, UINT64_MAX));
  MASM_ENC(a64, "mov w0, w0", 4, "e0 03 00 2a",
    masm_a64_shl32_imm(m, A64_X0, A64_X0, 32));
  MASM_ENC(a64, "asr x0, x1, #0x1", 4, "20 fc 41 93",
    masm_a64_sar64_imm(m, A64_X0, A64_X1, 65));
  MASM_ENC(a64, "cmp w1, #0x1, lsl #0xc; cset w0, hi", 8, "3f 04 40 71 e0 97 9f 1a",
    masm_a64_compare32_imm_set(m, ASM_COND_ABOVE, A64_X0, A64_X1, 4096));
  MASM_ENC(a64, "cmp x0, x1; csel x2, x3, x4, lt", 8, "1f 00 01 eb 62 b0 84 9a",
    masm_a64_select64(m, ASM_COND_LESS_THAN, A64_X0, A64_X1, A64_X2, A64_X3, A64_X4));
  MASM_ENC(a64, "ldr x16, 0x8; blr x16", 8, "50 00 00 58 00 02 3f d6 bc 9a 78 56 34 12 00 00",
    masm_a64_call_absolute(m, 0x123456789abc));
  MASM_ENC(a64, "fmov d0, #1.0", 4, "00 10 6e 1e",
    masm_a64_move_double_imm(m, A64_D0, 1.0));
  MASM_ENC(a64, "fmov d0, xzr", 4, "e0 03 67 9e",
    masm_a64_move_double_imm(m, A64_D0, 0.0));
  MASM_ENC(a64, "ldr d0, 0x8", 4, "40 00 00 5c 00 00 00 00 00 00 00 00 00 00 00 80",
    masm_a64_move_double_imm(m, A64_D0, -0.0));
  MASM_ENC(a64, "fcmp d0, d1; cset w0, mi; csinc w0, w0, wzr, le", 12, "00 20 61 1e e0 57 9f 1a 00 d4 9f 1a",
    masm_a64_compare_double_set(m, ASM_DCOND_NOT_EQUAL, A64_X0, A64_D0, A64_D1));
}

static void a64_masm_branches_table(void) {
  MASM_ENC(a64, "cbz w0, 0x0", 4, "00 00 00 34",
    asm_label_t l = masm_a64_new_label(m); masm_a64_bind(m, l); masm_a64_branch32_imm(m, ASM_COND_EQUAL, A64_X0, 0, l));
  MASM_ENC(a64, "cmn x0, #0x1; b.lt 0x0", 8, "1f 04 00 b1 eb ff ff 54",
    asm_label_t l = masm_a64_new_label(m); masm_a64_bind(m, l); masm_a64_branch64_imm(m, ASM_COND_LESS_THAN, A64_X0, -1, l));
  MASM_ENC(a64, "mov x17, #0x10000000000; cmp x0, x17; b.eq 0x0", 12, "11 20 c0 d2 1f 00 11 eb c0 ff ff 54",
    asm_label_t l = masm_a64_new_label(m); masm_a64_bind(m, l); masm_a64_branch64_imm(m, ASM_COND_EQUAL, A64_X0, (UINT64_C(1) << 40), l));
  MASM_ENC(a64, "tbnz x0, #0x30, 0x0", 4, "00 00 80 b7",
    asm_label_t l = masm_a64_new_label(m); masm_a64_bind(m, l); masm_a64_branch_test64(m, ASM_COND_NON_ZERO, A64_X0, (UINT64_C(1) << 48), l));
  MASM_ENC(a64, "tst x0, #0xffff000000000000; b.eq 0x0", 8, "1f 3c 50 f2 e0 ff ff 54",
    asm_label_t l = masm_a64_new_label(m); masm_a64_bind(m, l); masm_a64_branch_test64(m, ASM_COND_ZERO, A64_X0, 0xffff000000000000, l));
  MASM_ENC(a64, "tst w0, #0x80000000; b.mi 0x0", 8, "1f 00 01 72 e4 ff ff 54",
    asm_label_t l = masm_a64_new_label(m); masm_a64_bind(m, l); masm_a64_branch_test32(m, ASM_COND_NEGATIVE, A64_X0, 0x80000000, l));
  MASM_ENC(a64, "mov x17, #0x1234; tst x0, x17; b.eq 0x0", 12, "91 46 82 d2 1f 00 11 ea c0 ff ff 54",
    asm_label_t l = masm_a64_new_label(m); masm_a64_bind(m, l); masm_a64_branch_test64(m, ASM_COND_ZERO, A64_X0, 0x1234, l));
  MASM_ENC(a64, "ldr x16, [x1, #0x8]; cmp x16, #0x7; b.ne 0x0", 12, "30 04 40 f9 1f 1e 00 f1 c1 ff ff 54",
    asm_label_t l = masm_a64_new_label(m); masm_a64_bind(m, l); masm_a64_branch64_memory_imm(m, ASM_COND_NOT_EQUAL, asm_addr(A64_X1, 8), 7, l));
  MASM_ENC(a64, "ldr w16, [x1]; cmn w16, #0x2; b.ge 0x0", 12, "30 00 40 b9 1f 0a 00 31 ca ff ff 54",
    asm_label_t l = masm_a64_new_label(m); masm_a64_bind(m, l); masm_a64_branch32_memory_imm(m, ASM_COND_GREATER_THAN_OR_EQUAL, asm_addr(A64_X1, 0), -2, l));
  MASM_ENC(a64, "smull x17, w1, w2; cmp x17, w17, sxtw; b.ne 0x0; mov w0, w17", 16, "31 7c 22 9b 3f c2 31 eb c1 ff ff 54 e0 03 11 2a",
    asm_label_t l = masm_a64_new_label(m); masm_a64_bind(m, l); masm_a64_branch_mul32_overflow(m, A64_X0, A64_X1, A64_X2, l));
  MASM_ENC(a64, "sdiv w3, w1, w2; msub w0, w3, w2, w1", 8, "23 0c c2 1a 60 84 02 1b",
    masm_a64_remainder32(m, A64_X0, A64_X1, A64_X2, A64_X3));
  MASM_ENC(a64, "subs w0, w0, #0x1; b.vs 0x0", 8, "00 04 00 71 e6 ff ff 54",
    asm_label_t l = masm_a64_new_label(m); masm_a64_bind(m, l); masm_a64_branch_add32_imm_overflow(m, A64_X0, A64_X0, -1, l));
  MASM_ENC(a64, "negs w3, w4; b.vs 0x0", 8, "e3 03 04 6b e6 ff ff 54",
    asm_label_t l = masm_a64_new_label(m); masm_a64_bind(m, l); masm_a64_branch_neg32_overflow(m, A64_X3, A64_X4, l));
  MASM_ENC(a64, "fcmp d0, d1; b.eq 0x0; b.vs 0x0", 12, "00 20 61 1e e0 ff ff 54 c6 ff ff 54",
    asm_label_t l = masm_a64_new_label(m); masm_a64_bind(m, l); masm_a64_branch_double(m, ASM_DCOND_EQUAL_OR_UNORDERED, A64_D0, A64_D1, l));
  MASM_ENC(a64, "fcmp d0, d1; b.mi 0x0", 8, "00 20 61 1e e4 ff ff 54",
    asm_label_t l = masm_a64_new_label(m); masm_a64_bind(m, l); masm_a64_branch_double(m, ASM_DCOND_LESS_THAN, A64_D0, A64_D1, l));
  MASM_ENC(a64, "fcvtzs w0, d1; scvtf d31, w0; fcmp d31, d1; b.ne 0x0; cbnz w0, 0x1c; fmov x17, d1; tbnz x17, #0x3f, 0x0", 28, "20 00 78 1e 1f 00 62 1e e0 23 61 1e a1 ff ff 54 60 00 00 35 31 00 66 9e 51 ff ff b7",
    { asm_label_t l = masm_a64_new_label(m); masm_a64_bind(m, l); masm_a64_branch_convert_double_to_int32(m, A64_X0, A64_D1, l, ASM_NEGATIVE_ZERO_FAIL); });
}

static void a64_masm_frames_table(void) {
  MASM_ENC(a64, "stp x29, x30, [sp, #-0x10]!; mov x29, sp; stp x19, x20, [sp, #-0x10]!; str x21, [sp, #-0x10]!; str d8, [sp, #-0x10]!; sub sp, sp, #0x20; add sp, sp, #0x20; ldr d8, [sp], #0x10; ldr x21, [sp], #0x10; ldp x19, x20, [sp], #0x10; ldp x29, x30, [sp], #0x10", 44, "fd 7b bf a9 fd 03 00 91 f3 53 bf a9 f5 0f 1f f8 e8 0f 1f fc ff 83 00 d1 ff 83 00 91 e8 07 41 fc f5 07 41 f8 f3 53 c1 a8 fd 7b c1 a8",
    asm_frame_t frame = masm_a64_frame(ASM_REG(A64_X19) | ASM_REG(A64_X20) | ASM_REG(A64_X21) | ASM_REG(A64_FP) | ASM_REG(A64_LR) | ASM_REG(A64_SP), ASM_REG(A64_D8), 20); masm_a64_emit_prologue(m, &frame); masm_a64_emit_epilogue(m, &frame));
  MASM_ENC(a64, "stp x29, x30, [sp, #-0x10]!; mov x29, sp; mov x17, #0x10; movk x17, #0x1, lsl #0x10; sub sp, sp, x17; mov x17, #0x10; movk x17, #0x1, lsl #0x10; add sp, sp, x17; ldp x29, x30, [sp], #0x10", 36, "fd 7b bf a9 fd 03 00 91 11 02 80 d2 31 00 a0 f2 ff 63 31 cb 11 02 80 d2 31 00 a0 f2 ff 63 31 8b fd 7b c1 a8",
    asm_frame_t frame = masm_a64_frame(0, 0, 0x10008); masm_a64_emit_prologue(m, &frame); masm_a64_emit_epilogue(m, &frame));
}
static size_t x64_length_of_mov_ri(uint64_t imm) {
  x64_asm_t a;
  x64_init(&a);
  x64_mov_ri(&a, X64_RAX, imm);
  asm_code_t code = finish_x64(&a);
  size_t len = code.len;
  asm_code_free(&code);
  return len;
}

static void x64_immediate_moves_pick_the_shortest_form(void) {
  x64_immediate_moves_pick_the_shortest_form_table();
  CHECK(x64_length_of_mov_ri(1) == 5, "mov_ri(1)");
  CHECK(x64_length_of_mov_ri(UINT64_MAX) == 7, "mov_ri(MAX)");
  CHECK(x64_length_of_mov_ri(UINT64_C(1) << 32) == 10, "mov_ri(1 << 32)");
}

static void x64_check_bytes(asm_address_t address, const uint8_t *expected, size_t n) {
  x64_asm_t a;
  x64_init(&a);
  x64_mov_rm(&a, ASM_W64, X64_RAX, address);
  asm_code_t code = finish_x64(&a);
  CHECK(code.len == n && bytes_equal(&code, 0, expected, n), "base %u disp %d", address.base, address.disp);
  asm_code_free(&code);
}

static void x64_memory_operands(void) {
  x64_memory_operands_table();
  // [rbp] and [r13] need an explicit zero displacement; [rsp] and [r12] need a SIB byte.
  x64_check_bytes(asm_addr(X64_RBP, 0), (const uint8_t[]){ 0x48, 0x8b, 0x45, 0x00 }, 4);
  x64_check_bytes(asm_addr(X64_R13, 0), (const uint8_t[]){ 0x49, 0x8b, 0x45, 0x00 }, 4);
  x64_check_bytes(asm_addr(X64_RSP, 0), (const uint8_t[]){ 0x48, 0x8b, 0x04, 0x24 }, 4);
  x64_check_bytes(asm_addr(X64_R12, 0), (const uint8_t[]){ 0x49, 0x8b, 0x04, 0x24 }, 4);
}

static void x64_rsp_as_index(void) {
  x64_asm_t a;
  x64_init(&a);
  x64_mov_rm(&a, ASM_W64, X64_RAX, asm_addr_indexed(X64_RAX, X64_RSP, ASM_SCALE_1, 0));
}

static void x64_rsp_cannot_be_an_index(void) {
#if !defined(_WIN32)
  CHECK(panics_with(x64_rsp_as_index, "rsp cannot be an index register"), "expected a panic");
#endif
}

static void x64_arithmetic_and_logic(void) {
  x64_arithmetic_and_logic_table();
  x64_asm_t a;
  x64_init(&a);
  x64_add_ri(&a, ASM_W64, X64_RAX, 1);
  asm_code_t code = finish_x64(&a);
  CHECK(code.len == 4 && bytes_equal(&code, 0, (const uint8_t[]){ 0x48, 0x83, 0xc0, 0x01 }, 4), "add rax, 1 uses imm8");
  asm_code_free(&code);
  x64_init(&a);
  x64_add_ri(&a, ASM_W64, X64_RAX, 128);
  code = finish_x64(&a);
  CHECK(code.len == 7, "add rax, 128 uses imm32");
  asm_code_free(&code);
}

static void x64_condition_codes(void) {
  static const struct { x64_cond_t cond; const char *suffix; } conditions[] = {
    { X64_COND_OVERFLOW, "o" }, { X64_COND_NO_OVERFLOW, "no" }, { X64_COND_BELOW, "b" },
    { X64_COND_ABOVE_OR_EQUAL, "ae" }, { X64_COND_EQUAL, "e" }, { X64_COND_NOT_EQUAL, "ne" },
    { X64_COND_BELOW_OR_EQUAL, "be" }, { X64_COND_ABOVE, "a" }, { X64_COND_SIGN, "s" },
    { X64_COND_NOT_SIGN, "ns" }, { X64_COND_PARITY, "p" }, { X64_COND_NOT_PARITY, "np" },
    { X64_COND_LESS, "l" }, { X64_COND_GREATER_OR_EQUAL, "ge" }, { X64_COND_LESS_OR_EQUAL, "le" },
    { X64_COND_GREATER, "g" },
  };
  for (size_t i = 0; i < sizeof(conditions) / sizeof(conditions[0]); i++) {
    x64_cond_t cond = conditions[i].cond;
    x64_asm_t a;
    x64_init(&a);
    asm_label_t label = x64_new_label(&a);
    x64_bind(&a, label);
    x64_setcc(&a, cond, X64_RAX);
    x64_cmovcc(&a, cond, ASM_W64, X64_RBX, X64_R12);
    x64_jcc(&a, cond, label);
    asm_code_t code = finish_x64(&a);
    char expected[128], actual[256];
    const char *s = conditions[i].suffix;
    snprintf(expected, sizeof(expected), "set%s al; cmov%s rbx, r12; j%s short 0", s, s, s);
    join_listing(ASM_ARCH_X64, code.bytes, 0, code.len, actual, sizeof(actual));
    CHECK(strcmp(expected, actual) == 0, "expected %s, got %s", expected, actual);
    CHECK(x64_cond_invert(x64_cond_invert(cond)) == cond && x64_cond_invert(cond) != cond, "invert %s", s);
    asm_code_free(&code);
  }
  x64_condition_codes_table();
}

static void x64_branches_to_labels(void) {
  x64_branches_to_labels_table();
  // Backward branches just out of rel8 range are near.
  x64_asm_t a;
  x64_init(&a);
  asm_label_t label = x64_new_label(&a);
  x64_bind(&a, label);
  x64_nops(&a, 126);
  x64_jmp(&a, label);
  x64_nops(&a, 1);
  x64_jcc(&a, X64_COND_EQUAL, label);
  asm_code_t code = finish_x64(&a);
  CHECK(bytes_equal(&code, 126, (const uint8_t[]){ 0xeb, 0x80 }, 2), "short jmp at the rel8 limit");
  CHECK(bytes_equal(&code, 129, (const uint8_t[]){ 0x0f, 0x84, 0x79, 0xff, 0xff, 0xff }, 6), "near jcc past the rel8 limit");
  asm_code_free(&code);
}

static void x64_short_forward_branch_out_of_range_fails(void) {
  x64_asm_t a;
  x64_init(&a);
  asm_label_t label = x64_new_label(&a);
  x64_jmp_short(&a, label);
  x64_nops(&a, 127);
  x64_bind(&a, label);
  asm_code_t code;
  CHECK(x64_finish(&a, &code) == ASM_OK, "127 bytes forward reaches");
  asm_code_free(&code);

  x64_init(&a);
  label = x64_new_label(&a);
  x64_jcc_short(&a, X64_COND_EQUAL, label);
  x64_nops(&a, 128);
  x64_bind(&a, label);
  CHECK(x64_finish(&a, &code) == ASM_ERR_BRANCH_OUT_OF_RANGE, "128 bytes forward does not reach");
}

static void x64_unbound_label_fails(void) {
  x64_asm_t a;
  x64_init(&a);
  x64_jmp(&a, x64_new_label(&a));
  asm_code_t code;
  CHECK(x64_finish(&a, &code) == ASM_ERR_UNBOUND_LABEL, "expected unbound label");
}

static void x64_nops_and_alignment(void) {
  for (size_t size = 0; size <= 20; size++) {
    x64_asm_t a;
    x64_init(&a);
    x64_nops(&a, size);
    asm_code_t code = finish_x64(&a);
    CHECK(code.len == size, "nops(%zu) emitted %zu bytes", size, code.len);
    for (size_t offset = 0; offset < code.len;) {
      asm_insn_t insn;
      size_t length = asm_disasm_x64(code.bytes, code.len, offset, &insn);
      // The recommended two-byte NOP is 66 90, which decodes as xchg.
      CHECK(length && (strncmp(insn.text, "nop", 3) == 0 || strcmp(insn.text, "xchg ax, ax") == 0), "nops(%zu): %s", size, insn.text);
      offset += length ? length : 1;
    }
    asm_code_free(&code);
  }
  x64_asm_t a;
  x64_init(&a);
  x64_ret(&a);
  x64_align_code(&a, 16);
  x64_int3(&a);
  x64_align_data(&a, 8);
  asm_code_t code = finish_x64(&a);
  CHECK(code.len == 24, "aligned length %zu", code.len);
  for (size_t i = 17; i < code.len; i++) CHECK(code.bytes[i] == 0, "data padding at %zu", i);
  asm_code_free(&code);
}

static void x64_data_emission(void) {
  x64_asm_t a;
  x64_init(&a);
  x64_data_u8(&a, 1);
  x64_data_u32(&a, 0x03020100);
  x64_data_u64(&a, 0x0b0a090807060504);
  x64_data_bytes(&a, (const uint8_t[]){ 0xc, 0xd }, 2);
  asm_code_t code = finish_x64(&a);
  static const uint8_t expected[] = { 1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
  CHECK(code.len == sizeof(expected) && bytes_equal(&code, 0, expected, sizeof(expected)), "bad data");
  asm_code_free(&code);
}

static void x64_masm_frames(void) {
  asm_frame_t frame = masm_x64_frame(
    ASM_REG(X64_RBX) | ASM_REG(X64_R12) | ASM_REG(X64_RBP) | ASM_REG(X64_RSP), ASM_REG(X64_XMM8), 20
  );
  CHECK(frame.saved_gprs == (ASM_REG(X64_RBX) | ASM_REG(X64_R12)), "saved gprs %#x", frame.saved_gprs);
  CHECK(frame.locals_size == 24, "locals %u", frame.locals_size);
  CHECK(frame.caller_stack_offset == 64, "caller stack offset %u", frame.caller_stack_offset);
  x64_masm_frames_table();
}

static void x64_masm_register_sets_are_consistent(void) {
  CHECK((MASM_X64_ALLOCATABLE_GPRS & MASM_X64_SCRATCH_GPRS) == 0, "allocatable/scratch gprs");
  CHECK((MASM_X64_ALLOCATABLE_FPRS & MASM_X64_SCRATCH_FPRS) == 0, "allocatable/scratch fprs");
  CHECK(!asm_regset_contains(MASM_X64_ALLOCATABLE_GPRS, MASM_X64_STACK_POINTER), "sp allocatable");
  CHECK(!asm_regset_contains(MASM_X64_ALLOCATABLE_GPRS, MASM_X64_FRAME_POINTER), "fp allocatable");
  CHECK((MASM_X64_CALLEE_SAVED_GPRS & MASM_X64_CALLER_SAVED_GPRS) == 0, "callee/caller saved");
  CHECK((MASM_X64_SCRATCH_GPRS & ~MASM_X64_CALLER_SAVED_GPRS) == 0, "scratch not caller saved");
  CHECK(asm_regset_len(MASM_X64_ALLOCATABLE_GPRS) == 12, "allocatable gprs");
  CHECK(asm_regset_len(MASM_X64_ALLOCATABLE_FPRS) == 15, "allocatable fprs");
}

static asm_code_t a64_mov_imm_code(asm_width_t width, uint64_t value) {
  a64_asm_t a;
  a64_init(&a);
  a64_mov_imm(&a, width, A64_X9, value);
  return finish_a64(&a);
}

static bool decode_bit_masks(uint32_t n, uint32_t imms, uint32_t immr, asm_width_t width, uint64_t *out) {
  uint32_t combined = (n << 6) | (~imms & 0x3f);
  if (combined == 0) return false;
  uint32_t length = 31 - (uint32_t)__builtin_clz(combined);
  if (length < 1) return false;
  uint32_t size = UINT32_C(1) << length;
  if (size > asm_width_bits(width)) return false;
  uint32_t levels = size - 1;
  uint32_t s = imms & levels, r = immr & levels;
  if (s == levels) return false;
  uint64_t ones = (UINT64_C(1) << (s + 1)) - 1;
  uint64_t element;
  if (size == 64) {
    element = r ? (ones >> r) | (ones << (64 - r)) : ones;
  } else {
    uint64_t mask = (UINT64_C(1) << size) - 1;
    element = ((ones >> r) | (ones << ((size - r) % size))) & mask;
  }
  uint64_t value = 0;
  for (uint32_t position = 0; position < asm_width_bits(width); position += size) value |= element << position;
  *out = value;
  return true;
}

// The value a sequence of MOVZ/MOVN/MOVK/ORR-immediate instructions leaves in its destination.
static bool emulate_materialization(const asm_code_t *code, asm_width_t width, uint64_t *out) {
  uint64_t value = 0;
  for (size_t at = 0; at < code->len; at += 4) {
    uint32_t word = word_at(code->bytes, at);
    if ((word >> 31) != (width == ASM_W64 ? 1u : 0u)) return false;
    switch ((word >> 23) & 0x3f) {
      case 0x25: {
        uint32_t shift = 16 * ((word >> 21) & 3);
        uint64_t imm = (uint64_t)((word >> 5) & 0xffff) << shift;
        switch ((word >> 29) & 3) {
          case 0: value = ~imm; break;
          case 2: value = imm; break;
          case 3: value = (value & ~(UINT64_C(0xffff) << shift)) | imm; break;
          default: return false;
        }
        break;
      }
      case 0x24:
        if (((word >> 29) & 3) != 1 || ((word >> 5) & 31) != 31) return false;
        if (!decode_bit_masks((word >> 22) & 1, (word >> 10) & 0x3f, (word >> 16) & 0x3f, width, &value)) return false;
        break;
      default:
        return false;
    }
  }
  *out = width == ASM_W32 ? value & UINT32_MAX : value;
  return true;
}

static void a64_materialized_immediates_have_the_right_value(void) {
  uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
  static uint64_t values[20007];
  size_t count = 0;
  static const uint64_t fixed[] = { 0, 1, UINT64_MAX, 0xffff, 0xffff0000, UINT64_C(1) << 63, UINT64_C(0x5555555555555555) };
  for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) values[count++] = fixed[i];
  for (int i = 0; i < 20000; i++) {
    uint64_t random, mask;
    state ^= state >> 12; state ^= state << 25; state ^= state >> 27;
    random = state * UINT64_C(0x2545f4914f6cdd1d);
    state ^= state >> 12; state ^= state << 25; state ^= state >> 27;
    mask = state * UINT64_C(0x2545f4914f6cdd1d);
    // Bias toward halfwords of all zeros or all ones.
    uint64_t value = random;
    for (int h = 0; h < 4; h++) {
      switch ((mask >> (8 * h)) & 3) {
        case 0: value &= ~(UINT64_C(0xffff) << (16 * h)); break;
        case 1: value |= UINT64_C(0xffff) << (16 * h); break;
        default: break;
      }
    }
    values[count++] = value;
  }
  for (size_t i = 0; i < count; i++) {
    for (int w = 0; w < 2; w++) {
      asm_width_t width = w ? ASM_W64 : ASM_W32;
      asm_code_t code = a64_mov_imm_code(width, values[i]);
      uint64_t expected = width == ASM_W32 ? values[i] & UINT32_MAX : values[i], actual = 0;
      CHECK(emulate_materialization(&code, width, &actual) && actual == expected, "%#" PRIx64 " W%u", values[i], asm_width_bits(width));
      CHECK(code.len <= asm_width_bits(width) / 4, "%#" PRIx64 " took %zu bytes", values[i], code.len);
      asm_code_free(&code);
    }
  }
}

static int compare_u64(const void *x, const void *y) {
  uint64_t a = *(const uint64_t *)x, b = *(const uint64_t *)y;
  return a < b ? -1 : a > b;
}

static void a64_logical_immediates_round_trip_exhaustively(void) {
  static uint64_t values[2 * 64 * 64];
  for (int w = 0; w < 2; w++) {
    asm_width_t width = w ? ASM_W32 : ASM_W64;
    size_t expected_count = w ? 1302 : 5334, count = 0;
    for (uint32_t n = 0; n < 2; n++)
      for (uint32_t immr = 0; immr < 64; immr++)
        for (uint32_t imms = 0; imms < 64; imms++) {
          uint64_t value, decoded;
          if (!decode_bit_masks(n, imms, immr, width, &value)) continue;
          values[count++] = value;
          uint32_t encoding;
          bool ok = a64_encode_logical_immediate(value, width, &encoding);
          CHECK(ok, "%#" PRIx64 " should be encodable for W%u", value, asm_width_bits(width));
          if (!ok) continue;
          CHECK(
            decode_bit_masks(encoding >> 12, encoding & 0x3f, (encoding >> 6) & 0x3f, width, &decoded) && decoded == value,
            "%#" PRIx64 " W%u encoded as %#x", value, asm_width_bits(width), encoding
          );
        }
    qsort(values, count, sizeof(values[0]), compare_u64);
    size_t unique = 0;
    for (size_t i = 0; i < count; i++)
      if (unique == 0 || values[unique - 1] != values[i]) values[unique++] = values[i];
    CHECK(unique == expected_count, "W%u has %zu logical immediates", asm_width_bits(width), unique);

    // Everything else is rejected.
    uint64_t state = UINT64_C(0x123456789abcdef1);
    for (int i = 0; i < 100000; i++) {
      state ^= state << 13; state ^= state >> 7; state ^= state << 17;
      uint64_t value = width == ASM_W32 ? state & UINT32_MAX : state;
      uint32_t encoding;
      bool member = bsearch(&value, values, unique, sizeof(values[0]), compare_u64) != NULL;
      CHECK(a64_encode_logical_immediate(value, width, &encoding) == member, "%#" PRIx64 " W%u", value, asm_width_bits(width));
    }
  }
  uint32_t encoding;
  CHECK(!a64_encode_logical_immediate(0, ASM_W64, &encoding), "0");
  CHECK(!a64_encode_logical_immediate(UINT64_MAX, ASM_W64, &encoding), "all ones");
  CHECK(!a64_encode_logical_immediate(0xffffffff, ASM_W32, &encoding), "32-bit all ones");
  CHECK(!a64_encode_logical_immediate(UINT64_C(0x100000000), ASM_W32, &encoding), "beyond 32 bits");
  CHECK(!a64_encode_logical_immediate(0x1234, ASM_W64, &encoding), "0x1234");
  CHECK(a64_encode_logical_immediate(0xffffffff, ASM_W64, &encoding), "64-bit 0xffffffff");
}

static void a64_and_unencodable(void) {
  a64_asm_t a;
  a64_init(&a);
  a64_and_imm(&a, ASM_W64, A64_X0, A64_X1, 0x1234);
}

static void a64_unencodable_logical_immediate_is_rejected(void) {
#if !defined(_WIN32)
  CHECK(panics_with(a64_and_unencodable, "is not a valid 64-bit logical immediate"), "expected a panic");
#endif
}

static void a64_ldr_unencodable(void) {
  a64_asm_t a;
  a64_init(&a);
  a64_ldr(&a, ASM_W64, A64_X0, a64_mem_offset(A64_X1, 32768));
}

static void a64_unencodable_load_offset_is_rejected(void) {
#if !defined(_WIN32)
  CHECK(panics_with(a64_ldr_unencodable, "not encodable"), "expected a panic");
#endif
}

static void a64_conditional_select_and_condition_codes(void) {
  a64_conditional_select_and_condition_codes_table();
  static const struct { a64_cond_t cond; const char *name; } conditions[] = {
    { A64_COND_EQ, "eq" }, { A64_COND_NE, "ne" }, { A64_COND_HS, "hs" }, { A64_COND_LO, "lo" },
    { A64_COND_MI, "mi" }, { A64_COND_PL, "pl" }, { A64_COND_VS, "vs" }, { A64_COND_VC, "vc" },
    { A64_COND_HI, "hi" }, { A64_COND_LS, "ls" }, { A64_COND_GE, "ge" }, { A64_COND_LT, "lt" },
    { A64_COND_GT, "gt" }, { A64_COND_LE, "le" },
  };
  for (size_t i = 0; i < sizeof(conditions) / sizeof(conditions[0]); i++) {
    a64_cond_t cond = conditions[i].cond;
    a64_asm_t a;
    a64_init(&a);
    asm_label_t label = a64_new_label(&a);
    a64_bind(&a, label);
    a64_b_cond(&a, cond, label);
    a64_cset(&a, ASM_W64, A64_X3, cond);
    asm_code_t code = finish_a64(&a);
    char expected[64], actual[128];
    snprintf(expected, sizeof(expected), "b.%s 0x0; cset x3, %s", conditions[i].name, conditions[i].name);
    join_listing(ASM_ARCH_A64, code.bytes, 0, code.len, actual, sizeof(actual));
    CHECK(strcmp(expected, actual) == 0, "expected %s, got %s", expected, actual);
    CHECK(a64_cond_invert(a64_cond_invert(cond)) == cond && a64_cond_invert(cond) != cond, "invert %s", conditions[i].name);
    asm_code_free(&code);
  }
}

static void a64_fp_immediates_round_trip(void) {
  for (unsigned imm8 = 0; imm8 <= 255; imm8++) {
    // VFPExpandImm for a double.
    uint64_t sign = imm8 >> 7;
    uint64_t b = (imm8 >> 6) & 1;
    uint64_t exponent = ((b ^ 1) << 10) | (b ? UINT64_C(0xff) << 2 : 0) | ((imm8 >> 4) & 3);
    uint64_t fraction = (uint64_t)(imm8 & 0xf) << 48;
    double value = asm_f64_from_bits((sign << 63) | (exponent << 52) | fraction);
    uint8_t encoded;
    CHECK(a64_encode_fp_immediate(value, &encoded) && encoded == imm8, "%g", value);
  }
  static const double rejected[] = { 0.0, -0.0, 0.1, 32.0, 1.0 / 3.0, __builtin_nan(""), __builtin_inf(), 0.0625 };
  for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
    uint8_t encoded;
    CHECK(!a64_encode_fp_immediate(rejected[i], &encoded), "%g", rejected[i]);
  }
}

static void a64_backward_branches_out_of_range_use_a_long_branch(void) {
  // TBZ reaches exactly 32 KiB back.
  a64_asm_t a;
  a64_init(&a);
  asm_label_t label = a64_new_label(&a);
  a64_bind(&a, label);
  for (int i = 0; i < 8192; i++) a64_nop(&a);
  a64_tbz(&a, A64_X3, 3, label);
  a64_tbnz(&a, A64_X4, 5, label);
  a64_cbz(&a, ASM_W64, A64_X5, label);
  asm_code_t code = finish_a64(&a);
  char actual[256];
  join_listing(ASM_ARCH_A64, code.bytes, 0x8000, code.len, actual, sizeof(actual));
  const char *expected = "tbz w3, #0x3, 0x0; tbz w4, #0x5, 0x800c; b 0x0; cbz x5, 0x0";
  CHECK(strcmp(actual, expected) == 0, "expected %s, got %s", expected, actual);
  asm_code_free(&code);

  // B.cond reaches exactly 1 MiB back.
  a64_init(&a);
  label = a64_new_label(&a);
  a64_bind(&a, label);
  for (int i = 0; i < (1 << 18); i++) a64_nop(&a);
  a64_b_cond(&a, A64_COND_EQ, label);
  a64_b_cond(&a, A64_COND_LT, label);
  a64_cbnz(&a, ASM_W32, A64_X0, label);
  code = finish_a64(&a);
  join_listing(ASM_ARCH_A64, code.bytes, 0x100000, code.len, actual, sizeof(actual));
  expected = "b.eq 0x0; b.ge 0x10000c; b 0x0; cbz w0, 0x100014; b 0x0";
  CHECK(strcmp(actual, expected) == 0, "expected %s, got %s", expected, actual);
  asm_code_free(&code);
}

// Follows the branch at offset and returns its target.
static size_t branch_target(const asm_code_t *code, size_t offset) {
  uint32_t word = word_at(code->bytes, offset);
  int64_t displacement;
  if ((word & 0xfc000000) == 0x14000000) displacement = (int64_t)((int32_t)(word << 6) >> 6) * 4;
  else if ((word & 0x7e000000) == 0x36000000) displacement = (int64_t)((int32_t)(word << 13) >> 18) * 4;
  else displacement = (int64_t)((int32_t)(word << 8) >> 13) * 4;
  return (size_t)((int64_t)offset + displacement);
}

static void a64_forward_branches_get_veneers_before_going_out_of_range(void) {
  a64_asm_t a;
  a64_init(&a);
  asm_label_t far = a64_new_label(&a);
  asm_label_t near = a64_new_label(&a);
  a64_tbz(&a, A64_X3, 3, far);
  a64_cbz(&a, ASM_W64, A64_X4, far);
  a64_tbnz(&a, A64_X5, 63, near);
  for (int i = 0; i < 8; i++) a64_nop(&a);
  a64_bind(&a, near);
  const size_t nop_count = 9000;
  for (size_t i = 0; i < nop_count; i++) a64_nop(&a);
  a64_bind(&a, far);
  a64_ret(&a);
  size_t far_offset = a64_label_offset(&a, far);
  asm_code_t code = finish_a64(&a);

  // The near branch was resolved directly; the TBZ goes through a veneer; the CBZ still reaches directly.
  CHECK(branch_target(&code, 8) == 8 + 4 * 9, "near branch");
  size_t veneer = branch_target(&code, 0);
  CHECK(veneer < 32 * 1024, "veneer at %zu", veneer);
  CHECK((word_at(code.bytes, veneer) & 0xfc000000) == 0x14000000, "veneer is a b");
  CHECK(branch_target(&code, veneer) == far_offset, "veneer target");
  CHECK(branch_target(&code, 4) == far_offset, "cbz target");
  // The fallthrough path jumps over the pool.
  size_t pool_start = veneer - 4;
  CHECK(branch_target(&code, pool_start) == veneer + 4, "pool skip");
  size_t nops = 0;
  for (size_t at = 0; at < code.len; at += 4) nops += word_at(code.bytes, at) == NOP;
  CHECK(nops == nop_count + 8, "%zu nops", nops);
  CHECK(code.len == 4 * (3 + 8 + nop_count + 2 + 1), "length %zu", code.len);
  asm_code_free(&code);
}

static void a64_veneer_pools_do_not_split_a_label_reference_from_its_fixup(void) {
  // Sweep the distance so that some pool is due exactly before the reference, which is the last instruction before its label.
  for (int reference = 0; reference < 2; reference++) {
    for (int nop_count = 8100; nop_count < 8192; nop_count++) {
      a64_asm_t a;
      a64_init(&a);
      asm_label_t label = a64_new_label(&a);
      a64_tbz(&a, A64_X3, 3, label);
      for (int i = 0; i < nop_count; i++) a64_nop(&a);
      if (reference == 0) a64_b(&a, label);
      else a64_ldr_literal(&a, A64_X1, label);
      a64_bind(&a, label);
      a64_ret(&a);
      asm_code_t code = finish_a64(&a);
      CHECK(branch_target(&code, code.len - 8) == code.len - 4, "%d nops", nop_count);
      asm_code_free(&code);
    }
  }
}

static void a64_many_pending_branches_get_veneers(void) {
  a64_asm_t a;
  a64_init(&a);
  asm_label_t labels[300];
  size_t branches[300];
  for (int i = 0; i < 300; i++) labels[i] = a64_new_label(&a);
  for (int i = 0; i < 300; i++) {
    branches[i] = a64_offset(&a);
    a64_tbz(&a, (asm_gpr_t)(i % 29), (uint8_t)(i % 64), labels[i]);
    if (i % 2 == 0) a64_b_cond(&a, A64_COND_NE, labels[i]);
  }
  for (int i = 0; i < 40 * 1024 / 4; i++) a64_nop(&a);
  for (int i = 0; i < 300; i++) {
    a64_bind(&a, labels[i]);
    a64_nop(&a);
  }
  size_t offsets[300];
  for (int i = 0; i < 300; i++) offsets[i] = a64_label_offset(&a, labels[i]);
  asm_code_t code = finish_a64(&a);
  for (int i = 0; i < 300; i++) {
    size_t target = branch_target(&code, branches[i]);
    if (target != offsets[i]) target = branch_target(&code, target);
    CHECK(target == offsets[i], "branch %d", i);
  }
  asm_code_free(&code);
}

static void a64_forward_branch_beyond_branch_range_fails(void) {
  a64_asm_t a;
  a64_init(&a);
  asm_label_t label = a64_new_label(&a);
  a64_adr(&a, A64_X0, label);
  for (int i = 0; i < (1 << 18); i++) a64_nop(&a);
  a64_bind(&a, label);
  asm_code_t code;
  CHECK(a64_finish(&a, &code) == ASM_ERR_BRANCH_OUT_OF_RANGE, "expected out of range");
}

static void a64_unbound_label_fails(void) {
  a64_asm_t a;
  a64_init(&a);
  a64_cbz(&a, ASM_W64, A64_X0, a64_new_label(&a));
  asm_code_t code;
  CHECK(a64_finish(&a, &code) == ASM_ERR_UNBOUND_LABEL, "expected unbound label");
}

static void a64_alignment_and_data(void) {
  a64_asm_t a;
  a64_init(&a);
  a64_ret(&a);
  a64_align_code(&a, 16);
  a64_data_u32(&a, 0x12345678);
  a64_data_bytes(&a, (const uint8_t[]){ 1 }, 1);
  a64_align_data(&a, 8);
  a64_data_u64(&a, UINT64_MAX);
  asm_code_t code = finish_a64(&a);
  CHECK(code.len == 32, "length %zu", code.len);
  CHECK(word_at(code.bytes, 4) == NOP && word_at(code.bytes, 12) == NOP, "code padding");
  CHECK(word_at(code.bytes, 16) == 0x12345678, "data word");
  CHECK(bytes_equal(&code, 20, (const uint8_t[]){ 1, 0, 0, 0 }, 4), "data padding");
  for (size_t i = 24; i < 32; i++) CHECK(code.bytes[i] == 0xff, "data u64 at %zu", i);
  asm_code_free(&code);
}

static void a64_masm_frames(void) {
  asm_frame_t frame = masm_a64_frame(
    ASM_REG(A64_X19) | ASM_REG(A64_X20) | ASM_REG(A64_X21) | ASM_REG(A64_FP) | ASM_REG(A64_LR) | ASM_REG(A64_SP),
    ASM_REG(A64_D8), 20
  );
  CHECK(frame.saved_gprs == (ASM_REG(A64_X19) | ASM_REG(A64_X20) | ASM_REG(A64_X21)), "saved gprs %#x", frame.saved_gprs);
  CHECK(frame.locals_size == 32, "locals %u", frame.locals_size);
  CHECK(frame.caller_stack_offset == 96, "caller stack offset %u", frame.caller_stack_offset);
  a64_masm_frames_table();
}

static void a64_masm_register_sets_are_consistent(void) {
  CHECK((MASM_A64_ALLOCATABLE_GPRS & MASM_A64_SCRATCH_GPRS) == 0, "allocatable/scratch gprs");
  CHECK((MASM_A64_ALLOCATABLE_FPRS & MASM_A64_SCRATCH_FPRS) == 0, "allocatable/scratch fprs");
  CHECK(!asm_regset_contains(MASM_A64_ALLOCATABLE_GPRS, MASM_A64_STACK_POINTER), "sp allocatable");
  CHECK(!asm_regset_contains(MASM_A64_ALLOCATABLE_GPRS, MASM_A64_FRAME_POINTER), "fp allocatable");
  CHECK(!asm_regset_contains(MASM_A64_ALLOCATABLE_GPRS, A64_LR), "lr allocatable");
  CHECK(!asm_regset_contains(MASM_A64_ALLOCATABLE_GPRS, A64_X18), "x18 allocatable");
  CHECK((MASM_A64_CALLEE_SAVED_GPRS & MASM_A64_CALLER_SAVED_GPRS) == 0, "callee/caller saved gprs");
  CHECK((MASM_A64_CALLEE_SAVED_FPRS & MASM_A64_CALLER_SAVED_FPRS) == 0, "callee/caller saved fprs");
  CHECK((MASM_A64_SCRATCH_GPRS & ~MASM_A64_CALLER_SAVED_GPRS) == 0, "scratch not caller saved");
  CHECK(asm_regset_len(MASM_A64_ALLOCATABLE_GPRS) == 26, "allocatable gprs");
  CHECK(asm_regset_len(MASM_A64_ALLOCATABLE_FPRS) == 31, "allocatable fprs");
  asm_regset_t x19_to_x30 = 0;
  for (uint8_t r = 19; r < 31; r++) x19_to_x30 |= ASM_REG(r);
  CHECK(MASM_A64_CALLEE_SAVED_GPRS == x19_to_x30, "callee saved gprs are x19-x30");
}

static void disasm_lists_x86_64_code_with_constants(void) {
  // mov r11, 0x123456789abcdef0; call r11; ret
  static const uint8_t code[] = { 0x49, 0xbb, 0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12, 0x41, 0xff, 0xd3, 0xc3 };
  asm_listing_t listing = { 0 };
  asm_listing(&listing, ASM_ARCH_X64, code, sizeof(code));
  CHECK(listing.count == 3, "%zu instructions", listing.count);
  if (listing.count == 3) {
    CHECK(listing.items[0].offset == 0 && strcmp(listing.items[0].text, "mov r11, 0x123456789abcdef0") == 0, "%s", listing.items[0].text);
    CHECK(listing.items[0].has_constant && listing.items[0].constant == UINT64_C(0x123456789abcdef0), "movabs constant");
    CHECK(listing.items[1].offset == 10 && strcmp(listing.items[1].text, "call r11") == 0 && !listing.items[1].has_constant, "%s", listing.items[1].text);
    CHECK(listing.items[2].offset == 13 && strcmp(listing.items[2].text, "ret") == 0 && !listing.items[2].has_constant, "%s", listing.items[2].text);
  }
  asm_listing_free(&listing);
}

static void disasm_lists_aarch64_code_with_constants(void) {
  masm_a64_t m;
  masm_a64_init(&m);
  masm_a64_move_imm64(&m, A64_X9, UINT64_C(0x123400000000def0));
  asm_code_t code;
  CHECK(masm_a64_finish(&m, &code) == ASM_OK, "finish failed");
  asm_listing_t listing = { 0 };
  asm_listing(&listing, ASM_ARCH_A64, code.bytes, code.len);
  CHECK(listing.count > 0, "empty listing");
  if (listing.count > 0) {
    asm_insn_t *last = &listing.items[listing.count - 1];
    CHECK(last->has_constant && last->constant == UINT64_C(0x123400000000def0), "tracked constant");
    CHECK(strncmp(listing.items[0].text, "mov", 3) == 0, "%s", listing.items[0].text);
  }
  asm_listing_free(&listing);
  asm_code_free(&code);
}

static void disasm_lists_loads_from_the_constant_pool(void) {
  masm_a64_t m;
  masm_a64_init(&m);
  masm_a64_move_imm64(&m, A64_X9, UINT64_C(0x123456789abcdef0));
  asm_code_t code;
  CHECK(masm_a64_finish(&m, &code) == ASM_OK, "finish failed");
  asm_listing_t listing = { 0 };
  asm_listing_with_data(&listing, ASM_ARCH_A64, code.bytes, code.len, code.data_offset);
  CHECK(listing.count > 1, "short listing");
  if (listing.count > 1) {
    CHECK(strncmp(listing.items[0].text, "ldr", 3) == 0, "%s", listing.items[0].text);
    CHECK(listing.items[0].has_constant && listing.items[0].constant == UINT64_C(0x123456789abcdef0), "pool constant");
    CHECK(strcmp(listing.items[listing.count - 1].text, ".quad 0x123456789abcdef0") == 0, "%s", listing.items[listing.count - 1].text);
  }
  asm_listing_free(&listing);
  asm_code_free(&code);

  masm_x64_t x;
  masm_x64_init(&x);
  masm_x64_move_imm64(&x, X64_RAX, UINT64_C(0x123456789abcdef0));
  CHECK(masm_x64_finish(&x, &code) == ASM_OK, "finish failed");
  listing = (asm_listing_t){ 0 };
  asm_listing_with_data(&listing, ASM_ARCH_X64, code.bytes, code.len, code.data_offset);
  CHECK(listing.count > 0 && listing.items[0].has_constant && listing.items[0].constant == UINT64_C(0x123456789abcdef0), "rip-relative constant");
  asm_listing_free(&listing);
  asm_code_free(&code);
}

typedef struct {
  const char *name;
  void (*run)(void);
} test_case_t;

static const test_case_t tests[] = {
  { "buffer_forward_and_backward_references_are_patched", buffer_forward_and_backward_references_are_patched },
  { "buffer_unbound_label_fails", buffer_unbound_label_fails },
  { "buffer_unreferenced_unbound_label_is_fine", buffer_unreferenced_unbound_label_is_fine },
  { "buffer_out_of_range_reference_fails", buffer_out_of_range_reference_fails },
  { "buffer_rel8_reaches_exactly_its_range", buffer_rel8_reaches_exactly_its_range },
  { "buffer_alignment_pads_with_filler", buffer_alignment_pads_with_filler },
  { "x64_register_moves", x64_register_moves_table },
  { "x64_immediate_moves_pick_the_shortest_form", x64_immediate_moves_pick_the_shortest_form },
  { "x64_memory_operands", x64_memory_operands },
  { "x64_rsp_cannot_be_an_index", x64_rsp_cannot_be_an_index },
  { "x64_arithmetic_and_logic", x64_arithmetic_and_logic },
  { "x64_condition_codes", x64_condition_codes },
  { "x64_branches_to_labels", x64_branches_to_labels },
  { "x64_short_forward_branch_out_of_range_fails", x64_short_forward_branch_out_of_range_fails },
  { "x64_unbound_label_fails", x64_unbound_label_fails },
  { "x64_nops_and_alignment", x64_nops_and_alignment },
  { "x64_data_emission", x64_data_emission },
  { "x64_scalar_doubles", x64_scalar_doubles_table },
  { "x64_masm_operations_are_legalized", x64_masm_operations_are_legalized_table },
  { "x64_masm_frames", x64_masm_frames },
  { "x64_masm_register_sets_are_consistent", x64_masm_register_sets_are_consistent },
  { "a64_move_wide_immediates", a64_move_wide_immediates_table },
  { "a64_materialized_immediates_have_the_right_value", a64_materialized_immediates_have_the_right_value },
  { "a64_add_and_subtract", a64_add_and_subtract_table },
  { "a64_logical_operations", a64_logical_operations_table },
  { "a64_logical_immediates_round_trip_exhaustively", a64_logical_immediates_round_trip_exhaustively },
  { "a64_unencodable_logical_immediate_is_rejected", a64_unencodable_logical_immediate_is_rejected },
  { "a64_shifts_and_bitfields", a64_shifts_and_bitfields_table },
  { "a64_multiplication", a64_multiplication_table },
  { "a64_conditional_select_and_condition_codes", a64_conditional_select_and_condition_codes },
  { "a64_loads_and_stores", a64_loads_and_stores_table },
  { "a64_unencodable_load_offset_is_rejected", a64_unencodable_load_offset_is_rejected },
  { "a64_branches_and_misc", a64_branches_and_misc_table },
  { "a64_scalar_doubles", a64_scalar_doubles_table },
  { "a64_fp_immediates_round_trip", a64_fp_immediates_round_trip },
  { "a64_backward_branches_out_of_range_use_a_long_branch", a64_backward_branches_out_of_range_use_a_long_branch },
  { "a64_forward_branches_get_veneers_before_going_out_of_range", a64_forward_branches_get_veneers_before_going_out_of_range },
  { "a64_veneer_pools_do_not_split_a_label_reference_from_its_fixup", a64_veneer_pools_do_not_split_a_label_reference_from_its_fixup },
  { "a64_many_pending_branches_get_veneers", a64_many_pending_branches_get_veneers },
  { "a64_forward_branch_beyond_branch_range_fails", a64_forward_branch_beyond_branch_range_fails },
  { "a64_unbound_label_fails", a64_unbound_label_fails },
  { "a64_alignment_and_data", a64_alignment_and_data },
  { "a64_masm_addresses_are_legalized", a64_masm_addresses_are_legalized_table },
  { "a64_masm_operations_are_legalized", a64_masm_operations_are_legalized_table },
  { "a64_masm_branches", a64_masm_branches_table },
  { "a64_masm_frames", a64_masm_frames },
  { "a64_masm_register_sets_are_consistent", a64_masm_register_sets_are_consistent },
  { "disasm_lists_x86_64_code_with_constants", disasm_lists_x86_64_code_with_constants },
  { "disasm_lists_aarch64_code_with_constants", disasm_lists_aarch64_code_with_constants },
  { "disasm_lists_loads_from_the_constant_pool", disasm_lists_loads_from_the_constant_pool },
};

int main(void) {
  size_t passed = 0, failed = 0;
  for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    int before = failures;
    tests[i].run();
    bool ok = failures == before;
    printf("%s %s\n", ok ? "ok  " : "FAIL", tests[i].name);
    if (ok) passed++;
    else failed++;
  }
  printf("%zu passed, %zu failed\n", passed, failed);
  return failed ? 1 : 0;
}
