#include <compat.h> // IWYU pragma: keep

#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <crprintf.h>

#include "ant.h"
#include "crash.h"
#include "internal.h"
#include "reactor.h"
#include "utils.h"
#include "cli/version.h"

#include "silver/engine.h"
#include "modules/assert.h"
#include "modules/fetch.h"
#include "modules/json.h"

#define ANT_CRASH_FRAME_MAX      24
#define ANT_CRASH_ALT_STACK_SIZE 65536
#define ANT_CRASH_ARGV_MAX       1024
#define ANT_CRASH_EXE_PATH_MAX   1024
#define ANT_CRASH_PAYLOAD_MAX    32768

static char crash_argv[ANT_CRASH_ARGV_MAX] = "";
static char crash_exe_path[ANT_CRASH_EXE_PATH_MAX] = "";

static bool crash_print_trace = false;
static bool crash_reporting_enabled = true;
static bool crash_report_status_printed = false;
static bool crash_report_status_inline = false;

static uint64_t crash_start_ms = 0;
static volatile sig_atomic_t crash_reporting_suppressed = 0;

static bool should_upload_report(void) {
  return 
    crash_reporting_enabled && 
    crash_reporting_suppressed == 0;
}

bool ant_crash_is_internal_report(int argc, char **argv) {
  return
    argc >= 2 && argv && 
    argv[1] && strcmp(argv[1], "__internal-crash-report") == 0;
}

#ifdef _WIN32
#include <dbghelp.h>
#include <io.h>
#else
#include <limits.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/wait.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/utsname.h>
#endif

#if defined(__APPLE__) || defined(__GLIBC__)
#define ANT_CRASH_HAVE_EXECINFO 1
#include <execinfo.h>
#endif

#if !defined(ANT_CRASH_HAVE_EXECINFO) && defined(__has_include)
#if __has_include(<unwind.h>)
#define ANT_CRASH_HAVE_UNWIND_BACKTRACE 1
#include <unwind.h>
#endif
#endif
#endif

#ifdef ANT_CRASH_HAVE_UNWIND_BACKTRACE
typedef struct {
  uintptr_t *frames;
  int frame_count;
  int skip;
} crash_unwind_state_t;

static _Unwind_Reason_Code collect_unwind_frame(struct _Unwind_Context *ctx, void *arg) {
  crash_unwind_state_t *state = arg;
  uintptr_t ip = (uintptr_t)_Unwind_GetIP(ctx);
  if (!ip) return _URC_NO_REASON;
  if (state->skip > 0) {
    state->skip--;
    return _URC_NO_REASON;
  }
  if (state->frame_count >= ANT_CRASH_FRAME_MAX) return _URC_END_OF_STACK;
  state->frames[state->frame_count++] = ip;
  return _URC_NO_REASON;
}
#endif

typedef struct {
  char *buf;
  size_t cap;
  size_t len;
} crash_buf_t;

static void cb_putc(crash_buf_t *b, char c) {
  if (b->len + 1 >= b->cap) return;
  b->buf[b->len++] = c;
  b->buf[b->len] = '\0';
}

static void cb_puts(crash_buf_t *b, const char *s) {
  if (!s) return;
  while (*s && b->len + 1 < b->cap) b->buf[b->len++] = *s++;
  if (b->cap) b->buf[b->len] = '\0';
}

static void cb_put_uint(crash_buf_t *b, unsigned long long v) {
  char tmp[32];
  int i = (int)sizeof(tmp);
  tmp[--i] = '\0';
  if (v == 0) tmp[--i] = '0';
  while (v && i > 0) {
    tmp[--i] = (char)('0' + (v % 10));
    v /= 10;
  }
  cb_puts(b, &tmp[i]);
}

static void cb_put_hex(crash_buf_t *b, uint64_t v) {
  char tmp[32];
  int i = (int)sizeof(tmp);
  tmp[--i] = '\0';
  if (v == 0) tmp[--i] = '0';
  while (v && i > 0) {
    unsigned d = (unsigned)(v & 0xf);
    tmp[--i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    v >>= 4;
  }
  cb_puts(b, "0x");
  cb_puts(b, &tmp[i]);
}

static void cb_put_json_string(crash_buf_t *b, const char *s) {
  static const char hexdigits[] = "0123456789abcdef";
  cb_putc(b, '"');

  if (s) for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
  switch (*p) {
    case '"': cb_puts(b, "\\\""); break;
    case '\\': cb_puts(b, "\\\\"); break;
    case '\b': cb_puts(b, "\\b"); break;
    case '\f': cb_puts(b, "\\f"); break;
    case '\n': cb_puts(b, "\\n"); break;
    case '\r': cb_puts(b, "\\r"); break;
    case '\t': cb_puts(b, "\\t"); break;
    default: if (*p < 0x20) {
      cb_puts(b, "\\u00");
      cb_putc(b, hexdigits[*p >> 4]);
      cb_putc(b, hexdigits[*p & 0xf]);
    } else cb_putc(b, (char)*p);
  }}

  cb_putc(b, '"');
}

