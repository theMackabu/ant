#ifndef ANT_READLINE_INTERNAL_H
#define ANT_READLINE_INTERNAL_H

#include <ctype.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <conio.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#define STDIN_FILENO 0
#define mkdir_p(path) _mkdir(path)
#else
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#define mkdir_p(path) mkdir(path, 0755)
#endif

#include "highlight.h"
#include <crprintf.h>
#include <compat.h> // IWYU pragma: keep

#define MAX_LINE_LENGTH 4096
#define MAX_PREVIEW_LENGTH 1024


typedef enum {
  KEY_NONE, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
  KEY_HOME, KEY_END, KEY_DELETE, KEY_BACKSPACE, 
  KEY_ENTER, KEY_EOF, KEY_TAB, KEY_CHAR
} key_type_t;

typedef struct {
  key_type_t type;
  int ch;
} key_event_t;

int repl_terminal_cols(void);
key_event_t repl_read_key(void);
extern volatile sig_atomic_t ctrl_c_pressed;

void repl_jump_cursor(int x_pos, int y_offset);
void repl_clear_to_end(void);
void repl_set_cursor_visible(bool visible);
void repl_enable_raw_mode(void);
void repl_term_restore(void);
void repl_render_shutdown(void);
void repl_render_set_highlight_state(highlight_state state);
void repl_render_reset_cursor_row(void);
void move_cursor_only(const char *line, int pos, const char *prompt);

void refresh_line_with_preview(
  const char *line,
  int len, int pos,
  const char *prompt,
  const char *suffix,
  const char *preview
);

#endif
