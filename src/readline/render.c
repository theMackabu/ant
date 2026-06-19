#include "readline.h"
#include "utf8.h"
#include "readline_internal.h"

static crprintf_compiled *hl_prog = NULL;
static highlight_state hl_line_state = HL_STATE_INIT;
static int repl_last_cursor_row = 0;

void repl_render_shutdown(void) {
  if (hl_prog) {
    crprintf_compiled_free(hl_prog);
    hl_prog = NULL;
  }
}

void ant_readline_shutdown(void) {
  repl_render_shutdown();
}

void repl_render_set_highlight_state(highlight_state state) {
  hl_line_state = state;
}

void repl_render_reset_cursor_row(void) {
  repl_last_cursor_row = 0;
}

static size_t repl_skip_ansi_escape(const char *s, size_t len, size_t i) {
  if (i >= len || (unsigned char)s[i] != 0x1B) return i;
  if (i + 1 >= len) return i + 1;

  unsigned char next = (unsigned char)s[i + 1];

  if (next == '[') {
    i += 2;
    while (i < len) {
      unsigned char ch = (unsigned char)s[i++];
      if (ch >= 0x40 && ch <= 0x7E) break;
    }
    return i;
  }

  if (next == ']') {
    i += 2;
    while (i < len) {
      unsigned char ch = (unsigned char)s[i];
      if (ch == '\a') return i + 1;
      if (ch == 0x1B && i + 1 < len && s[i + 1] == '\\') return i + 2;
      i++;
    }
    return i;
  }

  return i + 2;
}

static void repl_virtual_render(
  const char *s, size_t n,
  int screen_cols, int prompt_len,
  int *x, int *y
) {
  bool wrapped = false;
  size_t i = 0;
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    if (c == '\n' || c == '\r') {
      if (c == '\n' && !wrapped) (*y)++;
      *x = prompt_len;
      i++;
      wrapped = false;
      continue;
    }

    if (c == 0x1B) {
      i = repl_skip_ansi_escape(s, n, i);
      continue;
    }

    utf8proc_int32_t cp = 0;
    utf8proc_ssize_t clen =
      utf8_next((const utf8proc_uint8_t *)(s + i), (utf8proc_ssize_t)(n - i), &cp);
    if (clen <= 0) {
      clen = 1;
      cp = c;
    }

    int w = utf8proc_charwidth(cp);
    if (w < 0) w = 1;

    if (w > 0) {
      *x += w;
      wrapped = false;
      if (*x >= screen_cols) {
        *x = 0;
        (*y)++;
        wrapped = true;
      }
    }

    i += (size_t)clen;
  }
}

static int repl_prompt_width(const char *prompt, int cols) {
  int x = 0;
  int y = 0;
  repl_virtual_render(prompt, strlen(prompt), cols, 0, &x, &y);
  return x;
}

static size_t repl_preview_fit_line(
  const char *preview,
  size_t preview_len,
  int cols,
  char *out,
  size_t out_len
) {
  if (!preview || !out || out_len == 0 || cols <= 0) return 0;

  int max_cols = cols > 4 ? cols - 1 : cols;
  int ellipsis_cols = 3;
  int x = 0;
  size_t i = 0;
  size_t n = 0;

  while (i < preview_len && preview[i] && n + 1 < out_len) {
    utf8proc_int32_t cp = 0;
    utf8proc_ssize_t clen = utf8_next(
      (const utf8proc_uint8_t *)(preview + i),
      (utf8proc_ssize_t)(preview_len - i),
      &cp
    );
    if (clen <= 0) {
      clen = 1;
      cp = (unsigned char)preview[i];
    }

    int w = utf8proc_charwidth(cp);
    if (w < 0) w = 1;
    int limit = (i + (size_t)clen < preview_len) ? max_cols - ellipsis_cols : max_cols;
    if (w > 0 && x + w > limit) break;
    if (n + (size_t)clen >= out_len) break;
    memcpy(out + n, preview + i, (size_t)clen);
    n += (size_t)clen;
    i += (size_t)clen;
    x += w;
  }

  bool clipped = i < preview_len && preview[i] != '\0';
  if (clipped && out_len > n + 3) {
    memcpy(out + n, "...", 3);
    n += 3;
  }
  out[n] = '\0';
  return n;
}

static void repl_track_sgr_color(
  const char *seq,
  size_t len,
  bool *has_fg,
  int *fg_r,
  int *fg_g,
  int *fg_b
) {
  if (len < 3 || seq[0] != '\033' || seq[1] != '[' || seq[len - 1] != 'm')
    return;

  int vals[16];
  int count = 0;
  size_t i = 2;
  while (i + 1 < len && count < (int)(sizeof(vals) / sizeof(vals[0]))) {
    int val = 0;
    bool have_digit = false;
    while (i + 1 < len && isdigit((unsigned char)seq[i])) {
      have_digit = true;
      val = val * 10 + (seq[i] - '0');
      i++;
    }
    vals[count++] = have_digit ? val : 0;
    if (i + 1 < len && seq[i] == ';') i++;
    else break;
  }

  for (int j = 0; j < count; j++) {
    if (vals[j] == 0) *has_fg = false;
    else if (vals[j] == 39) *has_fg = false;
    else if (vals[j] == 38 && j + 4 < count && vals[j + 1] == 2) {
      *fg_r = vals[j + 2];
      *fg_g = vals[j + 3];
      *fg_b = vals[j + 4];
      *has_fg = true;
      j += 4;
    }
  }
}

