#ifndef CPRINTF_H
#define CPRINTF_H

#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>

extern bool io_no_color;
typedef struct program_t program_t;

program_t *cprintf_compile(const char *fmt);
int cprintf_exec(program_t *prog, FILE *stream, ...);
int csprintf_inner(program_t *prog, char *buf, size_t size, ...);

#define cprintf(fmt, ...) ({ \
  static program_t *_cp_prog_ = NULL; \
  if (!_cp_prog_) _cp_prog_ = cprintf_compile(fmt); \
  cprintf_exec(_cp_prog_, stdout, ##__VA_ARGS__); \
})

#define cfprintf(stream, fmt, ...) ({ \
  static program_t *_cp_prog_ = NULL; \
  if (!_cp_prog_) _cp_prog_ = cprintf_compile(fmt); \
  cprintf_exec(_cp_prog_, stream, ##__VA_ARGS__); \
})

#define csprintf(buf, size, fmt, ...) ({ \
  static program_t *_cp_prog_ = NULL; \
  if (!_cp_prog_) _cp_prog_ = cprintf_compile(fmt); \
  csprintf_inner(_cp_prog_, buf, size, ##__VA_ARGS__); \
})

#endif