static const char *path_basename_const(const char *path) {
  if (!path) return "";
  const char *base = path;
  for (const char *p = path; *p; p++) {
    if (*p == '/' || *p == '\\') base = p + 1;
  }
  return base;
}

static void init_argv_strings(int argc, char **argv) {
  crash_buf_t payload = { crash_argv, sizeof(crash_argv), 0 };
  int limit = argc < 8 ? argc : 8;
  for (int i = 0; i < limit; i++) {
    if (i) cb_putc(&payload, ',');
    cb_put_json_string(&payload, argv[i] ? path_basename_const(argv[i]) : "");
  }
  if (argc > limit) {
    if (limit > 0) cb_putc(&payload, ',');
    cb_put_json_string(&payload, "...");
  }
}

static void init_report_controls() {
  crash_print_trace = ant_env_bool(getenv("ANT_CRASH_TRACE"), false);
  const char *enabled = getenv("ANT_ENABLE_CRASH_REPORTING");
  crash_reporting_enabled = enabled
    ? ant_env_bool(enabled, true)
    : !ant_env_bool(getenv("DO_NOT_TRACK"), false);
}

static void init_exe_path(int argc, char **argv) {
#ifdef _WIN32
  DWORD len = GetModuleFileNameA(NULL, crash_exe_path, (DWORD)sizeof(crash_exe_path));
  if (len > 0 && len < sizeof(crash_exe_path)) return;
#else
#ifdef __APPLE__
  uint32_t size = (uint32_t)sizeof(crash_exe_path);
  char tmp[ANT_CRASH_EXE_PATH_MAX];
  if (_NSGetExecutablePath(tmp, &size) == 0) {
    char *resolved = realpath(tmp, crash_exe_path);
    if (resolved) return;
    strncpy(crash_exe_path, tmp, sizeof(crash_exe_path) - 1);
    crash_exe_path[sizeof(crash_exe_path) - 1] = '\0';
    return;
  }
#elif defined(__linux__)
  ssize_t len = readlink("/proc/self/exe", crash_exe_path, sizeof(crash_exe_path) - 1);
  if (len > 0) {
    crash_exe_path[len] = '\0';
    return;
  }
#endif
#endif
  if (argc > 0 && argv && argv[0]) {
    strncpy(crash_exe_path, argv[0], sizeof(crash_exe_path) - 1);
    crash_exe_path[sizeof(crash_exe_path) - 1] = '\0';
  }
}

static const char *os_name(void) {
#ifdef _WIN32
  return "Windows";
#elif defined(__APPLE__)
  return "macOS";
#elif defined(__linux__)
  return "Linux";
#else
  return "unknown-os";
#endif
}

static const char *os_version(void) {
  static char version[128];
  if (version[0]) return version;

#ifdef _WIN32
  OSVERSIONINFOEXA info;
  memset(&info, 0, sizeof(info));
  info.dwOSVersionInfoSize = sizeof(info);
  if (GetVersionExA((OSVERSIONINFOA *)&info)) {
    snprintf(
      version, sizeof(version), "%lu.%lu.%lu",
      (unsigned long)info.dwMajorVersion,
      (unsigned long)info.dwMinorVersion,
      (unsigned long)info.dwBuildNumber
    );
    return version;
  }
#elif defined(__APPLE__)
  size_t len = sizeof(version);
  if (sysctlbyname("kern.osproductversion", version, &len, NULL, 0) == 0 && version[0])
    return version;
#elif defined(__linux__)
  struct utsname info;
  if (uname(&info) == 0 && info.release[0]) {
    snprintf(version, sizeof(version), "%s", info.release);
    return version;
  }
#endif
  snprintf(version, sizeof(version), "unknown");
  return version;
}

static const char *os_display_name(void) {
  static char display[160];
  if (!display[0]) snprintf(display, sizeof(display), "%s v%s", os_name(), os_version());
  return display;
}

static const char *arch_name(void) {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  return "x64";
#elif defined(__i386__) || defined(_M_IX86)
  return "x86";
#else
  return "unknown-arch";
#endif
}

static uint64_t now_ms(void) {
#ifdef _WIN32
  return GetTickCount64();
#else
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
}

static unsigned long long peak_rss_bytes(void) {
#ifdef _WIN32
  return 0;
#else
  struct rusage ru;
  if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
#ifdef __APPLE__
  return (unsigned long long)ru.ru_maxrss;
#else
  return (unsigned long long)ru.ru_maxrss * 1024ULL;
#endif
#endif
}

