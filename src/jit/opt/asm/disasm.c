#include "disasm.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>

typedef struct {
  char *buf;
  size_t cap;
  size_t len;
} text_t;

__attribute__((format(printf, 2, 3)))
static void put(text_t *t, const char *fmt, ...) {
  if (t->len >= t->cap) return;
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(t->buf + t->len, t->cap - t->len, fmt, args);
  va_end(args);
  if (n > 0) t->len += (size_t)n;
  if (t->len >= t->cap) t->len = t->cap - 1;
}

static inline text_t text_of(asm_insn_t *out) {
  out->text[0] = '\0';
  return (text_t){ .buf = out->text, .cap = sizeof(out->text) };
}

static inline uint32_t read_u32_le(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint64_t read_u64_le(const uint8_t *p) {
  return (uint64_t)read_u32_le(p) | (uint64_t)read_u32_le(p + 4) << 32;
}

static const char *const x64_gpr64[16] = {
  "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
  "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
};
static const char *const x64_gpr32[16] = {
  "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
  "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d",
};
static const char *const x64_gpr16[16] = {
  "ax", "cx", "dx", "bx", "sp", "bp", "si", "di",
  "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w",
};
static const char *const x64_gpr8_rex[16] = {
  "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil",
  "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b",
};
static const char *const x64_gpr8_legacy[8] = { "al", "cl", "dl", "bl", "ah", "ch", "dh", "bh" };
static const char *const x64_cc[16] = { "o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g" };
static const char *const x64_alu[8] = { "add", "or", "adc", "sbb", "and", "sub", "xor", "cmp" };
static const char *const x64_shifts[8] = { "rol", "ror", "rcl", "rcr", "shl", "shr", "sal", "sar" };

typedef enum { K8, K16, K32, K64, KXMM } x64_kind_t;

typedef struct {
  const uint8_t *code;
  size_t len, pos;
  bool ok;
  bool rex, rex_w, rex_r, rex_x, rex_b;
  bool p66, pf2, pf3;
} x64_reader_t;

typedef struct {
  uint8_t mod, reg, rm;
  bool is_reg, rip, has_base, has_index;
  uint8_t base, index, scale;
  int32_t disp;
} x64_modrm_t;

static inline uint8_t x64_u8(x64_reader_t *r) {
  if (r->pos >= r->len) {
    r->ok = false;
    return 0;
  }
  return r->code[r->pos++];
}

static inline uint32_t x64_u32(x64_reader_t *r) {
  uint32_t v = x64_u8(r);
  v |= (uint32_t)x64_u8(r) << 8;
  v |= (uint32_t)x64_u8(r) << 16;
  v |= (uint32_t)x64_u8(r) << 24;
  return v;
}

static inline uint16_t x64_u16(x64_reader_t *r) {
  uint16_t v = x64_u8(r);
  return (uint16_t)(v | (uint16_t)(x64_u8(r) << 8));
}

static x64_modrm_t x64_modrm(x64_reader_t *r) {
  x64_modrm_t m = { 0 };
  uint8_t byte = x64_u8(r);
  m.mod = byte >> 6;
  m.reg = (uint8_t)(((byte >> 3) & 7) | (r->rex_r ? 8 : 0));
  uint8_t rm = byte & 7;
  if (m.mod == 3) {
    m.is_reg = true;
    m.rm = (uint8_t)(rm | (r->rex_b ? 8 : 0));
    return m;
  }
  if (rm == 4) {
    uint8_t sib = x64_u8(r);
    m.scale = (uint8_t)(1u << (sib >> 6));
    uint8_t index = (uint8_t)(((sib >> 3) & 7) | (r->rex_x ? 8 : 0));
    if (index != 4) {
      m.has_index = true;
      m.index = index;
    }
    uint8_t base = sib & 7;
    if (base == 5 && m.mod == 0) {
      m.disp = (int32_t)x64_u32(r);
    } else {
      m.has_base = true;
      m.base = (uint8_t)(base | (r->rex_b ? 8 : 0));
    }
  } else if (rm == 5 && m.mod == 0) {
    m.rip = true;
    m.disp = (int32_t)x64_u32(r);
    return m;
  } else {
    m.has_base = true;
    m.base = (uint8_t)(rm | (r->rex_b ? 8 : 0));
  }
  if (m.mod == 1) m.disp = (int8_t)x64_u8(r);
  else if (m.mod == 2) m.disp = (int32_t)x64_u32(r);
  return m;
}

static void x64_number(text_t *t, uint64_t value) {
  if (value <= 9) put(t, "%" PRIu64, value);
  else put(t, "0x%" PRIx64, value);
}

static const char *x64_reg(const x64_reader_t *r, x64_kind_t kind, uint8_t reg) {
  switch (kind) {
    case K8: return r->rex ? x64_gpr8_rex[reg & 15] : x64_gpr8_legacy[reg & 7];
    case K16: return x64_gpr16[reg & 15];
    case K32: return x64_gpr32[reg & 15];
    case K64: return x64_gpr64[reg & 15];
    case KXMM: break;
  }
  static const char *const xmm[16] = {
    "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
    "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
  };
  return xmm[reg & 15];
}

static void x64_mem(text_t *t, const x64_modrm_t *m, const char *size, size_t end, size_t *rip_target) {
  if (size) put(t, "%s ptr ", size);
  put(t, "[");
  if (m->rip) {
    size_t target = (size_t)((int64_t)end + m->disp);
    if (rip_target) *rip_target = target;
    x64_number(t, (uint64_t)target);
    put(t, "]");
    return;
  }
  bool any = false;
  if (m->has_base) {
    put(t, "%s", x64_gpr64[m->base]);
    any = true;
  }
  if (m->has_index) {
    put(t, "%s%s", any ? "+" : "", x64_gpr64[m->index]);
    if (m->scale != 1) put(t, "*%u", m->scale);
    any = true;
  }
  if (m->disp != 0 || !any) {
    if (m->disp < 0) {
      put(t, "-");
      x64_number(t, (uint64_t)(-(int64_t)m->disp));
    } else {
      if (any) put(t, "+");
      x64_number(t, (uint64_t)m->disp);
    }
  }
  put(t, "]");
}

static const char *x64_size_name(x64_kind_t kind) {
  switch (kind) {
    case K8: return "byte";
    case K16: return "word";
    case K32: return "dword";
    case K64: return "qword";
    case KXMM: return "xmmword";
  }
  return "";
}

typedef struct {
  x64_reader_t *r;
  text_t *t;
  size_t *rip_target;
} x64_ctx_t;

static void x64_rm(x64_ctx_t *c, const x64_modrm_t *m, x64_kind_t reg_kind, const char *mem_size) {
  if (m->is_reg) put(c->t, "%s", x64_reg(c->r, reg_kind, m->rm));
  else x64_mem(c->t, m, mem_size, c->r->pos, c->rip_target);
}

static inline uint64_t x64_mask(x64_kind_t kind, uint64_t value) {
  switch (kind) {
    case K8: return value & 0xff;
    case K16: return value & 0xffff;
    case K32: return value & 0xffffffffu;
    default: return value;
  }
}

static bool x64_decode_0f(x64_ctx_t *c, x64_kind_t osz) {
  x64_reader_t *r = c->r;
  text_t *t = c->t;
  uint8_t op = x64_u8(r);
  if (op == 0x0b) {
    put(t, "ud2");
    return r->ok;
  }
  if (op >= 0x80 && op <= 0x8f) {
    int32_t rel = (int32_t)x64_u32(r);
    if (!r->ok) return false;
    put(t, "j%s ", x64_cc[op & 15]);
    x64_number(t, (uint64_t)((int64_t)r->pos + rel));
    return true;
  }
  if (op == 0x3a) {
    if (x64_u8(r) != 0x0b || !r->p66) return false;
    x64_modrm_t m = x64_modrm(r);
    uint8_t imm = x64_u8(r);
    if (!r->ok) return false;
    put(t, "roundsd %s, ", x64_reg(r, KXMM, m.reg));
    x64_rm(c, &m, KXMM, "qword");
    put(t, ", ");
    x64_number(t, imm);
    return true;
  }
  x64_modrm_t m = x64_modrm(r);
  if (!r->ok) return false;
  bool sd = r->pf2, ss = r->pf3, pd = r->p66;
  const char *sse = NULL;
  switch (op) {
    case 0x1f:
      put(t, "nop ");
      x64_rm(c, &m, osz, x64_size_name(osz));
      return true;
    case 0x10:
    case 0x11: {
      const char *name = sd ? "movsd" : ss ? "movss" : pd ? "movupd" : "movups";
      const char *size = sd ? "qword" : ss ? "dword" : "xmmword";
      put(t, "%s ", name);
      if (op == 0x10) {
        put(t, "%s, ", x64_reg(r, KXMM, m.reg));
        x64_rm(c, &m, KXMM, size);
      } else {
        x64_rm(c, &m, KXMM, size);
        put(t, ", %s", x64_reg(r, KXMM, m.reg));
      }
      return true;
    }
    case 0x28: sse = pd ? "movapd" : "movaps"; break;
    case 0x2a:
      if (!sd) return false;
      put(t, "cvtsi2sd %s, ", x64_reg(r, KXMM, m.reg));
      x64_rm(c, &m, r->rex_w ? K64 : K32, r->rex_w ? "qword" : "dword");
      return true;
    case 0x2c:
      if (!sd) return false;
      put(t, "cvttsd2si %s, ", x64_reg(r, r->rex_w ? K64 : K32, m.reg));
      x64_rm(c, &m, KXMM, "qword");
      return true;
    case 0x2e: if (!pd) return false; sse = "ucomisd"; break;
    case 0x50:
      if (!pd || !m.is_reg) return false;
      put(t, "movmskpd %s, %s", x64_reg(r, K32, m.reg), x64_reg(r, KXMM, m.rm));
      return true;
    case 0x51: if (!sd) return false; sse = "sqrtsd"; break;
    case 0x54: if (!pd) return false; sse = "andpd"; break;
    case 0x56: if (!pd) return false; sse = "orpd"; break;
    case 0x5d: if (!sd) return false; sse = "minsd"; break;
    case 0x5f: if (!sd) return false; sse = "maxsd"; break;
    case 0x57: if (!pd) return false; sse = "xorpd"; break;
    case 0x58: if (!sd) return false; sse = "addsd"; break;
    case 0x59: if (!sd) return false; sse = "mulsd"; break;
    case 0x5c: if (!sd) return false; sse = "subsd"; break;
    case 0x5e: if (!sd) return false; sse = "divsd"; break;
    case 0x5a:
      if (sd) sse = "cvtsd2ss";
      else if (ss) sse = "cvtss2sd";
      else return false;
      break;
    case 0x6e:
      if (!pd) return false;
      put(t, "%s %s, ", r->rex_w ? "movq" : "movd", x64_reg(r, KXMM, m.reg));
      x64_rm(c, &m, r->rex_w ? K64 : K32, r->rex_w ? "qword" : "dword");
      return true;
    case 0x7e:
      if (!pd) return false;
      put(t, "%s ", r->rex_w ? "movq" : "movd");
      x64_rm(c, &m, r->rex_w ? K64 : K32, r->rex_w ? "qword" : "dword");
      put(t, ", %s", x64_reg(r, KXMM, m.reg));
      return true;
    case 0xaf:
      put(t, "imul %s, ", x64_reg(r, osz, m.reg));
      x64_rm(c, &m, osz, x64_size_name(osz));
      return true;
    case 0xb6:
    case 0xbe:
      put(t, "%s %s, ", op == 0xb6 ? "movzx" : "movsx", x64_reg(r, osz, m.reg));
      x64_rm(c, &m, K8, "byte");
      return true;
    case 0xb7:
    case 0xbf:
      put(t, "%s %s, ", op == 0xb7 ? "movzx" : "movsx", x64_reg(r, osz, m.reg));
      x64_rm(c, &m, K16, "word");
      return true;
    default:
      break;
  }
  if (sse) {
    put(t, "%s %s, ", sse, x64_reg(r, KXMM, m.reg));
    const char *size = (op == 0x28 || op == 0x54 || op == 0x57) ? "xmmword" : (op == 0x5a && ss) ? "dword" : "qword";
    x64_rm(c, &m, KXMM, size);
    return true;
  }
  if (op >= 0x40 && op <= 0x4f) {
    put(t, "cmov%s %s, ", x64_cc[op & 15], x64_reg(r, osz, m.reg));
    x64_rm(c, &m, osz, x64_size_name(osz));
    return true;
  }
  if (op >= 0x90 && op <= 0x9f) {
    put(t, "set%s ", x64_cc[op & 15]);
    x64_rm(c, &m, K8, "byte");
    return true;
  }
  return false;
}

static bool x64_decode(x64_ctx_t *c, uint64_t *constant, bool *has_constant) {
  x64_reader_t *r = c->r;
  text_t *t = c->t;
  for (;;) {
    uint8_t p = r->pos < r->len ? r->code[r->pos] : 0;
    if (p == 0x66) r->p66 = true;
    else if (p == 0xf2) r->pf2 = true;
    else if (p == 0xf3) r->pf3 = true;
    else break;
    r->pos++;
  }
  uint8_t op = x64_u8(r);
  if (op >= 0x40 && op <= 0x4f) {
    r->rex = true;
    r->rex_w = op & 8;
    r->rex_r = op & 4;
    r->rex_x = op & 2;
    r->rex_b = op & 1;
    op = x64_u8(r);
  }
  if (!r->ok) return false;
  x64_kind_t osz = r->rex_w ? K64 : r->p66 ? K16 : K32;
  const char *msz = x64_size_name(osz);

  if (op == 0x0f) return x64_decode_0f(c, osz);
  if (op < 0x40 && (op & 7) == 1) {
    x64_modrm_t m = x64_modrm(r);
    if (!r->ok) return false;
    put(t, "%s ", x64_alu[op >> 3]);
    x64_rm(c, &m, osz, msz);
    put(t, ", %s", x64_reg(r, osz, m.reg));
    return true;
  }
  if (op < 0x40 && (op & 7) == 3) {
    x64_modrm_t m = x64_modrm(r);
    if (!r->ok) return false;
    put(t, "%s %s, ", x64_alu[op >> 3], x64_reg(r, osz, m.reg));
    x64_rm(c, &m, osz, msz);
    return true;
  }
  if (op >= 0x50 && op <= 0x5f) {
    put(t, "%s %s", op < 0x58 ? "push" : "pop", x64_gpr64[(op & 7) | (r->rex_b ? 8 : 0)]);
    return true;
  }
  if (op >= 0x70 && op <= 0x7f) {
    int8_t rel = (int8_t)x64_u8(r);
    if (!r->ok) return false;
    put(t, "j%s short ", x64_cc[op & 15]);
    x64_number(t, (uint64_t)((int64_t)r->pos + rel));
    return true;
  }
  if (op >= 0xb8 && op <= 0xbf) {
    uint8_t reg = (uint8_t)((op & 7) | (r->rex_b ? 8 : 0));
    if (r->rex_w) {
      uint64_t imm = x64_u32(r);
      imm |= (uint64_t)x64_u32(r) << 32;
      if (!r->ok) return false;
      put(t, "mov %s, ", x64_gpr64[reg]);
      x64_number(t, imm);
      *constant = imm;
      *has_constant = true;
      return true;
    }
    uint32_t imm = x64_u32(r);
    if (!r->ok) return false;
    put(t, "mov %s, ", x64_gpr32[reg]);
    x64_number(t, imm);
    return true;
  }
  switch (op) {
    case 0x63: {
      x64_modrm_t m = x64_modrm(r);
      if (!r->ok) return false;
      put(t, "movsxd %s, ", x64_reg(r, osz, m.reg));
      x64_rm(c, &m, K32, "dword");
      return true;
    }
    case 0x69:
    case 0x6b: {
      x64_modrm_t m = x64_modrm(r);
      int64_t imm = op == 0x6b ? (int8_t)x64_u8(r) : (int32_t)x64_u32(r);
      if (!r->ok) return false;
      put(t, "imul %s, ", x64_reg(r, osz, m.reg));
      x64_rm(c, &m, osz, msz);
      put(t, ", ");
      x64_number(t, x64_mask(osz, (uint64_t)imm));
      return true;
    }
    case 0x81:
    case 0x83: {
      x64_modrm_t m = x64_modrm(r);
      int64_t imm = op == 0x83 ? (int8_t)x64_u8(r) : osz == K16 ? (int16_t)x64_u16(r) : (int32_t)x64_u32(r);
      if (!r->ok) return false;
      put(t, "%s ", x64_alu[m.reg & 7]);
      x64_rm(c, &m, osz, msz);
      put(t, ", ");
      x64_number(t, x64_mask(osz, (uint64_t)imm));
      return true;
    }
    case 0x85:
    case 0x87:
    case 0x88:
    case 0x89: {
      x64_modrm_t m = x64_modrm(r);
      if (!r->ok) return false;
      x64_kind_t kind = op == 0x88 ? K8 : osz;
      put(t, "%s ", op == 0x85 ? "test" : op == 0x87 ? "xchg" : "mov");
      x64_rm(c, &m, kind, x64_size_name(kind));
      put(t, ", %s", x64_reg(r, kind, m.reg));
      return true;
    }
    case 0x8b:
    case 0x8d: {
      x64_modrm_t m = x64_modrm(r);
      if (!r->ok || (op == 0x8d && m.is_reg)) return false;
      put(t, "%s %s, ", op == 0x8b ? "mov" : "lea", x64_reg(r, osz, m.reg));
      x64_rm(c, &m, osz, op == 0x8b ? msz : NULL);
      return true;
    }
    case 0x90:
      if (r->rex_b) return false;
      put(t, "%s", r->p66 ? "xchg ax, ax" : "nop");
      return true;
    case 0x99:
      put(t, "%s", r->rex_w ? "cqo" : r->p66 ? "cwd" : "cdq");
      return true;
    case 0xc1:
    case 0xd1:
    case 0xd3: {
      x64_modrm_t m = x64_modrm(r);
      uint8_t amount = op == 0xc1 ? x64_u8(r) : 1;
      if (!r->ok) return false;
      put(t, "%s ", x64_shifts[m.reg & 7]);
      x64_rm(c, &m, osz, msz);
      if (op == 0xd3) put(t, ", cl");
      else {
        put(t, ", ");
        x64_number(t, amount);
      }
      return true;
    }
    case 0xc3: put(t, "ret"); return true;
    case 0xcc: put(t, "int3"); return true;
    case 0xc6:
    case 0xc7: {
      x64_modrm_t m = x64_modrm(r);
      x64_kind_t kind = op == 0xc6 ? K8 : osz;
      int64_t imm = kind == K8 ? x64_u8(r) : kind == K16 ? x64_u16(r) : (int32_t)x64_u32(r);
      if (!r->ok || (m.reg & 7) != 0) return false;
      put(t, "mov ");
      x64_rm(c, &m, kind, x64_size_name(kind));
      put(t, ", ");
      x64_number(t, x64_mask(kind, (uint64_t)imm));
      return true;
    }
    case 0xe8:
    case 0xe9: {
      int32_t rel = (int32_t)x64_u32(r);
      if (!r->ok) return false;
      put(t, "%s ", op == 0xe8 ? "call" : "jmp");
      x64_number(t, (uint64_t)((int64_t)r->pos + rel));
      return true;
    }
    case 0xeb: {
      int8_t rel = (int8_t)x64_u8(r);
      if (!r->ok) return false;
      put(t, "jmp short ");
      x64_number(t, (uint64_t)((int64_t)r->pos + rel));
      return true;
    }
    case 0xf6:
    case 0xf7: {
      x64_modrm_t m = x64_modrm(r);
      if (!r->ok) return false;
      x64_kind_t kind = op == 0xf6 ? K8 : osz;
      uint8_t sub = m.reg & 7;
      static const char *const group3[8] = { "test", "test", "not", "neg", "mul", "imul", "div", "idiv" };
      if (sub <= 1) {
        int64_t imm = kind == K8 ? x64_u8(r) : kind == K16 ? x64_u16(r) : (int32_t)x64_u32(r);
        if (!r->ok) return false;
        put(t, "test ");
        x64_rm(c, &m, kind, x64_size_name(kind));
        put(t, ", ");
        x64_number(t, x64_mask(kind, (uint64_t)imm));
        return true;
      }
      put(t, "%s ", group3[sub]);
      x64_rm(c, &m, kind, x64_size_name(kind));
      return true;
    }
    case 0xff: {
      x64_modrm_t m = x64_modrm(r);
      if (!r->ok) return false;
      uint8_t sub = m.reg & 7;
      if (sub != 2 && sub != 4) return false;
      put(t, "%s ", sub == 2 ? "call" : "jmp");
      x64_rm(c, &m, K64, "qword");
      return true;
    }
    default:
      return false;
  }
}

size_t asm_disasm_x64(const uint8_t *code, size_t len, size_t offset, asm_insn_t *out) {
  *out = (asm_insn_t){ .offset = offset };
  text_t t = text_of(out);
  x64_reader_t r = { .code = code, .len = len, .pos = offset, .ok = true };
  size_t rip_target = SIZE_MAX;
  x64_ctx_t c = { .r = &r, .t = &t, .rip_target = &rip_target };
  uint64_t constant = 0;
  bool has_constant = false;
  if (offset >= len || !x64_decode(&c, &constant, &has_constant) || !r.ok) {
    t = text_of(out);
    put(&t, ".byte %02x", offset < len ? code[offset] : 0);
    out->length = 1;
    return 0;
  }
  out->length = r.pos - offset;
  out->has_constant = has_constant;
  out->constant = constant;
  if (rip_target != SIZE_MAX) {
    out->has_pc_relative_target = true;
    out->pc_relative_target = rip_target;
  }
  return out->length;
}

static const char *const a64_conds[16] = {
  "eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv",
};
static const char *const a64_shifts[4] = { "lsl", "lsr", "asr", "ror" };
static const char *const a64_extends[8] = { "uxtb", "uxth", "uxtw", "uxtx", "sxtb", "sxth", "sxtw", "sxtx" };

typedef struct {
  char s[8];
} a64_name_t;

static a64_name_t a64_r(bool x, uint32_t reg, bool sp) {
  a64_name_t n;
  reg &= 31;
  if (reg == 31) snprintf(n.s, sizeof(n.s), "%s", sp ? (x ? "sp" : "wsp") : (x ? "xzr" : "wzr"));
  else snprintf(n.s, sizeof(n.s), "%c%u", x ? 'x' : 'w', reg);
  return n;
}

static a64_name_t a64_v(char kind, uint32_t reg) {
  a64_name_t n;
  snprintf(n.s, sizeof(n.s), "%c%u", kind, reg & 31);
  return n;
}

static void a64_imm(text_t *t, int64_t value) {
  if (value < 0) put(t, "#-0x%" PRIx64, (uint64_t)0 - (uint64_t)value);
  else put(t, "#0x%" PRIx64, (uint64_t)value);
}

static void a64_target(text_t *t, size_t offset, int64_t displacement) {
  put(t, "0x%" PRIx64, (uint64_t)((int64_t)offset + displacement));
}

static inline int64_t a64_sext(uint32_t value, unsigned bits) {
  return (int64_t)((uint64_t)value << (64 - bits)) >> (64 - bits);
}

static bool a64_decode_bit_masks(uint32_t n, uint32_t imms, uint32_t immr, bool x, uint64_t *out) {
  uint32_t combined = (n << 6) | (~imms & 0x3f);
  if (combined == 0) return false;
  unsigned length = 31u - (unsigned)__builtin_clz(combined);
  if (length < 1) return false;
  unsigned size = 1u << length;
  if (size > (x ? 64u : 32u)) return false;
  unsigned levels = size - 1;
  unsigned s = imms & levels, rot = immr & levels;
  if (s == levels) return false;
  uint64_t ones = s + 1 == 64 ? UINT64_MAX : ((uint64_t)1 << (s + 1)) - 1;
  uint64_t element;
  if (size == 64) element = rot ? (ones >> rot) | (ones << (64 - rot)) : ones;
  else {
    uint64_t mask = ((uint64_t)1 << size) - 1;
    element = ((ones >> rot) | (ones << ((size - rot) % size))) & mask;
  }
  uint64_t value = 0;
  for (unsigned position = 0; position < (x ? 64u : 32u); position += size) value |= element << position;
  *out = value;
  return true;
}

static bool a64_move_wide_preferred(bool sf, uint32_t n, uint32_t imms, uint32_t immr) {
  int s = (int)imms, r = (int)immr, width = sf ? 64 : 32;
  if (sf && n != 1) return false;
  if (!sf && (n != 0 || (imms & 0x20))) return false;
  if (s < 16) return ((16 - r) & 15) <= 15 - s;
  if (s >= width - 15) return (r & 15) <= s - (width - 15);
  return false;
}

static bool a64_bfx_preferred(bool sf, bool uns, uint32_t imms, uint32_t immr) {
  if (imms < immr) return false;
  if (imms == (sf ? 63u : 31u)) return false;
  if (immr == 0) {
    if (!sf && (imms == 7 || imms == 15)) return false;
    if (sf && !uns && (imms == 7 || imms == 15 || imms == 31)) return false;
  }
  return true;
}

static int64_t a64_signed_for(bool x, uint64_t value) {
  return x ? (int64_t)value : (int64_t)(int32_t)(uint32_t)value;
}

static void a64_fp_imm(text_t *t, uint32_t imm8) {
  uint64_t sign = imm8 >> 7, b = (imm8 >> 6) & 1;
  uint64_t exponent = ((b ^ 1) << 10) | (b ? 0xffu << 2 : 0) | ((imm8 >> 4) & 3);
  double value = asm_f64_from_bits((sign << 63) | (exponent << 52) | ((uint64_t)(imm8 & 0xf) << 48));
  char buf[40];
  for (int precision = 1; precision <= 17; precision++) {
    snprintf(buf, sizeof(buf), "%.*g", precision, value);
    if (strtod(buf, NULL) == value) break;
  }
  put(t, "#%s%s", buf, strpbrk(buf, ".e") ? "" : ".0");
}

static bool a64_load_store(text_t *t, uint32_t w) {
  bool unsigned_offset = (w & 0x3b000000) == 0x39000000;
  if (!unsigned_offset && (w & 0x3b000000) != 0x38000000) return false;
  uint32_t size = w >> 30, opc = (w >> 22) & 3;
  bool v = (w >> 26) & 1;
  const char *name = NULL, *uname = NULL;
  char kind = 'x';
  uint32_t scale = size;
  if (!v) {
    static const char *const names[4][4] = {
      { "strb", "ldrb", "ldrsb", "ldrsb" },
      { "strh", "ldrh", "ldrsh", "ldrsh" },
      { "str", "ldr", "ldrsw", NULL },
      { "str", "ldr", NULL, NULL },
    };
    static const char *const unames[4][4] = {
      { "sturb", "ldurb", "ldursb", "ldursb" },
      { "sturh", "ldurh", "ldursh", "ldursh" },
      { "stur", "ldur", "ldursw", NULL },
      { "stur", "ldur", NULL, NULL },
    };
    name = names[size][opc];
    uname = unames[size][opc];
    if (!name) return false;
    bool x = size == 3 || (opc == 2);
    kind = x ? 'x' : 'w';
  } else {
    if (size == 0 && opc >= 2) {
      kind = 'q';
      scale = 4;
      name = opc == 3 ? "ldr" : "str";
    } else if (size >= 2 && opc <= 1) {
      kind = size == 3 ? 'd' : 's';
      name = opc ? "ldr" : "str";
    } else {
      return false;
    }
    uname = name[0] == 'l' ? "ldur" : "stur";
  }
  a64_name_t rt = v ? a64_v(kind, w) : a64_r(kind == 'x', w, false);
  a64_name_t rn = a64_r(true, w >> 5, true);
  if (unsigned_offset) {
    uint64_t offset = (uint64_t)((w >> 10) & 0xfff) << scale;
    put(t, "%s %s, [%s", name, rt.s, rn.s);
    if (offset) put(t, ", #0x%" PRIx64, offset);
    put(t, "]");
    return true;
  }
  if ((w >> 21) & 1) {
    if (((w >> 10) & 3) != 2) return false;
    uint32_t option = (w >> 13) & 7;
    bool s = (w >> 12) & 1;
    if (!(option & 2)) return false;
    a64_name_t rm = a64_r(option & 1, w >> 16, false);
    put(t, "%s %s, [%s, %s", name, rt.s, rn.s, rm.s);
    if (option == 3) {
      if (s) put(t, ", lsl #0x%x", scale);
    } else {
      put(t, ", %s", a64_extends[option]);
      if (s) put(t, " #0x%x", scale);
    }
    put(t, "]");
    return true;
  }
  int64_t imm9 = a64_sext((w >> 12) & 0x1ff, 9);
  switch ((w >> 10) & 3) {
    case 0:
      put(t, "%s %s, [%s", uname, rt.s, rn.s);
      if (imm9) {
        put(t, ", ");
        a64_imm(t, imm9);
      }
      put(t, "]");
      return true;
    case 1:
      put(t, "%s %s, [%s], ", name, rt.s, rn.s);
      a64_imm(t, imm9);
      return true;
    case 3:
      put(t, "%s %s, [%s, ", name, rt.s, rn.s);
      a64_imm(t, imm9);
      put(t, "]!");
      return true;
    default:
      return false;
  }
}

static bool a64_pair(text_t *t, uint32_t w) {
  uint32_t opc = w >> 30;
  bool v = (w >> 26) & 1;
  if ((w & 0x3a000000) != 0x28000000) return false;
  uint32_t mode = (w >> 23) & 3;
  if (mode == 0) return false;
  char kind;
  if (!v && opc == 2) kind = 'x';
  else if (v && opc == 1) kind = 'd';
  else return false;
  bool load = (w >> 22) & 1;
  int64_t offset = a64_sext((w >> 15) & 0x7f, 7) * 8;
  a64_name_t r1 = kind == 'x' ? a64_r(true, w, false) : a64_v('d', w);
  a64_name_t r2 = kind == 'x' ? a64_r(true, w >> 10, false) : a64_v('d', w >> 10);
  a64_name_t rn = a64_r(true, w >> 5, true);
  put(t, "%s %s, %s, [%s", load ? "ldp" : "stp", r1.s, r2.s, rn.s);
  if (mode == 1) {
    put(t, "], ");
    a64_imm(t, offset);
  } else {
    if (offset || mode == 3) {
      put(t, ", ");
      a64_imm(t, offset);
    }
    put(t, mode == 3 ? "]!" : "]");
  }
  return true;
}

static bool a64_bitfield(text_t *t, uint32_t w) {
  bool sf = w >> 31;
  uint32_t opc = (w >> 29) & 3, n = (w >> 22) & 1;
  uint32_t immr = (w >> 16) & 0x3f, imms = (w >> 10) & 0x3f;
  uint32_t size = sf ? 64 : 32;
  if (opc == 1 || n != (uint32_t)sf || immr >= size || imms >= size) return false;
  a64_name_t rd = a64_r(sf, w, false), rn = a64_r(sf, w >> 5, false), wn = a64_r(false, w >> 5, false);
  bool uns = opc == 2;
  const char *pfx = uns ? "u" : "s";
  if (imms == size - 1) {
    put(t, "%s %s, %s, #0x%x", uns ? "lsr" : "asr", rd.s, rn.s, immr);
    return true;
  }
  if (uns && imms + 1 == immr) {
    put(t, "lsl %s, %s, #0x%x", rd.s, rn.s, size - 1 - imms);
    return true;
  }
  if (imms < immr) {
    put(t, "%sbfiz %s, %s, #0x%x, #0x%x", pfx, rd.s, rn.s, (size - immr) % size, imms + 1);
    return true;
  }
  if (a64_bfx_preferred(sf, uns, imms, immr)) {
    put(t, "%sbfx %s, %s, #0x%x, #0x%x", pfx, rd.s, rn.s, immr, imms - immr + 1);
    return true;
  }
  if (immr == 0 && (imms == 7 || imms == 15 || (!uns && imms == 31))) {
    const char *suffix = imms == 7 ? "b" : imms == 15 ? "h" : "w";
    put(t, "%sxt%s %s, %s", pfx, suffix, rd.s, wn.s);
    return true;
  }
  put(t, "%sbfm %s, %s, #0x%x, #0x%x", pfx, rd.s, rn.s, immr, imms);
  return true;
}

static bool a64_decode(text_t *t, uint32_t w, size_t offset) {
  bool sf = w >> 31;
  uint32_t rd_ = w & 31, rn_ = (w >> 5) & 31, rm_ = (w >> 16) & 31;

  if (w == 0xd503201f) { put(t, "nop"); return true; }
  if ((w & 0xffff0000) == 0) { put(t, "udf #0x%x", w & 0xffff); return true; }
  if ((w & 0xffe0001f) == 0xd4200000) { put(t, "brk #0x%x", (w >> 5) & 0xffff); return true; }
  if ((w & 0xfffffc1f) == 0xd61f0000) { put(t, "br %s", a64_r(true, rn_, false).s); return true; }
  if ((w & 0xfffffc1f) == 0xd63f0000) { put(t, "blr %s", a64_r(true, rn_, false).s); return true; }
  if ((w & 0xfffffc1f) == 0xd65f0000) {
    if (rn_ == 30) put(t, "ret");
    else put(t, "ret %s", a64_r(true, rn_, false).s);
    return true;
  }
  if ((w & 0x7c000000) == 0x14000000) {
    put(t, "%s ", (w >> 31) ? "bl" : "b");
    a64_target(t, offset, a64_sext(w & 0x3ffffff, 26) * 4);
    return true;
  }
  if ((w & 0xff000010) == 0x54000000) {
    put(t, "b.%s ", a64_conds[w & 15]);
    a64_target(t, offset, a64_sext((w >> 5) & 0x7ffff, 19) * 4);
    return true;
  }
  if ((w & 0x7e000000) == 0x34000000) {
    put(t, "%s %s, ", (w >> 24) & 1 ? "cbnz" : "cbz", a64_r(sf, rd_, false).s);
    a64_target(t, offset, a64_sext((w >> 5) & 0x7ffff, 19) * 4);
    return true;
  }
  if ((w & 0x7e000000) == 0x36000000) {
    uint32_t bit = ((w >> 31) << 5) | ((w >> 19) & 31);
    put(t, "%s %s, #0x%x, ", (w >> 24) & 1 ? "tbnz" : "tbz", a64_r(bit >= 32, rd_, false).s, bit);
    a64_target(t, offset, a64_sext((w >> 5) & 0x3fff, 14) * 4);
    return true;
  }
  if ((w & 0x9f000000) == 0x10000000) {
    int64_t imm = a64_sext(((w >> 5) & 0x7ffff) << 2 | ((w >> 29) & 3), 21);
    put(t, "adr %s, ", a64_r(true, rd_, false).s);
    a64_target(t, offset, imm);
    return true;
  }
  if ((w & 0xff000000) == 0x58000000 || (w & 0xff000000) == 0x5c000000 || (w & 0xff000000) == 0x9c000000) {
    a64_name_t rt = (w & 0xff000000) == 0x58000000 ? a64_r(true, rd_, false)
      : a64_v((w & 0xff000000) == 0x5c000000 ? 'd' : 'q', rd_);
    put(t, "ldr %s, ", rt.s);
    a64_target(t, offset, a64_sext((w >> 5) & 0x7ffff, 19) * 4);
    return true;
  }

  if ((w & 0x1f800000) == 0x12800000) {
    uint32_t opc = (w >> 29) & 3, hw = (w >> 21) & 3;
    uint64_t imm16 = (w >> 5) & 0xffff;
    if (opc == 1 || (!sf && hw >= 2)) return false;
    a64_name_t rd = a64_r(sf, rd_, false);
    if (opc == 3) {
      put(t, "movk %s, #0x%" PRIx64, rd.s, imm16);
      if (hw) put(t, ", lsl #0x%x", hw * 16);
      return true;
    }
    bool alias = !(imm16 == 0 && hw != 0) && !(opc == 0 && !sf && imm16 == 0xffff);
    if (alias) {
      uint64_t value = imm16 << (hw * 16);
      if (opc == 0) value = ~value;
      put(t, "mov %s, ", rd.s);
      a64_imm(t, a64_signed_for(sf, value));
    } else {
      put(t, "%s %s, #0x%" PRIx64, opc == 0 ? "movn" : "movz", rd.s, imm16);
      if (hw) put(t, ", lsl #0x%x", hw * 16);
    }
    return true;
  }

  if ((w & 0x1f000000) == 0x11000000) {
    bool sub = (w >> 30) & 1, s = (w >> 29) & 1, sh = (w >> 22) & 1;
    if ((w >> 23) & 1) return false;
    uint32_t imm12 = (w >> 10) & 0xfff;
    a64_name_t rd = a64_r(sf, rd_, !s), rn = a64_r(sf, rn_, true);
    if (s && rd_ == 31) put(t, "%s %s, #0x%x", sub ? "cmp" : "cmn", rn.s, imm12);
    else if (!sub && !s && !sh && imm12 == 0 && (rd_ == 31 || rn_ == 31)) {
      put(t, "mov %s, %s", rd.s, rn.s);
      return true;
    } else put(t, "%s%s %s, %s, #0x%x", sub ? "sub" : "add", s ? "s" : "", rd.s, rn.s, imm12);
    if (sh) put(t, ", lsl #0xc");
    return true;
  }

  if ((w & 0x1f800000) == 0x12000000) {
    uint32_t opc = (w >> 29) & 3, n = (w >> 22) & 1;
    uint64_t imm;
    if (!sf && n) return false;
    if (!a64_decode_bit_masks(n, (w >> 10) & 0x3f, (w >> 16) & 0x3f, sf, &imm)) return false;
    a64_name_t rd = a64_r(sf, rd_, opc != 3), rn = a64_r(sf, rn_, false);
    if (opc == 1 && rn_ == 31 && !a64_move_wide_preferred(sf, n, (w >> 10) & 0x3f, (w >> 16) & 0x3f)) {
      put(t, "mov %s, ", rd.s);
      a64_imm(t, a64_signed_for(sf, imm));
      return true;
    }
    if (opc == 3 && rd_ == 31) {
      put(t, "tst %s, #0x%" PRIx64, rn.s, imm);
      return true;
    }
    static const char *const names[4] = { "and", "orr", "eor", "ands" };
    put(t, "%s %s, %s, #0x%" PRIx64, names[opc], rd.s, rn.s, imm);
    return true;
  }

  if ((w & 0x1f800000) == 0x13000000) return a64_bitfield(t, w);

  if ((w & 0x1f000000) == 0x0a000000) {
    uint32_t opc = (w >> 29) & 3, shift = (w >> 22) & 3, amount = (w >> 10) & 0x3f;
    bool n = (w >> 21) & 1;
    if (!sf && amount >= 32) return false;
    a64_name_t rd = a64_r(sf, rd_, false), rn = a64_r(sf, rn_, false), rm = a64_r(sf, rm_, false);
    static const char *const names[2][4] = { { "and", "orr", "eor", "ands" }, { "bic", "orn", "eon", "bics" } };
    if (opc == 1 && !n && rn_ == 31 && shift == 0 && amount == 0) {
      put(t, "mov %s, %s", rd.s, rm.s);
      return true;
    }
    if (opc == 1 && n && rn_ == 31) put(t, "mvn %s, %s", rd.s, rm.s);
    else if (opc == 3 && !n && rd_ == 31) put(t, "tst %s, %s", rn.s, rm.s);
    else put(t, "%s %s, %s, %s", names[n][opc], rd.s, rn.s, rm.s);
    if (shift || amount) put(t, ", %s #0x%x", a64_shifts[shift], amount);
    return true;
  }

  if ((w & 0x1f200000) == 0x0b000000) {
    bool sub = (w >> 30) & 1, s = (w >> 29) & 1;
    uint32_t shift = (w >> 22) & 3, amount = (w >> 10) & 0x3f;
    if (shift == 3 || (!sf && amount >= 32)) return false;
    a64_name_t rd = a64_r(sf, rd_, false), rn = a64_r(sf, rn_, false), rm = a64_r(sf, rm_, false);
    if (s && rd_ == 31) put(t, "%s %s, %s", sub ? "cmp" : "cmn", rn.s, rm.s);
    else if (sub && rn_ == 31) put(t, "%s %s, %s", s ? "negs" : "neg", rd.s, rm.s);
    else put(t, "%s%s %s, %s, %s", sub ? "sub" : "add", s ? "s" : "", rd.s, rn.s, rm.s);
    if (shift || amount) put(t, ", %s #0x%x", a64_shifts[shift], amount);
    return true;
  }

  if ((w & 0x1fe00000) == 0x0b200000) {
    bool sub = (w >> 30) & 1, s = (w >> 29) & 1;
    uint32_t option = (w >> 13) & 7, amount = (w >> 10) & 7;
    if (amount > 4) return false;
    a64_name_t rd = a64_r(sf, rd_, !s), rn = a64_r(sf, rn_, true);
    a64_name_t rm = a64_r(sf && (option & 3) == 3, rm_, false);
    if (s && rd_ == 31) put(t, "%s %s, %s", sub ? "cmp" : "cmn", rn.s, rm.s);
    else put(t, "%s%s %s, %s, %s", sub ? "sub" : "add", s ? "s" : "", rd.s, rn.s, rm.s);
    bool sp_involved = rn_ == 31 || (!s && rd_ == 31);
    if (sp_involved && option == (sf ? 3u : 2u)) {
      if (amount) put(t, ", lsl #0x%x", amount);
    } else {
      put(t, ", %s", a64_extends[option]);
      if (amount) put(t, " #0x%x", amount);
    }
    return true;
  }

  if ((w & 0x7fe00000) == 0x1ac00000) {
    uint32_t opcode = (w >> 10) & 0x3f;
    const char *name;
    switch (opcode) {
      case 0x02: name = "udiv"; break;
      case 0x03: name = "sdiv"; break;
      case 0x08: name = "lsl"; break;
      case 0x09: name = "lsr"; break;
      case 0x0a: name = "asr"; break;
      case 0x0b: name = "ror"; break;
      default: return false;
    }
    put(t, "%s %s, %s, %s", name, a64_r(sf, rd_, false).s, a64_r(sf, rn_, false).s, a64_r(sf, rm_, false).s);
    return true;
  }

  if ((w & 0x1f000000) == 0x1b000000) {
    uint32_t op31 = (w >> 21) & 7, ra_ = (w >> 10) & 31;
    bool o0 = (w >> 15) & 1;
    a64_name_t rd = a64_r(sf, rd_, false), ra = a64_r(sf, ra_, false);
    if (op31 == 0) {
      a64_name_t rn = a64_r(sf, rn_, false), rm = a64_r(sf, rm_, false);
      if (ra_ == 31) put(t, "%s %s, %s, %s", o0 ? "mneg" : "mul", rd.s, rn.s, rm.s);
      else put(t, "%s %s, %s, %s, %s", o0 ? "msub" : "madd", rd.s, rn.s, rm.s, ra.s);
      return true;
    }
    if (!sf) return false;
    if (op31 == 2 || op31 == 6) {
      if (o0 || ra_ != 31) return false;
      put(t, "%s %s, %s, %s", op31 == 2 ? "smulh" : "umulh", rd.s, a64_r(true, rn_, false).s, a64_r(true, rm_, false).s);
      return true;
    }
    if (op31 == 1 || op31 == 5) {
      const char *u = op31 == 5 ? "u" : "s";
      a64_name_t rn = a64_r(false, rn_, false), rm = a64_r(false, rm_, false);
      if (ra_ == 31) put(t, "%s%s %s, %s, %s", u, o0 ? "mnegl" : "mull", rd.s, rn.s, rm.s);
      else put(t, "%s%s %s, %s, %s, %s", u, o0 ? "msubl" : "maddl", rd.s, rn.s, rm.s, ra.s);
      return true;
    }
    return false;
  }

  if ((w & 0x3fe00800) == 0x1a800000) {
    uint32_t op = (w >> 30) & 1, o2 = (w >> 10) & 1, cond = (w >> 12) & 15;
    a64_name_t rd = a64_r(sf, rd_, false), rn = a64_r(sf, rn_, false), rm = a64_r(sf, rm_, false);
    static const char *const names[2][2] = { { "csel", "csinc" }, { "csinv", "csneg" } };
    if ((op || o2) && (cond >> 1) != 7) {
      const char *invert = a64_conds[cond ^ 1];
      if (o2 && !op && rn_ == 31 && rm_ == 31) { put(t, "cset %s, %s", rd.s, invert); return true; }
      if (op && !o2 && rn_ == 31 && rm_ == 31) { put(t, "csetm %s, %s", rd.s, invert); return true; }
      if (rn_ == rm_ && rn_ != 31) {
        static const char *const aliases[2][2] = { { "", "cinc" }, { "cinv", "cneg" } };
        put(t, "%s %s, %s, %s", aliases[op][o2], rd.s, rn.s, invert);
        return true;
      }
    }
    put(t, "%s %s, %s, %s, %s", names[op][o2], rd.s, rn.s, rm.s, a64_conds[cond]);
    return true;
  }

  if ((w & 0x3b000000) == 0x38000000 || (w & 0x3b000000) == 0x39000000) return a64_load_store(t, w);
  if ((w & 0x3a000000) == 0x28000000) return a64_pair(t, w);

  a64_name_t vd = a64_v('d', rd_), vn = a64_v('d', rn_), vm = a64_v('d', rm_);
  switch (w & 0xfffffc00) {
    case 0x1e604000: put(t, "fmov %s, %s", vd.s, vn.s); return true;
    case 0x9e670000: put(t, "fmov %s, %s", vd.s, a64_r(true, rn_, false).s); return true;
    case 0x9e660000: put(t, "fmov %s, %s", a64_r(true, rd_, false).s, vn.s); return true;
    case 0x1e61c000: put(t, "fsqrt %s, %s", vd.s, vn.s); return true;
    case 0x1e654000: put(t, "frintm %s, %s", vd.s, vn.s); return true;
    case 0x1e64c000: put(t, "frintp %s, %s", vd.s, vn.s); return true;
    case 0x1e65c000: put(t, "frintz %s, %s", vd.s, vn.s); return true;
    case 0x1e614000: put(t, "fneg %s, %s", vd.s, vn.s); return true;
    case 0x1e60c000: put(t, "fabs %s, %s", vd.s, vn.s); return true;
    case 0x1e22c000: put(t, "fcvt %s, %s", vd.s, a64_v('s', rn_).s); return true;
    case 0x1e624000: put(t, "fcvt %s, %s", a64_v('s', rd_).s, vn.s); return true;
    case 0x1e620000:
    case 0x9e620000: put(t, "scvtf %s, %s", vd.s, a64_r(sf, rn_, false).s); return true;
    case 0x1e780000:
    case 0x9e780000: put(t, "fcvtzs %s, %s", a64_r(sf, rd_, false).s, vn.s); return true;
    case 0x1e7e0000: put(t, "fjcvtzs %s, %s", a64_r(false, rd_, false).s, vn.s); return true;
    default: break;
  }
  if ((w & 0xffe01fe0) == 0x1e601000) {
    put(t, "fmov %s, ", vd.s);
    a64_fp_imm(t, (w >> 13) & 0xff);
    return true;
  }
  if ((w & 0xffe0fc1f) == 0x1e602000) { put(t, "fcmp %s, %s", vn.s, vm.s); return true; }
  if ((w & 0xfffffc1f) == 0x1e602008) { put(t, "fcmp %s, #0.0", vn.s); return true; }
  switch (w & 0xffe0fc00) {
    case 0x1e602800: put(t, "fadd %s, %s, %s", vd.s, vn.s, vm.s); return true;
    case 0x1e603800: put(t, "fsub %s, %s, %s", vd.s, vn.s, vm.s); return true;
    case 0x1e600800: put(t, "fmul %s, %s, %s", vd.s, vn.s, vm.s); return true;
    case 0x1e601800: put(t, "fdiv %s, %s, %s", vd.s, vn.s, vm.s); return true;
    case 0x1e605800: put(t, "fmin %s, %s, %s", vd.s, vn.s, vm.s); return true;
    case 0x1e604800: put(t, "fmax %s, %s, %s", vd.s, vn.s, vm.s); return true;
    default: break;
  }
  return false;
}

size_t asm_disasm_a64(const uint8_t *code, size_t len, size_t offset, asm_insn_t *out) {
  *out = (asm_insn_t){ .offset = offset, .length = 4 };
  text_t t = text_of(out);
  if (offset + 4 > len) {
    out->length = len > offset ? len - offset : 0;
    put(&t, ".byte");
    for (size_t i = offset; i < len; i++) put(&t, " %02x", code[i]);
    return 0;
  }
  uint32_t w = read_u32_le(code + offset);
  if (!a64_decode(&t, w, offset)) {
    t = text_of(out);
    put(&t, ".word 0x%08x", w);
    return 0;
  }
  uint32_t top = w & 0xff000000;
  if (top == 0x58000000 || top == 0x5c000000) {
    out->has_pc_relative_target = true;
    out->pc_relative_target = (size_t)((int64_t)offset + 4 * a64_sext((w >> 5) & 0x7ffff, 19));
  }
  return 4;
}

static asm_insn_t *listing_push(asm_listing_t *l) {
  if (l->count == l->cap) {
    size_t cap = l->cap ? l->cap * 2 : 32;
    asm_insn_t *items = realloc(l->items, cap * sizeof(*items));
    if (!items) asm_panic("out of memory");
    l->items = items;
    l->cap = cap;
  }
  return &l->items[l->count++];
}

void asm_listing_with_data(asm_listing_t *out, asm_arch_t arch, const uint8_t *code, size_t len, size_t data_offset) {
  *out = (asm_listing_t){ 0 };
  if (data_offset > len) data_offset = len;
  uint64_t values[32];
  bool known[32] = { false };
  for (size_t offset = 0; offset < data_offset;) {
    asm_insn_t *insn = listing_push(out);
    if (arch == ASM_ARCH_X64) {
      asm_disasm_x64(code, data_offset, offset, insn);
      offset += insn->length;
      continue;
    }
    asm_disasm_a64(code, data_offset, offset, insn);
    if (insn->length == 4) {
      uint32_t w = read_u32_le(code + offset);
      uint32_t reg = w & 31, shift = 16 * ((w >> 21) & 3);
      uint64_t immediate = (uint64_t)((w >> 5) & 0xffff) << shift;
      if ((w & 0xff800000) == 0xd2800000) {
        values[reg] = immediate;
        known[reg] = true;
        insn->has_constant = true;
        insn->constant = immediate;
      } else if ((w & 0xff800000) == 0xf2800000 && known[reg]) {
        values[reg] = (values[reg] & ~((uint64_t)0xffff << shift)) | immediate;
        insn->has_constant = true;
        insn->constant = values[reg];
      }
    }
    offset += insn->length ? insn->length : 1;
  }
  for (size_t i = 0; i < out->count; i++) {
    asm_insn_t *insn = &out->items[i];
    size_t target = insn->pc_relative_target;
    if (insn->has_constant || !insn->has_pc_relative_target) continue;
    if (target >= data_offset && target <= len && len - target >= 8) {
      insn->has_constant = true;
      insn->constant = read_u64_le(code + target);
    }
  }
  for (size_t offset = (data_offset + 7) & ~(size_t)7; offset + 8 <= len; offset += 8) {
    asm_insn_t *insn = listing_push(out);
    *insn = (asm_insn_t){ .offset = offset, .length = 8, .has_constant = true, .constant = read_u64_le(code + offset) };
    snprintf(insn->text, sizeof(insn->text), ".quad 0x%" PRIx64, insn->constant);
  }
}

void asm_listing_free(asm_listing_t *listing) {
  free(listing->items);
  *listing = (asm_listing_t){ 0 };
}

void asm_listing_print(FILE *out, const asm_listing_t *listing) {
  for (size_t i = 0; i < listing->count; i++) {
    const asm_insn_t *insn = &listing->items[i];
    fprintf(out, "%6zx  %s", insn->offset, insn->text);
    if (insn->has_constant && insn->text[0] != '.') fprintf(out, "  ; 0x%" PRIx64, insn->constant);
    fputc('\n', out);
  }
}
