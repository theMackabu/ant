#ifndef PROGRESS_H
#define PROGRESS_H

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define PROGRESS_ISATTY(fd) _isatty(fd)
#define PROGRESS_FILENO(f) _fileno(f)
#else
#include <unistd.h>
#include <sys/ioctl.h>
#define PROGRESS_ISATTY(fd) isatty(fd)
#define PROGRESS_FILENO(f) fileno(f)
#endif

#define PROGRESS_MSG_SIZE 256

#ifdef __has_include
#if __has_include(<pthread.h>)
#include <pthread.h>
#define PROGRESS_HAS_PTHREADS 1
#endif
#endif

typedef struct {
#if defined(PROGRESS_HAS_PTHREADS)
  pthread_mutex_t mtx;
#elif defined(_WIN32)
  CRITICAL_SECTION cs;
#else
  int dummy;
#endif
} progress_mutex_t;

typedef struct {
  FILE *terminal;
  bool is_windows_terminal;
  bool supports_ansi;
  bool dont_print_on_dumb;
  uint64_t start_time_ns;
  uint64_t prev_refresh_ns;
  uint64_t refresh_rate_ns;
  uint64_t initial_delay_ns;
  bool done;
  bool timer_valid;
  size_t columns_written;
  progress_mutex_t mutex;
  char buffer[PROGRESS_MSG_SIZE];
  char msg_buffer[PROGRESS_MSG_SIZE];
} progress_t;

static inline void progress_mutex_init(progress_mutex_t *m) {
#if defined(PROGRESS_HAS_PTHREADS)
  pthread_mutex_init(&m->mtx, NULL);
#elif defined(_WIN32)
  InitializeCriticalSection(&m->cs);
#else
  (void)m;
#endif
}

static inline void progress_mutex_destroy(progress_mutex_t *m) {
#if defined(PROGRESS_HAS_PTHREADS)
  pthread_mutex_destroy(&m->mtx);
#elif defined(_WIN32)
  DeleteCriticalSection(&m->cs);
#else
  (void)m;
#endif
}

static inline void progress_mutex_lock(progress_mutex_t *m) {
#if defined(PROGRESS_HAS_PTHREADS)
  pthread_mutex_lock(&m->mtx);
#elif defined(_WIN32)
  EnterCriticalSection(&m->cs);
#else
  (void)m;
#endif
}

static inline void progress_mutex_unlock(progress_mutex_t *m) {
#if defined(PROGRESS_HAS_PTHREADS)
  pthread_mutex_unlock(&m->mtx);
#elif defined(_WIN32)
  LeaveCriticalSection(&m->cs);
#else
  (void)m;
#endif
}

static inline bool progress_mutex_trylock(progress_mutex_t *m) {
#if defined(PROGRESS_HAS_PTHREADS)
  return pthread_mutex_trylock(&m->mtx) == 0;
#elif defined(_WIN32)
  return TryEnterCriticalSection(&m->cs) != 0;
#else
  (void)m;
  return true;
#endif
}

static inline uint64_t progress_now_ns(void) {
  struct timespec ts;
#if defined(CLOCK_MONOTONIC)
  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
  }
#elif defined(_WIN32)
  static LARGE_INTEGER freq = {0};
  if (freq.QuadPart == 0) {
    QueryPerformanceFrequency(&freq);
  }
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  return (uint64_t)(counter.QuadPart * 1000000000ULL / freq.QuadPart);
#endif
  if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
  }
  return 0;
}

static inline bool progress_detect_ansi(FILE *f) {
  if (!f) return false;
  
  int fd = PROGRESS_FILENO(f);
  if (!PROGRESS_ISATTY(fd)) return false;
  
#ifdef _WIN32
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  DWORD mode;
  if (GetConsoleMode(h, &mode)) {
    if (SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) return true;
  }
  const char *term = getenv("TERM");
  return (term && strstr(term, "xterm"))
      || (term && strstr(term, "vt100"))
      || (term && strstr(term, "color"))
      || (term && strstr(term, "ansi"));
#else
  const char *term = getenv("TERM");
  if (!term) return false;
  if (strcmp(term, "dumb") == 0) return false;
  return true;
#endif
}

static void progress_refresh_locked(progress_t *p);
static void progress_clear_locked(progress_t *p, size_t *end);