static unsigned long long crash_process_id(void) {
#ifdef _WIN32
  return (unsigned long long)GetCurrentProcessId();
#else
  return (unsigned long long)getpid();
#endif
}

static bool crash_streq_len(const char *s, size_t len, const char *literal) {
  size_t literal_len = strlen(literal);
  return len == literal_len && strncmp(s, literal, literal_len) == 0;
}

static const char *crash_code_detail(const char *code, size_t len) {
  if (crash_streq_len(code, len, "SIGSEGV")) return "invalid memory access";
  if (crash_streq_len(code, len, "SIGBUS"))  return "bus error";
  if (crash_streq_len(code, len, "SIGFPE"))  return "floating point exception";
  if (crash_streq_len(code, len, "SIGILL"))  return "illegal instruction";
  if (crash_streq_len(code, len, "SIGABRT")) return "abort";
  
  if (crash_streq_len(code, len, "EXCEPTION_ACCESS_VIOLATION"))    return "invalid memory access";
  if (crash_streq_len(code, len, "EXCEPTION_STACK_OVERFLOW"))      return "stack overflow";
  if (crash_streq_len(code, len, "EXCEPTION_ILLEGAL_INSTRUCTION")) return "illegal instruction";
  
  return "fatal error";
}

static int crash_code_signal_number(const char *code, size_t len) {
#ifdef SIGSEGV
  if (crash_streq_len(code, len, "SIGSEGV")) return SIGSEGV;
#endif
#ifdef SIGBUS
  if (crash_streq_len(code, len, "SIGBUS")) return SIGBUS;
#endif
#ifdef SIGFPE
  if (crash_streq_len(code, len, "SIGFPE")) return SIGFPE;
#endif
#ifdef SIGILL
  if (crash_streq_len(code, len, "SIGILL")) return SIGILL;
#endif
#ifdef SIGABRT
  if (crash_streq_len(code, len, "SIGABRT")) return SIGABRT;
#endif
  return 0;
}

static const char *posix_signal_reason(int sig) {
switch (sig) {
  case SIGSEGV: return "Segmentation fault";
#ifdef SIGBUS
  case SIGBUS:  return "Bus error";
#endif
  case SIGFPE:  return "Floating point exception";
  case SIGILL:  return "Illegal instruction";
  case SIGABRT: return "Abort";
  default:      return "Fatal signal";
}}

static const char *posix_signal_code(int sig) {
switch (sig) {
  case SIGSEGV: return "SIGSEGV";
#ifdef SIGBUS
  case SIGBUS:  return "SIGBUS";
#endif
  case SIGFPE:  return "SIGFPE";
  case SIGILL:  return "SIGILL";
  case SIGABRT: return "SIGABRT";
  default:      return "SIGNAL";
}}

static void format_native_frame(char *out, size_t out_cap, uintptr_t addr) {
  if (out_cap == 0) return;
  out[0] = '\0';

#ifdef _WIN32
  HANDLE process = GetCurrentProcess();
  DWORD64 displacement = 0;
  DWORD64 base = SymGetModuleBase64(process, (DWORD64)addr);
  
  char module_path[MAX_PATH] = "";
  const char *image = "unknown";
  if (base && GetModuleFileNameA((HMODULE)(uintptr_t)base, module_path, (DWORD)sizeof(module_path)) > 0)
    image = path_basename_const(module_path);

  union {
    SYMBOL_INFO info;
    char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
  } symbol_buf;
  
  SYMBOL_INFO *symbol = &symbol_buf.info;
  memset(&symbol_buf, 0, sizeof(symbol_buf));
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = MAX_SYM_NAME;

  if (SymFromAddr(process, (DWORD64)addr, &displacement, symbol)) {
    snprintf(
      out, out_cap, "%s 0x%016llx %s + %llu", image,
      (unsigned long long)addr, symbol->Name, (unsigned long long)displacement);
    return;
  }

  snprintf(out, out_cap, "%s 0x%016llx", image, (unsigned long long)addr);
#else
  Dl_info info;
  memset(&info, 0, sizeof(info));

  if (dladdr((void *)addr, &info) && info.dli_fname) {
    const char *image = path_basename_const(info.dli_fname);
    if (info.dli_sname && info.dli_saddr) {
      unsigned long long offset = (unsigned long long)(addr - (uintptr_t)info.dli_saddr);
      snprintf(
        out, out_cap, "%s 0x%016llx %s + %llu",
        image, (unsigned long long)addr, info.dli_sname, offset);
      return;
    }
    if (info.dli_fbase) {
      unsigned long long offset = (unsigned long long)(addr - (uintptr_t)info.dli_fbase);
      snprintf(out, out_cap, "%s 0x%016llx + %llu", image, (unsigned long long)addr, offset);
      return;
    }
    snprintf(out, out_cap, "%s 0x%016llx", image, (unsigned long long)addr);
    return;
  }

  snprintf(out, out_cap, "0x%016llx", (unsigned long long)addr);
#endif
}

