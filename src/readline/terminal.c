#include "readline_internal.h"

#include <errno.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/select.h>
#endif

volatile sig_atomic_t ctrl_c_pressed = 0;

#ifndef _WIN32
static struct termios saved_tio;
static int signal_pipe[2] = { -1, -1 };
#endif

static void sigint_handler(int sig) {
  (void)sig;
  ctrl_c_pressed++;
#ifndef _WIN32
  if (signal_pipe[1] >= 0) {
    int saved_errno = errno;
    unsigned char wake = 1;
    (void)write(signal_pipe[1], &wake, 1);
    errno = saved_errno;
  }
#endif
}

void ant_readline_install_signal_handler(void) {
#ifdef _WIN32
  signal(SIGINT, sigint_handler);
#else
  if (signal_pipe[0] < 0 && pipe(signal_pipe) == 0) for (int i = 0; i < 2; i++) {
    int flags = fcntl(signal_pipe[i], F_GETFL, 0);
    if (flags >= 0) (void)fcntl(signal_pipe[i], F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(signal_pipe[i], F_GETFD, 0);
    if (flags >= 0) (void)fcntl(signal_pipe[i], F_SETFD, flags | FD_CLOEXEC);
  }
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = sigint_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
#endif
}

bool ant_readline_interrupt_pending(void) {
  return ctrl_c_pressed > 0;
}

void ant_readline_clear_interrupt(void) {
  ctrl_c_pressed = 0;
  ant_readline_drain_interrupt_wake();
}

int ant_readline_interrupt_fd(void) {
#ifdef _WIN32
  return -1;
#else
  return signal_pipe[0];
#endif
}

void ant_readline_drain_interrupt_wake(void) {
#ifndef _WIN32
  if (signal_pipe[0] < 0) return;
  unsigned char buffer[64];
  while (read(signal_pipe[0], buffer, sizeof(buffer)) > 0) {}
#endif
}

void ant_readline_shutdown_signal_bridge(void) {
#ifndef _WIN32
  for (int i = 0; i < 2; i++) {
    if (signal_pipe[i] >= 0) close(signal_pipe[i]);
    signal_pipe[i] = -1;
  }
#endif
}

int repl_terminal_cols(void) {
  int cols = 80;
#ifdef _WIN32
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
    cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
  }
#else
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
    cols = ws.ws_col;
  }
#endif
  return (cols > 0) ? cols : 80;
}

void repl_jump_cursor(int x_pos, int y_offset) {
  if (y_offset != 0) {
    char seq[32];
    snprintf(seq, sizeof(seq), "\033[%d%c", abs(y_offset), (y_offset > 0) ? 'B' : 'A');
    fputs(seq, stdout);
  }
  char seq[32];
  snprintf(seq, sizeof(seq), "\033[%dG", x_pos + 1);
  fputs(seq, stdout);
}

void repl_clear_to_end(void) {
  fputs("\033[J", stdout);
}

void repl_set_cursor_visible(bool visible) {
#ifdef _WIN32
  (void)visible;
#else
  fputs(visible ? "\033[?25h" : "\033[?25l", stdout);
#endif
}

void repl_enable_raw_mode(void) {
#ifndef _WIN32
  struct termios new_tio;
  tcgetattr(STDIN_FILENO, &saved_tio);
  new_tio = saved_tio;
  new_tio.c_lflag &= ~(ICANON | ECHO);
  new_tio.c_cc[VMIN] = 1;
  new_tio.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
#endif
}

void repl_term_restore(void) {
#ifndef _WIN32
  tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio);
#endif
}

#ifdef _WIN32
bool repl_input_pending(void) {
  return _kbhit() != 0;
}