static void repl_fputs_dim_after_visible(const char *s, size_t dim_after) {
  bool dimmed = false;
  bool has_fg = false;
  int fg_r = 0;
  int fg_g = 0;
  int fg_b = 0;
  size_t visible = 0;
  size_t i = 0;

  while (s[i]) {
    if ((unsigned char)s[i] == 0x1B) {
      size_t next = repl_skip_ansi_escape(s, strlen(s), i);
      fwrite(s + i, 1, next - i, stdout);
      repl_track_sgr_color(s + i, next - i, &has_fg, &fg_r, &fg_g, &fg_b);
      i = next;
      continue;
    }

    if (!dimmed && visible >= dim_after) {
      if (has_fg) {
        int dr = (fg_r * 58) / 100;
        int dg = (fg_g * 58) / 100;
        int db = (fg_b * 58) / 100;
        fprintf(stdout, "\033[38;2;%d;%d;%dm", dr, dg, db);
      } else fputs("\033[2m", stdout);
      dimmed = true;
    }

    utf8proc_int32_t cp = 0;
    utf8proc_ssize_t clen = utf8_next(
      (const utf8proc_uint8_t *)(s + i),
      (utf8proc_ssize_t)strlen(s + i),
      &cp
    );
    if (clen <= 0) clen = 1;
    fwrite(s + i, 1, (size_t)clen, stdout);
    i += (size_t)clen;
    visible += (size_t)clen;
  }

  if (dimmed && !has_fg) fputs("\033[22m", stdout);
}

void refresh_line_with_preview(
  const char *line,
  int len,
  int pos,
  const char *prompt,
  const char *suffix,
  const char *preview
) {
  int cols = repl_terminal_cols();
  int prompt_len = repl_prompt_width(prompt, cols);
  int x_cursor = prompt_len;
  int y_cursor = 0;
  int x_end = prompt_len;
  int y_end = 0;
  int x_render_end = prompt_len;
  int y_render_end = 0;
  size_t suffix_len = (suffix && pos == len) ? strlen(suffix) : 0;
  size_t preview_len = preview ? strlen(preview) : 0;
  char display_line[MAX_LINE_LENGTH + MAX_PREVIEW_LENGTH];
  size_t display_len = (size_t)len;
  char preview_line[MAX_PREVIEW_LENGTH];
  const char *preview_render = NULL;

  if (len > 0) memcpy(display_line, line, (size_t)len);
  if (suffix_len > 0) {
    size_t avail = sizeof(display_line) - display_len - 1;
    if (suffix_len > avail) suffix_len = avail;
    memcpy(display_line + display_len, suffix, suffix_len);
    display_len += suffix_len;
  }
  display_line[display_len] = '\0';

  repl_virtual_render(line, (size_t)pos, cols, prompt_len, &x_cursor, &y_cursor);
  repl_virtual_render(display_line, display_len, cols, prompt_len, &x_end, &y_end);
  x_render_end = x_end;
  y_render_end = y_end;
  if (preview_len > 0) {
    preview_len = repl_preview_fit_line(
      preview,
      preview_len,
      cols,
      preview_line,
      sizeof(preview_line)
    );
    preview_render = preview_line;
    x_render_end = 0;
    y_render_end = y_end + 1;
    repl_virtual_render(preview_render, preview_len, cols, 0, &x_render_end, &y_render_end);
  }

  repl_set_cursor_visible(false);
  repl_jump_cursor(prompt_len, -repl_last_cursor_row);
  repl_clear_to_end();
  if (crprintf_get_color() && display_len > 0) {
    if (display_len <= 2048) {
    char tagged[8192];
    char rendered[8192];

    highlight_state state = hl_line_state;
    ant_highlight_stateful(display_line, display_len, tagged, sizeof(tagged), &state);

    hl_prog = crprintf_recompile(hl_prog, tagged);
    crprintf_state *rs = crprintf_state_new();
    crsprintf_compiled(rendered, sizeof(rendered), rs, hl_prog);
    crprintf_state_free(rs);

    if (suffix_len > 0) repl_fputs_dim_after_visible(rendered, (size_t)len);
    else fputs(rendered, stdout);
    } else {
      fwrite(line, 1, (size_t)len, stdout);
      if (suffix_len > 0) {
        fputs("\033[2m", stdout);
        fwrite(suffix, 1, suffix_len, stdout);
        fputs("\033[22m", stdout);
      }
    }
  } else if (display_len > 0) {
    fwrite(line, 1, (size_t)len, stdout);
    if (suffix_len > 0) {
      fputs("\033[2m", stdout);
      fwrite(suffix, 1, suffix_len, stdout);
      fputs("\033[22m", stdout);
    }
  }

  if (preview_len > 0) {
    fputs("\n\033[2m", stdout);
    fwrite(preview_render, 1, preview_len, stdout);
    fputs("\033[0m", stdout);
  }

#ifndef _WIN32
  if (
    preview_len == 0 &&
    (x_end == 0) && (y_end > 0) && (len > 0) && (line[len - 1] != '\n')
  ) {
    fputs("\n", stdout);
  }
#endif

  repl_jump_cursor(x_cursor, -(y_render_end - y_cursor));
  repl_set_cursor_visible(true);
  repl_last_cursor_row = y_cursor;
  fflush(stdout);
}

void move_cursor_only(const char *line, int pos, const char *prompt) {
  int cols = repl_terminal_cols();
  int prompt_len = repl_prompt_width(prompt, cols);
  int x_cursor = prompt_len;
  int y_cursor = 0;
  repl_virtual_render(line, (size_t)pos, cols, prompt_len, &x_cursor, &y_cursor);
  repl_jump_cursor(x_cursor, -(repl_last_cursor_row - y_cursor));
  repl_last_cursor_row = y_cursor;
  fflush(stdout);
}