static size_t build_report_payload(
  char *payload_buf, size_t payload_cap,
  const char *kind, const char *code, const char *reason,
  uint64_t fault_addr, const uintptr_t *frames, int frame_count
) {
  crash_buf_t p = { payload_buf, payload_cap, 0 };
  cb_puts(&p, "{\"upload\":");
  cb_puts(&p, should_upload_report() ? "true" : "false");
  cb_puts(&p, ",\"trace\":");
  cb_puts(&p, crash_print_trace ? "true" : "false");
  cb_puts(&p, ",\"pid\":");
  cb_put_uint(&p, crash_process_id());
  cb_puts(&p, ",\"argv\":[");
  cb_puts(&p, crash_argv);
  cb_puts(&p, "],\"report\":{\"schema\":1,\"runtime\":\"ant\",\"version\":");
  cb_put_json_string(&p, ANT_VERSION);
  cb_puts(&p, ",\"target\":");
  cb_put_json_string(&p, ANT_TARGET_TRIPLE);
  cb_puts(&p, ",\"os\":");
  cb_put_json_string(&p, os_display_name());
  cb_puts(&p, ",\"arch\":");
  cb_put_json_string(&p, arch_name());
  cb_puts(&p, ",\"kind\":");
  cb_put_json_string(&p, kind);
  cb_puts(&p, ",\"code\":");
  cb_put_json_string(&p, code);
  cb_puts(&p, ",\"reason\":");
  cb_put_json_string(&p, reason);
  cb_puts(&p, ",\"addr\":");
  cb_putc(&p, '"');
  cb_put_hex(&p, fault_addr);
  cb_putc(&p, '"');
  cb_puts(&p, ",\"elapsedMs\":");
  cb_put_uint(&p, now_ms() - crash_start_ms);
  cb_puts(&p, ",\"peakRss\":");
  cb_put_uint(&p, peak_rss_bytes());
  cb_puts(&p, ",\"frames\":[");
  for (int i = 0; i < frame_count && i < ANT_CRASH_FRAME_MAX; i++) {
    if (i) cb_putc(&p, ',');
    char frame_text[384];
    format_native_frame(frame_text, sizeof(frame_text), frames[i]);
    cb_put_json_string(&p, frame_text);
  }
  cb_puts(&p, "]}}");

  return p.len;
}

static bool crash_stderr_is_tty(void) {
#ifdef _WIN32
  return _isatty(_fileno(stderr)) != 0;
#else
  return isatty(fileno(stderr));
#endif
}

#ifndef _WIN32
static void write_all_fd(int fd, const char *data, size_t len) {
while (len > 0) {
  ssize_t n = write(fd, data, len);
  if (n <= 0) return;
  data += n;
  len -= (size_t)n;
}}

static void spawn_reporter(const char *payload, size_t payload_len) {
  int stdin_pipe[2];
  if (pipe(stdin_pipe) != 0) return;

  pid_t pid = fork();
  if (pid != 0) {
    close(stdin_pipe[0]);
    if (pid > 0) write_all_fd(stdin_pipe[1], payload, payload_len);
    close(stdin_pipe[1]);
    if (pid > 0) {
      int status = 0;
      (void)waitpid(pid, &status, 0);
    }
    return;
  }

  close(stdin_pipe[1]);
  dup2(stdin_pipe[0], STDIN_FILENO);
  if (stdin_pipe[0] > STDERR_FILENO) close(stdin_pipe[0]);

  int null_out = open("/dev/null", O_WRONLY);
  if (null_out >= 0) {
    dup2(null_out, STDOUT_FILENO);
    if (null_out > STDERR_FILENO) close(null_out);
  }

  const char *exe = crash_exe_path[0] ? crash_exe_path : "ant";
  char *const reporter_argv[] = {
    (char *)exe,
    "__internal-crash-report",
    NULL
  };
  
  execv(exe, reporter_argv);
  execvp(exe, reporter_argv);
  _exit(0);
}
#else
static void spawn_reporter(const char *payload, size_t payload_len) {
  if (!crash_exe_path[0]) return;

  SECURITY_ATTRIBUTES sa;
  memset(&sa, 0, sizeof(sa));
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE read_pipe = NULL;
  HANDLE write_pipe = NULL;
  if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return;
  SetHandleInformation(write_pipe, HANDLE_FLAG_INHERIT, 0);

  HANDLE null_out = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

  char cmd[ANT_CRASH_EXE_PATH_MAX + 64] = "";
  snprintf(cmd, sizeof(cmd), "\"%s\" __internal-crash-report", crash_exe_path);

  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof(si));
  memset(&pi, 0, sizeof(pi));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = read_pipe;
  si.hStdOutput = null_out != INVALID_HANDLE_VALUE ? null_out : GetStdHandle(STD_OUTPUT_HANDLE);
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

  if (CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
    CloseHandle(read_pipe);
    DWORD written = 0;
    WriteFile(write_pipe, payload, (DWORD)payload_len, &written, NULL);
    CloseHandle(write_pipe);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  } else {
    CloseHandle(read_pipe);
    CloseHandle(write_pipe);
  }
  if (null_out != INVALID_HANDLE_VALUE) CloseHandle(null_out);
}
#endif

