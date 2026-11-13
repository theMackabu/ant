#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
  size_t len;
  char text[1024];
} ant_hvf_dump_t;

static inline void ant_hvf_dump_add(ant_hvf_dump_t *dump, const char *name, uint64_t value) {
  if (dump->len >= sizeof(dump->text)) return;
  int written = snprintf(
    dump->text + dump->len, sizeof(dump->text) - dump->len,
    " %s=0x%llx", name, (unsigned long long)value
  );
  if (written > 0) dump->len += (size_t)written;
}

static inline void ant_hvf_dump_report(const ant_hvf_dump_t *dump, const char *reason) {
  fprintf(stderr, "sandbox vm: %s%s\n", reason, dump->text);
}
