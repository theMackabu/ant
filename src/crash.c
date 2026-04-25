#include <compat.h> // IWYU pragma: keep
#include "crash.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef _WIN32
#include <signal.h>

#define ANT_CRASH_ALT_STACK_SIZE (64 * 1024)

#if defined(__APPLE__) || defined(__linux__) || defined(__GLIBC__)
#define ANT_CRASH_HAVE_EXECINFO 1
#include <execinfo.h>
#endif

static void as_write(int fd, const char *s) {
  size_t len = 0;
  while (s[len]) len++;
  ssize_t _ = write(fd, s, len);
  (void)_;
}

static void as_write_uint(int fd, unsigned long v) {
  char buf[32];
  int i = (int)sizeof(buf);
  buf[--i] = '\0';
  if (v == 0) buf[--i] = '0';
  while (v && i > 0) {
    buf[--i] = (char)('0' + (v % 10));
    v /= 10;
  }
  as_write(fd, &buf[i]);
}

static int install_altstack(void) {
#ifdef SA_ONSTACK
  static void *stack_mem;
  if (stack_mem) return 1;

  stack_mem = malloc(ANT_CRASH_ALT_STACK_SIZE);
  if (!stack_mem) return 0;

  stack_t ss;
  memset(&ss, 0, sizeof(ss));
  ss.ss_sp = stack_mem;
  ss.ss_size = ANT_CRASH_ALT_STACK_SIZE;
  ss.ss_flags = 0;

  if (sigaltstack(&ss, NULL) == 0) return 1;

  free(stack_mem);
  stack_mem = NULL;
#endif
  return 0;
}

static const char *signal_name(int sig) {
switch (sig) {
  case SIGSEGV: return "SIGSEGV (invalid memory access)";
  case SIGBUS:  return "SIGBUS (bus error)";
  case SIGFPE:  return "SIGFPE (arithmetic exception)";
  case SIGILL:  return "SIGILL (illegal instruction)";
  case SIGABRT: return "SIGABRT (abort)";
  default:      return "fatal signal";
}}

static void crash_handler(int sig, siginfo_t *info, void *ucontext) {
  struct sigaction dfl;
  memset(&dfl, 0, sizeof(dfl));
  
  dfl.sa_handler = SIG_DFL;
  sigemptyset(&dfl.sa_mask);
  sigaction(sig, &dfl, NULL);

  int fd = STDERR_FILENO;
  as_write(fd, "\n=== ant crashed: ");
  as_write(fd, signal_name(sig));
  as_write(fd, " (signal ");
  as_write_uint(fd, (unsigned long)sig);
  as_write(fd, ") ===\n");

  if (info) {
    as_write(fd, "  fault address: 0x");
    char hex[2 + sizeof(void *) * 2 + 1];
    int hi = (int)sizeof(hex);
    
    hex[--hi] = '\0';
    uintptr_t addr = (uintptr_t)info->si_addr;
    
    if (addr == 0) hex[--hi] = '0';
    else while (addr && hi > 0) {
      unsigned d = (unsigned)(addr & 0xF);
      hex[--hi] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
      addr >>= 4;
    }
    
    as_write(fd, &hex[hi]);
    as_write(fd, "\n");
  }

  as_write(fd, "  ant version: " ANT_VERSION "\n");
  as_write(fd, "  pid: ");
  as_write_uint(fd, (unsigned long)getpid());
  as_write(fd, "\n\nNative backtrace:\n");

#ifdef ANT_CRASH_HAVE_EXECINFO
  void *frames[64];
  int n = backtrace(frames, (int)(sizeof(frames) / sizeof(frames[0])));
  int skip = n > 2 ? 2 : 0;
  if (skip) as_write(fd, "  (omitted crash_handler and signal trampoline frames)\n");
  backtrace_symbols_fd(frames + skip, n - skip, fd);
#else
  as_write(fd, "  (no execinfo support on this platform)\n");
#endif

  as_write(fd,
    "\nPlease report this at https://github.com/themackabu/ant/issues\n"
    "Include the backtrace above and a minimal reproducer if possible.\n\n");

  raise(sig);
}

void ant_crash_init(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
#ifdef SA_ONSTACK
  if (install_altstack()) sa.sa_flags |= SA_ONSTACK;
#endif
  sigemptyset(&sa.sa_mask);

  static const int sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };
  for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
    sigaction(sigs[i], &sa, NULL);
  }
}

#else // _WIN32

void ant_crash_init(void) {
  // TODO: SetUnhandledExceptionFilter + StackWalk64 / dbghelp
}

#endif