static void crash_report_print_upload_failed(void) {
  if (crash_report_status_inline) crfprintf(stderr, "\r\033[2K <red>Crash report upload failed.</red>\n");
  else crfprintf(stderr, "<red>Crash report upload failed.</red>\n");
}

static void crash_report_print_upload_error(const char *message) {
  crash_report_print_upload_failed();
  if (message && *message) fprintf(stderr, "%s\n", message);
}

static ant_value_t crash_report_noop(ant_t *js, ant_value_t *args, int nargs) {
  if (crash_report_status_printed) return js_mkundef();
  crash_report_status_printed = true;

  if (args && nargs > 0) crash_report_print_upload_error(js_str(js, args[0]));
  else crash_report_print_upload_failed();

  return js_mkundef();
}

static ant_value_t crash_report_print_url(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();

  size_t len = 0;
  const char *s = vtype(args[0]) == T_STR ? js_getstr(js, args[0], &len) : NULL;
  
  if (!s || len == 0) {
    if (!crash_report_status_printed) {
      crash_report_status_printed = true;
      crash_report_print_upload_failed();
    }
    return js_mkundef();
  }

  crash_report_status_printed = true;
  if (crash_report_status_inline) crfprintf(stderr, "\r\033[2K <cyan>%.*s</cyan>\n\n", (int)len, s);
  else crfprintf(stderr, "\n <cyan>%.*s</cyan>\n\n", (int)len, s);
  
  return args[0];
}

static ant_value_t crash_report_response_text(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();

  ant_value_t text_fn = js_getprop_fallback(js, args[0], "text");
  if (!is_callable(text_fn)) {
    if (!crash_report_status_printed) {
      crash_report_status_printed = true;
      crash_report_print_upload_failed();
      fputs("Response.text is not available.\n", stderr);
    }
    return js_mkundef();
  }

  ant_value_t text_promise = sv_vm_call(js->vm, js, text_fn, args[0], NULL, 0, NULL, false);
  if (is_err(text_promise)) return text_promise;

  ant_value_t print_promise = js_promise_then(
    js, text_promise,
    js_mkfun(crash_report_print_url),
    js_mkfun(crash_report_noop)
  );
  
  promise_mark_handled(text_promise);
  promise_mark_handled(print_promise);

  return js_mkundef();
}

static void crash_report_url(char *out, size_t out_cap) {
  const char *base = getenv("ANT_CRASH_REPORT_URL");
  if (!base || !*base) base = "https://js.report";

  size_t n = strlen(base);
  while (n > 0 && base[n - 1] == '/') n--;
  if (n >= out_cap) n = out_cap - 1;

  memcpy(out, base, n);
  out[n] = '\0';
  strncat(out, "/report", out_cap - strlen(out) - 1);
}

static char *crash_read_stdin(size_t *len) {
  size_t cap = 4096;
  *len = 0;
  char *buf = malloc(cap);
  if (!buf) return NULL;

  size_t n = 0;
  while ((n = fread(buf + *len, 1, cap - *len, stdin)) > 0) {
  *len += n;
  if (*len == cap) {
    cap *= 2;
    char *next = realloc(buf, cap);
    if (!next) {
      free(buf);
      return NULL;
    }
    buf = next;
  }}

  buf[*len] = '\0';
  return buf;
}

static ant_value_t crash_get(ant_t *js, ant_value_t obj, const char *key) {
  if (!is_object_type(obj)) return js_mkundef();
  return js_get(js, obj, key);
}

static const char *crash_get_string(ant_t *js, ant_value_t obj, const char *key, size_t *len, const char *fallback) {
  ant_value_t value = crash_get(js, obj, key);
  if (vtype(value) == T_STR) {
    const char *s = js_getstr(js, value, len);
    if (s) return s;
  }
  *len = strlen(fallback);
  return fallback;
}

