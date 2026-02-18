#ifndef MISC_H
#define MISC_H

#include <stdio.h>
struct arg_end;

typedef struct {
  const char *s;
  const char *l;
  const char *d;
  const char *g;
  int opt;
} flag_help_t;

void print_flags_help(FILE *fp, void **argtable);
void print_flag(FILE *fp, flag_help_t f);
void print_errors(FILE *fp, struct arg_end *end);

#endif