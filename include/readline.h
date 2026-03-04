#ifndef ANT_READLINE_H
#define ANT_READLINE_H

#include <stdbool.h>
#include "highlight.h"

typedef struct {
  char **lines;
  int count;
  int capacity;
  int current;
} ant_history_t;

typedef enum {
  ANT_READLINE_LINE,
  ANT_READLINE_EOF,
  ANT_READLINE_INTERRUPT,
} ant_readline_result_t;

void ant_readline_install_signal_handler(void);
void ant_readline_shutdown(void);

void ant_history_init(ant_history_t *hist, int capacity);
void ant_history_add(ant_history_t *hist, const char *line);
void ant_history_load(ant_history_t *hist);
void ant_history_save(const ant_history_t *hist);
void ant_history_free(ant_history_t *hist);

const char *ant_history_prev(ant_history_t *hist);
const char *ant_history_next(ant_history_t *hist);

ant_readline_result_t ant_readline(
  ant_history_t *hist,
  const char *prompt,
  highlight_state line_state,
  char **out_line
);

#endif
