#include "cli/misc.h"

#include <stdio.h>
#include <argtable3.h>
#include <crprintf.h>

void print_flag(FILE *fp, flag_help_t f) {
  const char *s = f.s, *l = f.l, *d = f.d, *g = f.g;
  int opt = f.opt;

  char syn[200];
  crsprintf(syn, sizeof(syn),
    "<cyan>%s</cyan>%s<cyan>%s%s%s%s</cyan>",
    s             ? (char[]){'-', s[0], '\0'}  : (l ? "    " : ""),
    s && l        ? ", "                       : "",
    l             ? "--"                       : "",
    l             ? l                          : "",
    d && l && opt ? "[="  :
    d && l        ? "="   :
    d && s        ? " "   :                      "",
    d             ? d                          : "");

  crfprintf(fp, "<space=2/><pad=32>%s</pad> %s</>\n", syn, g);
}

void print_flags_help(FILE *fp, void **argtable) {
  struct arg_hdr **table = (struct arg_hdr **)argtable;
  for (int i = 0; !(table[i]->flag & ARG_TERMINATOR); i++) {
    struct arg_hdr *hdr = table[i];
    if (!hdr->glossary) continue;
    print_flag(fp, (flag_help_t){
      .s = hdr->shortopts, .l = hdr->longopts,
      .d = hdr->datatype,  .g = hdr->glossary,
      .opt = hdr->flag & ARG_HASOPTVALUE,
    });
  }
}