static inline void progress_start(progress_t *p, const char *message) {
  memset(p, 0, sizeof(*p));
  progress_mutex_init(&p->mutex);
  
  FILE *f = stderr;
  int fd = PROGRESS_FILENO(f);
  
  if (PROGRESS_ISATTY(fd)) {
    p->terminal = f;
    p->supports_ansi = progress_detect_ansi(f);
#ifdef _WIN32
    if (!p->supports_ansi) p->is_windows_terminal = true;
#endif
  } else {
    p->terminal = f;
    p->supports_ansi = false;
    p->is_windows_terminal = false;
  }
  
  if (message) {
    strncpy(p->msg_buffer, message, PROGRESS_MSG_SIZE - 1);
    p->msg_buffer[PROGRESS_MSG_SIZE - 1] = '\0';
  } else p->msg_buffer[0] = '\0';
  
  p->refresh_rate_ns = 50 * 1000000ULL;
  p->initial_delay_ns = 500 * 1000000ULL;
  p->start_time_ns = progress_now_ns();
  p->prev_refresh_ns = 0;
  p->timer_valid = (p->start_time_ns != 0);
  
  p->columns_written = 0;
  p->done = false;
  
  progress_refresh_locked(p);
}

static inline void progress_maybe_refresh(progress_t *p) {
  if (!p->timer_valid) return;
  if (!progress_mutex_trylock(&p->mutex)) return;
  
  uint64_t now = progress_now_ns();
  uint64_t elapsed = now - p->start_time_ns;
  
  if (elapsed < p->initial_delay_ns) {
    progress_mutex_unlock(&p->mutex);
    return;
  }
  
  if (now < p->prev_refresh_ns || (now - p->prev_refresh_ns) < p->refresh_rate_ns) {
    progress_mutex_unlock(&p->mutex);
    return;
  }
  
  progress_refresh_locked(p);
  progress_mutex_unlock(&p->mutex);
}

static inline void progress_update(progress_t *p, const char *message) {
  progress_mutex_lock(&p->mutex);
  if (message) {
    strncpy(p->msg_buffer, message, PROGRESS_MSG_SIZE - 1);
    p->msg_buffer[PROGRESS_MSG_SIZE - 1] = '\0';
  } else p->msg_buffer[0] = '\0';

  progress_mutex_unlock(&p->mutex);
  progress_maybe_refresh(p);
}

static inline void progress_refresh(progress_t *p) {
  if (!progress_mutex_trylock(&p->mutex)) return;
  progress_refresh_locked(p);
  progress_mutex_unlock(&p->mutex);
}

static void progress_clear_locked(progress_t *p, size_t *end) {
  if (!p->terminal) return;
  
  size_t pos = *end;
  
  if (p->columns_written > 0) {
    if (p->supports_ansi) {
      int written = snprintf(p->buffer + pos, sizeof(p->buffer) - pos, "\x1b[%zuD\x1b[0K", p->columns_written);
      if (written > 0) pos += (size_t)written;
    } else if (p->is_windows_terminal) {
#ifdef _WIN32
      HANDLE h = (HANDLE)_get_osfhandle(PROGRESS_FILENO(p->terminal));
      CONSOLE_SCREEN_BUFFER_INFO info;
      if (GetConsoleScreenBufferInfo(h, &info)) {
        COORD cursor = {
          .X = (SHORT)(info.dwCursorPosition.X - (SHORT)p->columns_written),
          .Y = info.dwCursorPosition.Y
        };
        if (cursor.X < 0) cursor.X = 0;
        
        DWORD fill_len = (DWORD)(info.dwSize.X - cursor.X);
        DWORD written;
        FillConsoleOutputAttribute(h, info.wAttributes, fill_len, cursor, &written);
        FillConsoleOutputCharacterA(h, ' ', fill_len, cursor, &written);
        SetConsoleCursorPosition(h, cursor);
      }
#endif
    } else {
      if (pos < sizeof(p->buffer)) p->buffer[pos++] = '\n';
    }
    p->columns_written = 0;
  }
  
  *end = pos;
}

static void progress_refresh_locked(progress_t *p) {
  bool is_dumb = !p->supports_ansi && !p->is_windows_terminal;
  if (is_dumb && p->dont_print_on_dumb) return;
  if (!p->terminal) return;
  
  size_t end = 0;
  progress_clear_locked(p, &end);
  
  if (!p->done) {
    if (p->msg_buffer[0]) {
      int written = snprintf(p->buffer + end, sizeof(p->buffer) - end, "  %s", p->msg_buffer);
      if (written > 0) {
        size_t amt = (
          (size_t)written < sizeof(p->buffer) - end) 
           ? (size_t)written 
           : sizeof(p->buffer) - end - 1;
        end += amt;
        p->columns_written = amt;
      }
    }
  }
  
  if (end > 0) {
    fwrite(p->buffer, 1, end, p->terminal);
    fflush(p->terminal);
  }
  
  p->prev_refresh_ns = progress_now_ns();
}

static inline void progress_stop(progress_t *p) {
  progress_mutex_lock(&p->mutex);
  p->done = true;
  
  size_t end = 0;
  progress_clear_locked(p, &end);
  if (end > 0 && p->terminal) {
    fwrite(p->buffer, 1, end, p->terminal);
    fflush(p->terminal);
  }
  
  progress_mutex_unlock(&p->mutex);
  progress_mutex_destroy(&p->mutex);
}

#endif