static unsigned long long crash_get_uint(ant_t *js, ant_value_t obj, const char *key) {
  ant_value_t value = crash_get(js, obj, key);
  if (vtype(value) != T_NUM) return 0;
  double n = js_getnum(value);
  if (n <= 0) return 0;
  return (unsigned long long)n;
}

static void crash_format_bytes(char *out, size_t out_cap, unsigned long long bytes) {
  if (bytes == 0) {
    snprintf(out, out_cap, "unknown");
    return;
  }

  if (bytes >= 1024ULL * 1024ULL)
    snprintf(out, out_cap, "%lluMB", bytes / (1024ULL * 1024ULL));
  else if (bytes >= 1024ULL)
    snprintf(out, out_cap, "%lluKB", bytes / 1024ULL);
  else
    snprintf(out, out_cap, "%lluB", bytes);
}

static void crash_print_quoted_value(ant_t *js, ant_value_t value) {
  size_t len = 0;
  const char *s = NULL;
  
  if (vtype(value) == T_STR) s = js_getstr(js, value, &len);
  if (!s) {
    s = "<unknown>";
    len = strlen(s);
  }

  fputc('"', stderr);
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    if (c == '"' || c == '\\') fputc('\\', stderr);
    if (c == '\n' || c == '\r') c = ' ';
    fputc(c, stderr);
  }
  
  fputc('"', stderr);
}

static void crash_print_string_value(ant_t *js, ant_value_t value) {
  size_t len = 0;
  const char *s = NULL;
  
  if (vtype(value) == T_STR) s = js_getstr(js, value, &len);
  if (!s) {
    s = "<unknown>";
    len = strlen(s);
  }

  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    if (c == '\n' || c == '\r') c = ' ';
    fputc(c, stderr);
  }
}

static void crash_print_args(ant_t *js, ant_value_t argv) {
  crfprintf(stderr, "<dim>Args:");
  if (vtype(argv) != T_ARR || js_arr_len(js, argv) == 0) {
    fputs(" \"ant\"\n", stderr);
    return;
  }

  ant_offset_t len = js_arr_len(js, argv);
  for (ant_offset_t i = 0; i < len; i++) {
    fputc(' ', stderr);
    crash_print_quoted_value(js, js_arr_get(js, argv, i));
  }
  fputc('\n', stderr);
}

static void crash_print_frames(ant_t *js, ant_value_t report) {
  ant_value_t frames = crash_get(js, report, "frames");
  crfprintf(stderr, "<dim>Native backtrace:</>\n");
  if (vtype(frames) != T_ARR || js_arr_len(js, frames) == 0) {
    crfprintf(stderr, "  <dim>(no native frames were captured)</>\n\n");
    return;
  }

  ant_offset_t len = js_arr_len(js, frames);
  for (ant_offset_t i = 0; i < len; i++) {
    fputs("  ", stderr);
    fprintf(stderr, "%-2lld ", (long long)i);
    crash_print_string_value(js, js_arr_get(js, frames, i));
    fputc('\n', stderr);
  }
  fputc('\n', stderr);
}

static void crash_print_report_summary(ant_t *js, ant_value_t report, ant_value_t argv, bool upload, bool trace, unsigned long long pid) {
  size_t code_len = 0, addr_len = 0, reason_len = 0;
  
  const char *code = crash_get_string(js, report, "code", &code_len, "SIGNAL");
  const char *addr = crash_get_string(js, report, "addr", &addr_len, "0x0");
  const char *reason = crash_get_string(js, report, "reason", &reason_len, "Fatal signal");

  unsigned long long elapsed = crash_get_uint(js, report, "elapsedMs");
  unsigned long long peak_rss = crash_get_uint(js, report, "peakRss");
  
  const char *detail = crash_code_detail(code, code_len);
  int signal_number = crash_code_signal_number(code, code_len);

  char peak_rss_text[32];
  crash_format_bytes(peak_rss_text, sizeof(peak_rss_text), peak_rss);

  fprintf(stderr, "=== (%llu) ===================================================\n", pid);

  crfprintf(stderr, "<dim>Ant v%s (%s) %s</>\n", ant_semver(), ANT_GIT_HASH, ANT_TARGET_TRIPLE);
  crfprintf(stderr, "<dim>%s</>\n", os_display_name());

  crash_print_args(js, argv);

  crfprintf(stderr, "<dim>Summary: %s (%.*s) with signal %d</>\n", detail, (int)code_len, code, signal_number);
  crfprintf(stderr, "<dim>Elapsed: %llums | RSS Peak: %s</>\n\n", elapsed, peak_rss_text);
  crfprintf(stderr, "<red>panic</red><dim>(main thread):</> %.*s at address %.*s \n", (int)reason_len, reason, (int)addr_len, addr);

  crfprintf(stderr, "oh no<dim>:</> Ant has crashed. This indicates a bug in Ant, not your code.\n\n");
  if (trace) crash_print_frames(js, report);

  if (upload) {
    crfprintf(stderr, "To send a redacted crash report to Ant's team,\n");
    crfprintf(stderr, "please file a GitHub issue using the link below:\n\n");
    if (crash_report_status_inline) crfprintf(stderr, " <yellow>uploading...</>");
  }
  else crfprintf(stderr, "Crash reporting is disabled for this process.\n\n");
}

