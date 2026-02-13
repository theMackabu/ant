#ifndef CRPRINTF_H
#define CRPRINTF_H

#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>

extern bool io_no_color;
extern bool crprintf_debug;
extern bool crprintf_debug_hex;

typedef struct program_t program_t;
program_t *crprintf_compile(const char *fmt);

void crprintf_var(const char *name, const char *value);
void crprintf_hexdump(program_t *prog, FILE *out);
void crprintf_disasm(program_t *prog, FILE *out);

int crprintf_exec(program_t *prog, FILE *stream, ...);
int crsprintf_inner(program_t *prog, char *buf, size_t size, ...);

#define _CRPRINTF_INIT(prog, fmt) \
  if (!prog) { \
    prog = crprintf_compile(fmt); \
    if (crprintf_debug) crprintf_disasm(prog, stderr); \
    if (crprintf_debug_hex) crprintf_hexdump(prog, stderr); \
  }
  
#define crprintf(fmt, ...) ({ \
  static program_t *_cp_prog_ = NULL; \
  _CRPRINTF_INIT(_cp_prog_, fmt); \
  crprintf_exec(_cp_prog_, stdout, ##__VA_ARGS__); \
})

#define crfprintf(stream, fmt, ...) ({ \
  static program_t *_cp_prog_ = NULL; \
  _CRPRINTF_INIT(_cp_prog_, fmt); \
  crprintf_exec(_cp_prog_, stream, ##__VA_ARGS__); \
})

#define crsprintf(buf, size, fmt, ...) ({ \
  static program_t *_cp_prog_ = NULL; \
  _CRPRINTF_INIT(_cp_prog_, fmt); \
  crsprintf_inner(_cp_prog_, buf, size, ##__VA_ARGS__); \
})

#endif
