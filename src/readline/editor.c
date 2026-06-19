#include "readline.h"
#include "utf8.h"
#include "readline_internal.h"

typedef struct {
  ant_readline_preview_fn fn;
  void *ctx;
  char suffix[MAX_PREVIEW_LENGTH];
  char text[MAX_PREVIEW_LENGTH];
} preview_state_t;

static int utf8_prev_pos(const char *line, int pos) {
  if (pos <= 0) return 0;
  int prev = 0;
  int i = 0;
  while (i < pos) {
    prev = i;
    utf8proc_int32_t cp = 0;
    utf8proc_ssize_t n = utf8_next(
      (const utf8proc_uint8_t *)(line + i),
      (utf8proc_ssize_t)(pos - i),
      &cp
    );
    i += (int)((n > 0) ? n : 1);
  }
  return prev;
}

static int utf8_next_pos(const char *line, int len, int pos) {
  if (pos >= len) return len;
  utf8proc_int32_t cp = 0;
  utf8proc_ssize_t n = utf8_next(
    (const utf8proc_uint8_t *)(line + pos),
    (utf8proc_ssize_t)(len - pos),
    &cp
  );
  int next = pos + (int)((n > 0) ? n : 1);
  return (next > len) ? len : next;
}

static void preview_update(preview_state_t *preview, const char *line, int len) {
  if (!preview || !preview->fn) return;
  preview->suffix[0] = '\0';
  preview->text[0] = '\0';
  if (len <= 0) return;
  if (!preview->fn(
    preview->ctx,
    line,
    (size_t)len,
    preview->suffix,
    sizeof(preview->suffix),
    preview->text,
    sizeof(preview->text)
  )) {
    preview->suffix[0] = '\0';
    preview->text[0] = '\0';
  }
}

static const char *preview_text(preview_state_t *preview) {
  return (preview && preview->text[0]) ? preview->text : NULL;
}

static const char *preview_suffix(preview_state_t *preview) {
  return (preview && preview->suffix[0]) ? preview->suffix : NULL;
}

static bool preview_clear(preview_state_t *preview) {
  if (!preview) return false;
  bool had_preview = preview->suffix[0] != '\0' || preview->text[0] != '\0';
  preview->suffix[0] = '\0';
  preview->text[0] = '\0';
  return had_preview;
}

static void line_set(
  char *line,
  int *pos,
  int *len,
  const char *str,
  const char *prompt,
  preview_state_t *preview
) {
  size_t n = strlen(str);
  if (n >= (size_t)MAX_LINE_LENGTH) n = (size_t)MAX_LINE_LENGTH - 1;
  memcpy(line, str, n);
  line[n] = '\0';
  *len = (int)strlen(line);
  *pos = *len;
  preview_update(preview, line, *len);
  refresh_line_with_preview(
    line, *len, *pos, prompt,
    preview_suffix(preview), preview_text(preview)
  );
}

static void line_backspace(
  char *line,
  int *pos,
  int *len,
  const char *prompt,
  preview_state_t *preview
) {
  if (*pos <= 0) return;

  int prev = utf8_prev_pos(line, *pos);
  memmove(line + prev, line + *pos, (size_t)(*len - *pos + 1));
  *len -= (*pos - prev);
  *pos = prev;
  preview_update(preview, line, *len);
  refresh_line_with_preview(
    line, *len, *pos, prompt,
    preview_suffix(preview), preview_text(preview)
  );
}

static void line_delete(
  char *line,
  int *pos,
  int *len,
  const char *prompt,
  preview_state_t *preview
) {
  if (*pos >= *len) return;
  int next = utf8_next_pos(line, *len, *pos);
  memmove(line + *pos, line + next, (size_t)(*len - next + 1));
  *len -= (next - *pos);
  preview_update(preview, line, *len);
  refresh_line_with_preview(
    line, *len, *pos, prompt,
    preview_suffix(preview), preview_text(preview)
  );
}

static void line_insert(
  char *line,
  int *pos,
  int *len,
  int c,
  const char *prompt,
  preview_state_t *preview
) {
  if (*len >= MAX_LINE_LENGTH - 1) return;

  memmove(line + *pos + 1, line + *pos, (size_t)(*len - *pos + 1));
  line[*pos] = (char)c;
  (*pos)++;
  (*len)++;
  preview_update(preview, line, *len);
  refresh_line_with_preview(
    line, *len, *pos, prompt,
    preview_suffix(preview), preview_text(preview)
  );
}

