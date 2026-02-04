#ifndef ANT_COMPAT_H
#define ANT_COMPAT_H

#if defined(__linux__) && !defined(_GNU_SOURCE)
  #define _GNU_SOURCE
#endif

#include <time.h>
#include <string.h>
#include <stdlib.h>

#ifndef PATH_MAX
  #ifdef _WIN32
  #define PATH_MAX MAX_PATH
#else
  #define PATH_MAX 4096
  #endif
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#include <direct.h>

static inline char *compat_realpath(const char *path, char *resolved) {
  char *allocated = NULL;
  if (resolved == NULL) {
    resolved = malloc(MAX_PATH);
    if (resolved == NULL) return NULL;
    allocated = resolved;
  }
  DWORD len = GetFullPathNameA(path, MAX_PATH, resolved, NULL);
  if (len == 0 || len > MAX_PATH) {
    if (allocated != NULL) free(allocated);
    return NULL;
  }
  DWORD attr = GetFileAttributesA(resolved);
  if (attr == INVALID_FILE_ATTRIBUTES) {
    if (allocated != NULL) free(allocated);
    return NULL;
  }
  return resolved;
}

static inline time_t compat_timegm(struct tm *tm) { return _mkgmtime(tm); }
static inline int compat_usleep(unsigned int us) { Sleep((us + 999) / 1000); return 0; }
static inline unsigned int compat_sleep(unsigned int seconds) { Sleep(seconds * 1000); return 0; }

#include <process.h>
#define getpid _getpid
#ifndef getppid
#define getppid _getpid
#endif

struct timeval {
  long tv_sec;
  long tv_usec;
};

static inline int compat_gettimeofday(struct timeval *tv, void *tz) {
  (void)tz;
  FILETIME ft;
  ULARGE_INTEGER ull;
  GetSystemTimeAsFileTime(&ft);
  ull.LowPart = ft.dwLowDateTime;
  ull.HighPart = ft.dwHighDateTime;
  ull.QuadPart -= 116444736000000000ULL;
  tv->tv_sec = (long)(ull.QuadPart / 10000000);
  tv->tv_usec = (long)((ull.QuadPart % 10000000) / 10);
  return 0;
}
#define gettimeofday compat_gettimeofday

static inline char *compat_strndup(const char *s, size_t n) {
  size_t len = strnlen(s, n);
  char *dup = malloc(len + 1);
  if (dup) { memcpy(dup, s, len); dup[len] = '\0'; }
  return dup;
}

static inline void *compat_memmem(const void *haystack, size_t h_len, const void *needle, size_t n_len) {
  const char *h = haystack;
  const char *last = h + h_len - n_len;
  const char *n = needle;
  char first;
  if (n_len == 0) return (void *)h;
  if (h_len < n_len) return NULL;
  if (n_len == 1) return memchr(haystack, *n, h_len);
  first = *n++;
  for (; h <= last; h++)
    if (*h == first && !memcmp(h + 1, n, n_len - 1)) return (void *)h;
  return NULL;
}

static inline char *compat_basename(char *path) {
  if (!path || !*path) return ".";
  char *p = path + strlen(path) - 1;
  while (p > path && (*p == '/' || *p == '\\')) *p-- = '\0';
  while (p > path && *p != '/' && *p != '\\') p--;
  return (p > path) ? p + 1 : path;
}

static inline char *compat_dirname(char *path) {
  if (!path || !*path) return ".";
  char *p = path + strlen(path) - 1;
  while (p > path && (*p == '/' || *p == '\\')) *p-- = '\0';
  while (p > path && *p != '/' && *p != '\\') p--;
  if (p == path) {
    if (*p == '/' || *p == '\\') return (*p == '/') ? "/" : "\\";
    return ".";
  }
  *p = '\0';
  return path;
}

static inline int compat_setenv(const char *name, const char *value, int overwrite) {
  if (!overwrite && getenv(name) != NULL) return 0;
  return _putenv_s(name, value);
}

static inline int compat_unsetenv(const char *name) {
  return _putenv_s(name, "");
}

#define realpath compat_realpath
#define timegm compat_timegm
#define usleep compat_usleep
#define sleep compat_sleep
#define strndup compat_strndup
#define memmem compat_memmem
#define basename compat_basename
#define dirname compat_dirname
#define setenv compat_setenv
#define unsetenv compat_unsetenv

typedef unsigned int useconds_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;

#ifndef S_IFLNK
#define S_IFLNK 0120000
#endif

#ifndef S_ISLNK
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#endif

#else
#include <unistd.h>
#include <libgen.h>
#include <sys/time.h>
#endif

#endif