int ant_crash_run_internal_report(ant_t *js) {
  if (!js) return EXIT_FAILURE;

  size_t payload_len = 0;
  char *payload = crash_read_stdin(&payload_len);
  if (!payload) return EXIT_FAILURE;

  crash_report_status_printed = false;

  ant_value_t wrapper_json = js_mkstr(js, payload, payload_len);
  ant_value_t wrapper = json_parse_value(js, wrapper_json);

  if (is_err(wrapper) || !is_object_type(wrapper)) {
    crash_report_print_upload_failed();
    free(payload);
    return EXIT_FAILURE;
  }

  ant_value_t report = crash_get(js, wrapper, "report");
  if (!is_object_type(report)) {
    crash_report_print_upload_failed();
    free(payload);
    return EXIT_FAILURE;
  }

  bool upload = js_truthy(js, crash_get(js, wrapper, "upload"));
  bool trace = js_truthy(js, crash_get(js, wrapper, "trace"));
  
  unsigned long long pid = crash_get_uint(js, wrapper, "pid");
  ant_value_t argv = crash_get(js, wrapper, "argv");
  
  crash_report_status_inline = upload && crash_stderr_is_tty();
  crash_print_report_summary(js, report, argv, upload, trace, pid);

  if (!upload) {
    free(payload);
    return EXIT_SUCCESS;
  }

  ant_value_t report_json = js_json_stringify(js, &report, 1);
  if (vtype(report_json) != T_STR) {
    crash_report_print_upload_failed();
    free(payload);
    return EXIT_FAILURE;
  }

  size_t report_payload_len = 0;
  const char *report_payload = js_getstr(js, report_json, &report_payload_len);
  if (!report_payload) {
    crash_report_print_upload_failed();
    free(payload);
    return EXIT_FAILURE;
  }

  fflush(stderr);
  char url[512] = "";
  crash_report_url(url, sizeof(url));

  ant_value_t headers = js_mkobj(js);
  js_set(js, headers, "content-type", js_mkstr(js, "application/json", 16));

  ant_value_t init = js_mkobj(js);
  js_set(js, init, "headers", headers);
  js_set(js, init, "method", js_mkstr(js, "POST", 4));
  js_set(js, init, "body", js_mkstr(js, report_payload, report_payload_len));

  ant_value_t fetch_args[2] = { js_mkstr(js, url, strlen(url)), init };
  ant_value_t fetch_promise = ant_fetch(js, fetch_args, 2);

  if (is_err(fetch_promise)) {
    crash_report_status_printed = true;
    crash_report_print_upload_error(js_str(js, fetch_promise));
    free(payload);
    return EXIT_FAILURE;
  }

  ant_value_t report_promise = js_promise_then(
    js, fetch_promise,
    js_mkfun(crash_report_response_text),
    js_mkfun(crash_report_noop)
  );

  promise_mark_handled(fetch_promise);
  if (is_err(report_promise)) {
    crash_report_status_printed = true;
    crash_report_print_upload_error(js_str(js, report_promise));
    free(payload);
    return EXIT_FAILURE;
  }

  promise_mark_handled(report_promise);
  js_run_event_loop(js);
  free(payload);
  
  return EXIT_SUCCESS;
}

void ant_crash_suppress_reporting(void) {
  crash_reporting_suppressed = 1;
}

#ifndef _WIN32
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