static void move_or_refresh_without_preview(
  char *line,
  int len,
  int pos,
  const char *prompt,
  preview_state_t *preview
) {
  if (preview_clear(preview))
    refresh_line_with_preview(line, len, pos, prompt, NULL, NULL);
  else move_cursor_only(line, pos, prompt);
}

static char *read_line_with_history(
  ant_history_t *hist,
  const char *prompt,
  ant_readline_preview_fn preview_fn,
  void *preview_ctx
) {
  char *line = malloc(MAX_LINE_LENGTH);
  if (!line) return NULL;
  int pos = 0;
  int len = 0;
  line[0] = '\0';
  preview_state_t preview = {
    .fn = preview_fn,
    .ctx = preview_ctx,
    .suffix = {0},
    .text = {0},
  };
  repl_render_reset_cursor_row();
  repl_enable_raw_mode();

  for (;;) {
    key_event_t key = repl_read_key();
    static const void *dispatch[] = {
      [KEY_NONE] = &&l_none,
      [KEY_UP] = &&l_up,
      [KEY_DOWN] = &&l_down,
      [KEY_LEFT] = &&l_left,
      [KEY_RIGHT] = &&l_right,
      [KEY_HOME] = &&l_home,
      [KEY_END] = &&l_end,
      [KEY_DELETE] = &&l_delete,
      [KEY_BACKSPACE] = &&l_backspace,
      [KEY_ENTER] = &&l_enter,
      [KEY_EOF] = &&l_eof,
      [KEY_CHAR] = &&l_char,
    };

    unsigned int t = (unsigned int)key.type;
    if (t < (sizeof(dispatch) / sizeof(*dispatch)) && dispatch[t]) goto *dispatch[t];
    goto l_none;

  l_enter:
    preview_clear(&preview);
    refresh_line_with_preview(line, len, len, prompt, NULL, NULL);
    putchar('\n');
    repl_term_restore();
    return line;

  l_eof:
    refresh_line_with_preview(line, len, pos, prompt, NULL, NULL);
    putchar('\n');
    repl_term_restore();
    free(line);
    return NULL;

  l_up: {
    const char *h = ant_history_prev(hist);
    if (h) line_set(line, &pos, &len, h, prompt, &preview);
    continue;
  }

  l_down: {
    const char *h = ant_history_next(hist);
    if (h) line_set(line, &pos, &len, h, prompt, &preview);
    continue;
  }

  l_left:
    if (pos > 0) {
      pos = utf8_prev_pos(line, pos);
      move_or_refresh_without_preview(line, len, pos, prompt, &preview);
    }
    continue;

  l_right:
    if (pos < len) {
      pos = utf8_next_pos(line, len, pos);
      move_or_refresh_without_preview(line, len, pos, prompt, &preview);
    }
    continue;

  l_home:
    if (pos != 0) {
      pos = 0;
      move_or_refresh_without_preview(line, len, pos, prompt, &preview);
    }
    continue;

  l_end:
    if (pos != len) {
      pos = len;
      move_or_refresh_without_preview(line, len, pos, prompt, &preview);
    }
    continue;

  l_delete:
    line_delete(line, &pos, &len, prompt, &preview);
    continue;

  l_backspace:
    line_backspace(line, &pos, &len, prompt, &preview);
    continue;

  l_char:
    line_insert(line, &pos, &len, key.ch, prompt, &preview);
    continue;

  l_none:
    continue;
  }
}

ant_readline_result_t ant_readline_with_preview(
  ant_history_t *hist,
  const char *prompt,
  highlight_state line_state,
  ant_readline_preview_fn preview_fn,
  void *preview_ctx,
  char **out_line
) {
  if (out_line) *out_line = NULL;

  repl_render_set_highlight_state(line_state);
  ctrl_c_pressed = 0;
  char *line = read_line_with_history(hist, prompt, preview_fn, preview_ctx);

  if (ctrl_c_pressed > 0) {
    if (line) free(line);
    return ANT_READLINE_INTERRUPT;
  }
  if (!line) return ANT_READLINE_EOF;

  if (out_line) *out_line = line;
  return ANT_READLINE_LINE;
}

ant_readline_result_t ant_readline(
  ant_history_t *hist,
  const char *prompt,
  highlight_state line_state,
  char **out_line
) {
  return ant_readline_with_preview(hist, prompt, line_state, NULL, NULL, out_line);
}
