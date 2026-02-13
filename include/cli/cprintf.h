#ifndef CPRINTF_H
#define CPRINTF_H

#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>

extern bool io_no_color;
extern bool cprintf_debug;
extern bool cprintf_debug_hex;

typedef struct program_t program_t;
program_t *cprintf_compile(const char *fmt);

void cprintf_hexdump(program_t *prog, FILE *out);
void cprintf_disasm(program_t *prog, FILE *out);

int cprintf_exec(program_t *prog, FILE *stream, ...);
int csprintf_inner(program_t *prog, char *buf, size_t size, ...);

#define _CPRINTF_INIT(prog, fmt) \
  if (!prog) { \
    prog = cprintf_compile(fmt); \
    if (cprintf_debug) cprintf_disasm(prog, stderr); \
    if (cprintf_debug_hex) cprintf_hexdump(prog, stderr); \
  }
  
#define cprintf(fmt, ...) ({ \
  static program_t *_cp_prog_ = NULL; \
  _CPRINTF_INIT(_cp_prog_, fmt); \
  cprintf_exec(_cp_prog_, stdout, ##__VA_ARGS__); \
})

#define cfprintf(stream, fmt, ...) ({ \
  static program_t *_cp_prog_ = NULL; \
  _CPRINTF_INIT(_cp_prog_, fmt); \
  cprintf_exec(_cp_prog_, stream, ##__VA_ARGS__); \
})

#define csprintf(buf, size, fmt, ...) ({ \
  static program_t *_cp_prog_ = NULL; \
  _CPRINTF_INIT(_cp_prog_, fmt); \
  csprintf_inner(_cp_prog_, buf, size, ##__VA_ARGS__); \
})

#endif