key_event_t repl_read_key(void) {
  if (ctrl_c_pressed > 0) return (key_event_t){ KEY_EOF, 0 };
  int c = _getch();
  if (c == 0 || c == 0xE0) {
    int ext = _getch();
    switch (ext) {
      case 72: return (key_event_t){ KEY_UP, 0 };
      case 80: return (key_event_t){ KEY_DOWN, 0 };
      case 77: return (key_event_t){ KEY_RIGHT, 0 };
      case 75: return (key_event_t){ KEY_LEFT, 0 };
      case 71: return (key_event_t){ KEY_HOME, 0 };
      case 79: return (key_event_t){ KEY_END, 0 };
      case 83: return (key_event_t){ KEY_DELETE, 0 };
      default: return (key_event_t){ KEY_NONE, 0 };
    }
  }
  if (c == 8) return (key_event_t){ KEY_BACKSPACE, 0 };
  if (c == '\t') return (key_event_t){ KEY_TAB, 0 };
  if (c == '\r' || c == '\n') return (key_event_t){ KEY_ENTER, 0 };
  if (c == 3) {
    ctrl_c_pressed++;
    return (key_event_t){ KEY_EOF, 0 };
  }
  if (c == 4 || c == 26) return (key_event_t){ KEY_EOF, 0 };
  if (isprint(c) || (unsigned char)c >= 0x80) return (key_event_t){ KEY_CHAR, c };
  return (key_event_t){ KEY_NONE, 0 };
}
#else
bool repl_input_pending(void) {
  for (;;) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    int signal_fd = ant_readline_interrupt_fd();
    if (signal_fd >= 0) FD_SET(signal_fd, &fds);

    struct timeval timeout = {0, 0};
    int max_fd = signal_fd > STDIN_FILENO ? signal_fd : STDIN_FILENO;
    int ready = select(max_fd + 1, &fds, NULL, NULL, &timeout);
    if (ready < 0 && errno == EINTR && ctrl_c_pressed == 0) continue;

    return ready > 0 && (
      FD_ISSET(STDIN_FILENO, &fds) ||
      (signal_fd >= 0 && FD_ISSET(signal_fd, &fds))
    );
  }
}

static int repl_read_byte(void) {
  unsigned char c = 0;
  for (;;) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    int signal_fd = ant_readline_interrupt_fd();
    if (signal_fd >= 0) FD_SET(signal_fd, &fds);
    int max_fd = signal_fd > STDIN_FILENO ? signal_fd : STDIN_FILENO;
    int ready = select(max_fd + 1, &fds, NULL, NULL, NULL);
    if (ready < 0) {
      if (errno == EINTR && ctrl_c_pressed == 0) continue;
      return EOF;
    }
    if (signal_fd >= 0 && FD_ISSET(signal_fd, &fds)) {
      ant_readline_drain_interrupt_wake();
      return EOF;
    }
    if (FD_ISSET(STDIN_FILENO, &fds))
      return read(STDIN_FILENO, &c, 1) == 1 ? (int)c : EOF;
  }
}

key_event_t repl_read_key(void) {
  if (ctrl_c_pressed > 0) return (key_event_t){ KEY_EOF, 0 };
  int c = repl_read_byte();
  if (c == EOF) return (key_event_t){ KEY_EOF, 0 };

  if (c == 27) {
    int seq1 = repl_read_byte();
    if (seq1 == EOF) return (key_event_t){ KEY_NONE, 0 };
    if (seq1 == 'O') {
      int seq2 = repl_read_byte();
      if (seq2 == 'H') return (key_event_t){ KEY_HOME, 0 };
      if (seq2 == 'F') return (key_event_t){ KEY_END, 0 };
      return (key_event_t){ KEY_NONE, 0 };
    }
    if (seq1 != '[') return (key_event_t){ KEY_NONE, 0 };
    int seq2 = repl_read_byte();
    if (seq2 == EOF) return (key_event_t){ KEY_NONE, 0 };
    switch (seq2) {
      case 'A': return (key_event_t){ KEY_UP, 0 };
      case 'B': return (key_event_t){ KEY_DOWN, 0 };
      case 'C': return (key_event_t){ KEY_RIGHT, 0 };
      case 'D': return (key_event_t){ KEY_LEFT, 0 };
      case 'H': return (key_event_t){ KEY_HOME, 0 };
      case 'F': return (key_event_t){ KEY_END, 0 };
      default: {
        if (seq2 >= '0' && seq2 <= '9') {
          int seq3 = repl_read_byte();
          if (seq3 == '~') {
            if (seq2 == '1' || seq2 == '7') return (key_event_t){ KEY_HOME, 0 };
            if (seq2 == '4' || seq2 == '8') return (key_event_t){ KEY_END, 0 };
            if (seq2 == '3') return (key_event_t){ KEY_DELETE, 0 };
          }
        }
        return (key_event_t){ KEY_NONE, 0 };
      }
    }
  }

  if (c == 127 || c == 8) return (key_event_t){ KEY_BACKSPACE, 0 };
  if (c == '\t') return (key_event_t){ KEY_TAB, 0 };
  if (c == '\n' || c == '\r') return (key_event_t){ KEY_ENTER, 0 };
  if (isprint(c) || (unsigned char)c >= 0x80) return (key_event_t){ KEY_CHAR, c };
  return (key_event_t){ KEY_NONE, 0 };
}
#endif