static void crash_handler(int sig, siginfo_t *info, void *ucontext) {
  struct sigaction dfl;
  memset(&dfl, 0, sizeof(dfl));
  
  dfl.sa_handler = SIG_DFL;
  sigemptyset(&dfl.sa_mask);
  sigaction(sig, &dfl, NULL);

  uintptr_t frames[ANT_CRASH_FRAME_MAX] = {0};
  int frame_count = 0;
#ifdef ANT_CRASH_HAVE_EXECINFO
  void *raw_frames[64];
  int n = backtrace(raw_frames, (int)(sizeof(raw_frames) / sizeof(raw_frames[0])));
  int skip = n > 1 ? 1 : 0;
  for (int i = skip; i < n && frame_count < ANT_CRASH_FRAME_MAX; i++)
    frames[frame_count++] = (uintptr_t)raw_frames[i];
#elif defined(ANT_CRASH_HAVE_UNWIND_BACKTRACE)
  (void)ucontext;
  crash_unwind_state_t state = { frames, 0, 1 };
  _Unwind_Backtrace(collect_unwind_frame, &state);
  frame_count = state.frame_count;
#endif

  uint64_t fault_addr = info ? (uint64_t)(uintptr_t)info->si_addr : 0;
  char payload[ANT_CRASH_PAYLOAD_MAX] = "";
  const char *reason = posix_signal_reason(sig);
  const char *code = posix_signal_code(sig);
  
  size_t payload_len = build_report_payload(
    payload, sizeof(payload), "signal", code, 
    reason, fault_addr, frames, frame_count
  );

  spawn_reporter(payload, payload_len);
#ifdef SIGTRAP
  struct sigaction trap_dfl;
  memset(&trap_dfl, 0, sizeof(trap_dfl));
  trap_dfl.sa_handler = SIG_DFL;
  sigemptyset(&trap_dfl.sa_mask);
  sigaction(SIGTRAP, &trap_dfl, NULL);
  raise(SIGTRAP);
#else
  raise(sig);
#endif
}

void ant_crash_init(int argc, char **argv) {
  crash_start_ms = now_ms();
  init_exe_path(argc, argv);
  init_argv_strings(argc, argv);
  init_report_controls();

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
#ifdef SA_ONSTACK
  if (install_altstack()) sa.sa_flags |= SA_ONSTACK;
#endif
  sigemptyset(&sa.sa_mask);
  static const int sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };
  for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) sigaction(sigs[i], &sa, NULL);
}

#else // _WIN32
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

static const char *exception_reason(DWORD code) {
switch (code) {
  case EXCEPTION_ACCESS_VIOLATION: return "Segmentation fault";
  case EXCEPTION_ILLEGAL_INSTRUCTION: return "Illegal instruction";
  case EXCEPTION_STACK_OVERFLOW: return "Stack overflow";
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
  case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "Divide by zero";
  default: return "Fatal exception";
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

static int collect_windows_frames(EXCEPTION_POINTERS *exc, uintptr_t *frames, int max_frames) {
  if (!exc || !exc->ContextRecord) return 0;
  HANDLE process = GetCurrentProcess();
  HANDLE thread = GetCurrentThread();
  CONTEXT ctx = *exc->ContextRecord;
  STACKFRAME64 frame;
  DWORD machine;
  if (!init_stack_frame(&ctx, &frame, &machine)) return 0;

  int count = 0;
  while (count < max_frames) {
    DWORD64 addr = frame.AddrPC.Offset;
    if (addr == 0) break;
    frames[count++] = (uintptr_t)addr;
    DWORD64 prev_pc = frame.AddrPC.Offset;
    DWORD64 prev_sp = frame.AddrStack.Offset;
    BOOL ok = StackWalk64(
      machine, process, thread, &frame, &ctx, NULL,
      SymFunctionTableAccess64, SymGetModuleBase64, NULL
    );
    if (!ok || (frame.AddrPC.Offset == prev_pc && frame.AddrStack.Offset == prev_sp)) break;
  }
  return count;
}

static LONG WINAPI windows_crash_handler(EXCEPTION_POINTERS *exc) {
  if (InterlockedExchange(&crash_in_progress, 1) != 0)
    return EXCEPTION_CONTINUE_SEARCH;

  HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
  SymInitialize(process, NULL, TRUE);

  EXCEPTION_RECORD *record = exc ? exc->ExceptionRecord : NULL;
  DWORD code = record ? record->ExceptionCode : 0;
  uint64_t fault_addr = (uint64_t)exception_fault_address(record);

  uintptr_t frames[ANT_CRASH_FRAME_MAX] = {0};
  int frame_count = collect_windows_frames(exc, frames, ANT_CRASH_FRAME_MAX);
  char payload[ANT_CRASH_PAYLOAD_MAX] = "";
  
  size_t payload_len = build_report_payload(
    payload, sizeof(payload), "exception", exception_name(code), 
    exception_reason(code), fault_addr, frames, frame_count
  );

  spawn_reporter(payload, payload_len);
  if (previous_filter) return previous_filter(exc);
  return EXCEPTION_EXECUTE_HANDLER;
}

void ant_crash_init(int argc, char **argv) {
  crash_start_ms = now_ms();
  init_exe_path(argc, argv);
  init_argv_strings(argc, argv);
  init_report_controls();
  previous_filter = SetUnhandledExceptionFilter(windows_crash_handler);
}

#endif
