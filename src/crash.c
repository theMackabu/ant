#include <compat.h> // IWYU pragma: keep
#include "crash.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <signal.h>
#include <unistd.h>

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

#include <dbghelp.h>
#include <process.h>

static LPTOP_LEVEL_EXCEPTION_FILTER previous_filter;
static volatile LONG crash_in_progress;

static const char *exception_name(DWORD code) {
switch (code) {
  case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
  case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
  case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
  case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
  case EXCEPTION_FLT_DENORMAL_OPERAND: return "EXCEPTION_FLT_DENORMAL_OPERAND";
  case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
  case EXCEPTION_FLT_INEXACT_RESULT: return "EXCEPTION_FLT_INEXACT_RESULT";
  case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
  case EXCEPTION_FLT_OVERFLOW: return "EXCEPTION_FLT_OVERFLOW";
  case EXCEPTION_FLT_STACK_CHECK: return "EXCEPTION_FLT_STACK_CHECK";
  case EXCEPTION_FLT_UNDERFLOW: return "EXCEPTION_FLT_UNDERFLOW";
  case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
  case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
  case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
  case EXCEPTION_INT_OVERFLOW: return "EXCEPTION_INT_OVERFLOW";
  case EXCEPTION_INVALID_DISPOSITION: return "EXCEPTION_INVALID_DISPOSITION";
  case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
  case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
  case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
  default: return "fatal exception";
}}

static DWORD64 exception_fault_address(EXCEPTION_RECORD *record) {
  if (!record) return 0;
  if ((
    record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || 
    record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) 
    && record->NumberParameters >= 2
  ) return (DWORD64)record->ExceptionInformation[1];
  return (DWORD64)(uintptr_t)record->ExceptionAddress;
}

static BOOL init_stack_frame(CONTEXT *ctx, STACKFRAME64 *frame, DWORD *machine) {
  memset(frame, 0, sizeof(*frame));

#if defined(_M_X64) || defined(__x86_64__)
  *machine = IMAGE_FILE_MACHINE_AMD64;
  frame->AddrPC.Offset = ctx->Rip;
  frame->AddrFrame.Offset = ctx->Rbp;
  frame->AddrStack.Offset = ctx->Rsp;
#elif defined(_M_IX86) || defined(__i386__)
  *machine = IMAGE_FILE_MACHINE_I386;
  frame->AddrPC.Offset = ctx->Eip;
  frame->AddrFrame.Offset = ctx->Ebp;
  frame->AddrStack.Offset = ctx->Esp;
#elif defined(_M_ARM64) || defined(__aarch64__)
  *machine = IMAGE_FILE_MACHINE_ARM64;
  frame->AddrPC.Offset = ctx->Pc;
  frame->AddrFrame.Offset = ctx->Fp;
  frame->AddrStack.Offset = ctx->Sp;
#else
  return FALSE;
#endif
  frame->AddrPC.Mode = AddrModeFlat;
  frame->AddrFrame.Mode = AddrModeFlat;
  frame->AddrStack.Mode = AddrModeFlat;
  return TRUE;
}

static void print_windows_backtrace(EXCEPTION_POINTERS *exc) {
  if (!exc || !exc->ContextRecord) {
    fprintf(stderr, "  (no exception context available)\n");
    return;
  }

  HANDLE process = GetCurrentProcess();
  HANDLE thread = GetCurrentThread();

  SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
  if (!SymInitialize(process, NULL, TRUE)) {
    fprintf(stderr, "  (SymInitialize failed: %lu)\n", GetLastError());
    return;
  }

  CONTEXT ctx = *exc->ContextRecord;
  STACKFRAME64 frame;
  DWORD machine;
  if (!init_stack_frame(&ctx, &frame, &machine)) {
    fprintf(stderr, "  (unsupported Windows architecture for StackWalk64)\n");
    return;
  }

  union {
    SYMBOL_INFO info;
    char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
  } symbol_buf;
  
  SYMBOL_INFO *symbol = &symbol_buf.info;
  memset(&symbol_buf, 0, sizeof(symbol_buf));
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = MAX_SYM_NAME;

  for (int i = 0; i < 64; i++) {
    DWORD64 addr = frame.AddrPC.Offset;
    if (addr == 0) break;
    DWORD64 displacement = 0;
    
    fprintf(stderr, "%d   0x%016llx", i, (unsigned long long)addr);
    if (SymFromAddr(process, addr, &displacement, symbol)) 
      fprintf(stderr, " %s + %llu", symbol->Name, (unsigned long long)displacement);
    
    IMAGEHLP_LINE64 line;
    DWORD line_disp = 0;
    memset(&line, 0, sizeof(line));
    line.SizeOfStruct = sizeof(line);
    if (SymGetLineFromAddr64(process, addr, &line_disp, &line)) 
      fprintf(stderr, " (%s:%lu)", line.FileName, line.LineNumber);
    
    fputc('\n', stderr);
    DWORD64 prev_pc = frame.AddrPC.Offset;
    DWORD64 prev_sp = frame.AddrStack.Offset;
    BOOL ok = StackWalk64(
      machine, process, thread, &frame, &ctx, NULL,
      SymFunctionTableAccess64, SymGetModuleBase64, NULL
    );
    if (!ok || (frame.AddrPC.Offset == prev_pc && frame.AddrStack.Offset == prev_sp)) break;
  }
}

static LONG WINAPI windows_crash_handler(EXCEPTION_POINTERS *exc) {
  if (InterlockedExchange(&crash_in_progress, 1) != 0) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  EXCEPTION_RECORD *record = exc ? exc->ExceptionRecord : NULL;
  DWORD code = record ? record->ExceptionCode : 0;

  fprintf(stderr, "\n=== ant crashed: %s (0x%08lx) ===\n", exception_name(code), (unsigned long)code);
  fprintf(stderr, "  fault address: 0x%016llx\n", (unsigned long long)exception_fault_address(record));
  fprintf(stderr, "  ant version: " ANT_VERSION "\n");
  fprintf(stderr, "  pid: %lu\n\n", (unsigned long)_getpid());
  fprintf(stderr, "Native backtrace:\n");
  print_windows_backtrace(exc);
  
  fprintf(stderr,
    "\nPlease report this at https://github.com/themackabu/ant/issues\n"
    "Include the backtrace above and a minimal reproducer if possible.\n\n");

  if (previous_filter) return previous_filter(exc);
  return EXCEPTION_EXECUTE_HANDLER;
}

void ant_crash_init(void) {
  previous_filter = SetUnhandledExceptionFilter(windows_crash_handler);
}

#endif
