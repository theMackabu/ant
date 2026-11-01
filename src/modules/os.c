#include <compat.h> // IWYU pragma: keep

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <lmcons.h>
#ifdef _MSC_VER
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#endif
#else
#include <signal.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <pwd.h>
#include <dlfcn.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#ifdef __APPLE__
#include <net/if_dl.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#elif defined(__linux__)
#include <netpacket/packet.h>
#include <sys/sysinfo.h>
#endif
#endif

#include "ant.h"
#include "cfunc.h"
#include "gc.h"
#include "errors.h"
#include "internal.h"
#include "gc/roots.h"

#ifdef _WIN32
#define OS_EOL "\r\n"
#define OS_DEVNULL "\\\\.\\nul"
#else
#define OS_EOL "\n"
#define OS_DEVNULL "/dev/null"
#endif

static ant_value_t os_arch(ant_params_t) {
  (void)args; (void)nargs;
#if defined(__x86_64__) || defined(_M_X64)
  return js_mkstr(js, "x64", 3);
#elif defined(__i386__) || defined(_M_IX86)
  return js_mkstr(js, "ia32", 4);
#elif defined(__aarch64__) || defined(_M_ARM64)
  return js_mkstr(js, "arm64", 5);
#elif defined(__arm__) || defined(_M_ARM)
  return js_mkstr(js, "arm", 3);
#elif defined(__riscv) && __riscv_xlen == 64
  return js_mkstr(js, "riscv64", 7);
#elif defined(__ppc64__) || defined(__PPC64__)
  return js_mkstr(js, "ppc64", 5);
#elif defined(__s390x__)
  return js_mkstr(js, "s390x", 5);
#elif defined(__mips64)
  return js_mkstr(js, "mips64", 6);
#elif defined(__mips__)
  return js_mkstr(js, "mipsel", 6);
#elif defined(__loongarch64)
  return js_mkstr(js, "loong64", 7);
#else
  return js_mkstr(js, "unknown", 7);
#endif
}

static ant_value_t os_platform(ant_params_t) {
  (void)args; (void)nargs;
#if defined(__APPLE__)
  return js_mkstr(js, "darwin", 6);
#elif defined(__linux__)
  return js_mkstr(js, "linux", 5);
#elif defined(_WIN32) || defined(_WIN64)
  return js_mkstr(js, "win32", 5);
#elif defined(__FreeBSD__)
  return js_mkstr(js, "freebsd", 7);
#elif defined(__OpenBSD__)
  return js_mkstr(js, "openbsd", 7);
#elif defined(__sun)
  return js_mkstr(js, "sunos", 5);
#elif defined(_AIX)
  return js_mkstr(js, "aix", 3);
#else
  return js_mkstr(js, "unknown", 7);
#endif
}

static ant_value_t os_type(ant_params_t) {
  (void)args; (void)nargs;
#if defined(__APPLE__)
  return js_mkstr(js, "Darwin", 6);
#elif defined(__linux__)
  return js_mkstr(js, "Linux", 5);
#elif defined(_WIN32) || defined(_WIN64)
  return js_mkstr(js, "Windows_NT", 10);
#elif defined(__FreeBSD__)
  return js_mkstr(js, "FreeBSD", 7);
#elif defined(__OpenBSD__)
  return js_mkstr(js, "OpenBSD", 7);
#else
  return js_mkstr(js, "Unknown", 7);
#endif
}

static ant_value_t os_release(ant_params_t) {
  (void)args; (void)nargs;
#ifdef _WIN32
  OSVERSIONINFOA osvi;
  ZeroMemory(&osvi, sizeof(OSVERSIONINFOA));
  osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOA);
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  if (GetVersionExA(&osvi)) {
  #pragma GCC diagnostic pop
    char release[64];
    snprintf(release, sizeof(release), "%lu.%lu.%lu", osvi.dwMajorVersion, osvi.dwMinorVersion, osvi.dwBuildNumber);
    return js_mkstr(js, release, strlen(release));
  }
  return js_mkstr(js, "", 0);
#else
  struct utsname info;
  if (uname(&info) == 0) {
    return js_mkstr(js, info.release, strlen(info.release));
  }
  return js_mkstr(js, "", 0);
#endif
}

static ant_value_t os_version(ant_params_t) {
  (void)args; (void)nargs;
#ifdef _WIN32
  OSVERSIONINFOA osvi;
  ZeroMemory(&osvi, sizeof(OSVERSIONINFOA));
  osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOA);
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  if (GetVersionExA(&osvi)) {
  #pragma GCC diagnostic pop
    char version[256];
    snprintf(version, sizeof(version), "Windows %lu.%lu", osvi.dwMajorVersion, osvi.dwMinorVersion);
    return js_mkstr(js, version, strlen(version));
  }
  return js_mkstr(js, "", 0);
#else
  struct utsname info;
  if (uname(&info) == 0) {
    return js_mkstr(js, info.version, strlen(info.version));
  }
  return js_mkstr(js, "", 0);
#endif
}

static ant_value_t os_machine(ant_params_t) {
  (void)args; (void)nargs;
#ifdef _WIN32
  SYSTEM_INFO sysinfo;
  GetSystemInfo(&sysinfo);
  switch (sysinfo.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: return js_mkstr(js, "x86_64", 6);
    case PROCESSOR_ARCHITECTURE_ARM64: return js_mkstr(js, "aarch64", 7);
    case PROCESSOR_ARCHITECTURE_INTEL: return js_mkstr(js, "i686", 4);
    case PROCESSOR_ARCHITECTURE_ARM: return js_mkstr(js, "arm", 3);
    default: return js_mkstr(js, "unknown", 7);
  }
#else
  struct utsname info;
  if (uname(&info) == 0) {
    return js_mkstr(js, info.machine, strlen(info.machine));
  }
  return js_mkstr(js, "", 0);
#endif
}

static ant_value_t os_hostname(ant_params_t) {
  (void)args; (void)nargs;
  char hostname[256];
  if (gethostname(hostname, sizeof(hostname)) == 0) {
    return js_mkstr(js, hostname, strlen(hostname));
  }
  return js_mkstr(js, "", 0);
}

static ant_value_t os_homedir(ant_params_t) {
  (void)args; (void)nargs;
#ifdef _WIN32
  const char *userprofile = getenv("USERPROFILE");
  if (userprofile) {
    return js_mkstr(js, userprofile, strlen(userprofile));
  }
  const char *homedrive = getenv("HOMEDRIVE");
  const char *homepath = getenv("HOMEPATH");
  if (homedrive && homepath) {
    static char home[MAX_PATH];
    snprintf(home, sizeof(home), "%s%s", homedrive, homepath);
    return js_mkstr(js, home, strlen(home));
  }
  return js_mkstr(js, "", 0);
#else
  const char *home = getenv("HOME");
  if (home) {
    return js_mkstr(js, home, strlen(home));
  }
  struct passwd *pw = getpwuid(getuid());
  if (pw && pw->pw_dir) {
    return js_mkstr(js, pw->pw_dir, strlen(pw->pw_dir));
  }
  return js_mkstr(js, "", 0);
#endif
}

static ant_value_t os_tmpdir(ant_params_t) {
  (void)args; (void)nargs;
  const char *tmpdir = getenv("TMPDIR");
  if (!tmpdir) tmpdir = getenv("TMP");
  if (!tmpdir) tmpdir = getenv("TEMP");
  if (!tmpdir) tmpdir = "/tmp";
  return js_mkstr(js, tmpdir, strlen(tmpdir));
}

static ant_value_t os_endianness(ant_params_t) {
  (void)args; (void)nargs;
  uint16_t test = 1;
  if (*(uint8_t *)&test == 1) {
    return js_mkstr(js, "LE", 2);
  }
  return js_mkstr(js, "BE", 2);
}

static ant_value_t os_uptime(ant_params_t) {
  (void)args; (void)nargs;
#ifdef _WIN32
  return Ant_Value_FromNumber((double)GetTickCount64() / 1000.0);
#elif defined(__APPLE__)
  struct timeval boottime;
  size_t len = sizeof(boottime);
  int mib[2] = { CTL_KERN, KERN_BOOTTIME };
  if (sysctl(mib, 2, &boottime, &len, NULL, 0) == 0) {
    time_t now = time(NULL);
    return Ant_Value_FromNumber((double)(now - boottime.tv_sec));
  }
  return Ant_Value_FromNumber(0);
#elif defined(__linux__)
  struct sysinfo info;
  if (sysinfo(&info) == 0) {
    return Ant_Value_FromNumber((double)info.uptime);
  }
  return Ant_Value_FromNumber(0);
#else
  return Ant_Value_FromNumber(0);
#endif
}

static ant_value_t os_totalmem(ant_params_t) {
  (void)args; (void)nargs;
#ifdef _WIN32
  MEMORYSTATUSEX memInfo;
  memInfo.dwLength = sizeof(MEMORYSTATUSEX);
  if (GlobalMemoryStatusEx(&memInfo)) {
    return Ant_Value_FromNumber((double)memInfo.ullTotalPhys);
  }
  return Ant_Value_FromNumber(0);
#elif defined(__APPLE__)
  int64_t memsize;
  size_t len = sizeof(memsize);
  if (sysctlbyname("hw.memsize", &memsize, &len, NULL, 0) == 0) {
    return Ant_Value_FromNumber((double)memsize);
  }
  return Ant_Value_FromNumber(0);
#elif defined(__linux__)
  struct sysinfo info;
  if (sysinfo(&info) == 0) {
    return Ant_Value_FromNumber((double)info.totalram * info.mem_unit);
  }
  return Ant_Value_FromNumber(0);
#else
  return Ant_Value_FromNumber(0);
#endif
}

static ant_value_t os_freemem(ant_params_t) {
  (void)args; (void)nargs;
#ifdef _WIN32
  MEMORYSTATUSEX memInfo;
  memInfo.dwLength = sizeof(MEMORYSTATUSEX);
  if (GlobalMemoryStatusEx(&memInfo)) {
    return Ant_Value_FromNumber((double)memInfo.ullAvailPhys);
  }
  return Ant_Value_FromNumber(0);
#elif defined(__APPLE__)
  vm_size_t page_size;
  mach_port_t mach_port = mach_host_self();
  vm_statistics64_data_t vm_stats;
  mach_msg_type_number_t count = sizeof(vm_stats) / sizeof(natural_t);
  
  if (host_page_size(mach_port, &page_size) == KERN_SUCCESS &&
      host_statistics64(mach_port, HOST_VM_INFO64, (host_info64_t)&vm_stats, &count) == KERN_SUCCESS) {
    return Ant_Value_FromNumber((double)vm_stats.free_count * (double)page_size);
  }
  return Ant_Value_FromNumber(0);
#elif defined(__linux__)
  struct sysinfo info;
  if (sysinfo(&info) == 0) {
    return Ant_Value_FromNumber((double)info.freeram * info.mem_unit);
  }
  return Ant_Value_FromNumber(0);
#else
  return Ant_Value_FromNumber(0);
#endif
}

static ant_value_t os_availableParallelism(ant_params_t) {
  (void)args; (void)nargs;
#ifdef _WIN32
  SYSTEM_INFO sysinfo;
  GetSystemInfo(&sysinfo);
  return Ant_Value_FromNumber((double)sysinfo.dwNumberOfProcessors);
#elif defined(__APPLE__)
  int count;
  size_t size = sizeof(count);
  if (sysctlbyname("hw.ncpu", &count, &size, NULL, 0) == 0) {
    return Ant_Value_FromNumber((double)count);
  }
  return Ant_Value_FromNumber(1);
#elif defined(__linux__)
  long count = sysconf(_SC_NPROCESSORS_ONLN);
  return Ant_Value_FromNumber(count > 0 ? (double)count : 1);
#else
  return Ant_Value_FromNumber(1);
#endif
}

typedef struct {
  double user, nice, sys, idle, irq;
} cpu_times_t;

enum { CPU_MODEL, CPU_SPEED, CPU_TIMES };
enum { CPU_TIME_USER, CPU_TIME_NICE, CPU_TIME_SYS, CPU_TIME_IDLE, CPU_TIME_IRQ };

static ant_value_t os_new_cpu_record(ant_t *js) {
  ant_value_t seed = js->mutable_roots.os_cpu_template;
  if (vtype(seed) != kTypeObject) {
    seed = Ant_Object_Create(js);
    if (is_err(seed)) return seed;
    GC_ROOT_SAVE(mark, js);
    GC_ROOT_PIN(js, seed);
    js_mkprop_fast(js, seed, "model", 5, Ant_Value_Undefined);
    js_mkprop_fast(js, seed, "speed", 5, Ant_Value_Undefined);
    js_mkprop_fast(js, seed, "times", 5, Ant_Value_Undefined);
    GC_ROOT_RESTORE(js, mark);
    if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
    js->mutable_roots.os_cpu_template = seed;
  }
  return js_mkobj_from_template(js, seed);
}

static ant_value_t os_new_cpu_times(ant_t *js) {
  ant_value_t seed = js->mutable_roots.os_cpu_times_template;
  if (vtype(seed) != kTypeObject) {
    seed = Ant_Object_Create(js);
    if (is_err(seed)) return seed;
    GC_ROOT_SAVE(mark, js);
    GC_ROOT_PIN(js, seed);
    js_mkprop_fast(js, seed, "user", 4, Ant_Value_Undefined);
    js_mkprop_fast(js, seed, "nice", 4, Ant_Value_Undefined);
    js_mkprop_fast(js, seed, "sys", 3, Ant_Value_Undefined);
    js_mkprop_fast(js, seed, "idle", 4, Ant_Value_Undefined);
    js_mkprop_fast(js, seed, "irq", 3, Ant_Value_Undefined);
    GC_ROOT_RESTORE(js, mark);
    if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
    js->mutable_roots.os_cpu_times_template = seed;
  }
  return js_mkobj_from_template(js, seed);
}

static ant_value_t push_cpu_entry(ant_t *js, ant_value_t arr, const char *model, double speed, cpu_times_t *times) {
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, arr);
  
  ant_value_t cpu = os_new_cpu_record(js);
  if (is_err(cpu)) { GC_ROOT_RESTORE(js, mark); return cpu; }
  GC_ROOT_PIN(js, cpu);
  
  ant_value_t t = os_new_cpu_times(js);
  if (is_err(t)) { GC_ROOT_RESTORE(js, mark); return t; }
  GC_ROOT_PIN(js, t);
  
  ant_object_t *timing = js_obj_ptr(t);
  ant_object_prop_set_unchecked(timing, CPU_TIME_USER, Ant_Value_FromNumber(times->user));
  ant_object_prop_set_unchecked(timing, CPU_TIME_NICE, Ant_Value_FromNumber(times->nice));
  ant_object_prop_set_unchecked(timing, CPU_TIME_SYS, Ant_Value_FromNumber(times->sys));
  ant_object_prop_set_unchecked(timing, CPU_TIME_IDLE, Ant_Value_FromNumber(times->idle));
  ant_object_prop_set_unchecked(timing, CPU_TIME_IRQ, Ant_Value_FromNumber(times->irq));

  ant_value_t name = js_mkstr(js, model, strlen(model));
  if (is_err(name)) { GC_ROOT_RESTORE(js, mark); return name; }
  
  ant_object_t *obj = js_obj_ptr(cpu);
  ant_object_prop_set_unchecked(obj, CPU_MODEL, name);
  gc_write_barrier(js, obj, name);
  ant_object_prop_set_unchecked(obj, CPU_SPEED, Ant_Value_FromNumber(speed));
  ant_object_prop_set_unchecked(obj, CPU_TIMES, t);
  
  gc_write_barrier(js, obj, t);
  js_arr_push(js, arr, cpu);
  GC_ROOT_RESTORE(js, mark);
  
  return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : Ant_Value_Undefined;
}

#ifdef __APPLE__
static ant_value_t os_cpus_darwin(ant_t *js) {
  ant_value_t arr = Ant_Array_Create(js);
  
  natural_t ncpu;
  processor_info_array_t cpu_info;
  mach_msg_type_number_t info_count;
  
  if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &ncpu, &cpu_info, &info_count) != KERN_SUCCESS)
    return arr;
  
  char model[256] = "Unknown";
  size_t len = sizeof(model);
  sysctlbyname("machdep.cpu.brand_string", model, &len, NULL, 0);
  
  uint64_t freq = 0;
  len = sizeof(freq);
  if (sysctlbyname("hw.cpufrequency", &freq, &len, NULL, 0) != 0)
    freq = 2400000000;
  double speed = (double)freq / 1000000.0;
  
  processor_cpu_load_info_data_t *load = (processor_cpu_load_info_data_t *)cpu_info;
  
  for (natural_t i = 0; i < ncpu; i++) {
    cpu_times_t times = {
      .user = (double)load[i].cpu_ticks[CPU_STATE_USER] * 10,
      .nice = (double)load[i].cpu_ticks[CPU_STATE_NICE] * 10,
      .sys  = (double)load[i].cpu_ticks[CPU_STATE_SYSTEM] * 10,
      .idle = (double)load[i].cpu_ticks[CPU_STATE_IDLE] * 10,
      .irq  = 0
    };
    
    ant_value_t result = push_cpu_entry(js, arr, model, speed, &times);
    if (is_err(result)) {
      vm_deallocate(mach_task_self(), (vm_address_t)cpu_info, info_count * sizeof(integer_t));
      return result;
    }
  }
  
  vm_deallocate(mach_task_self(), (vm_address_t)cpu_info, info_count * sizeof(integer_t));
  return arr;
}
#endif

#ifdef __linux__
static char *parse_colon_value(char *line) {
  char *colon = strchr(line, ':');
  if (!colon) return NULL;
  colon++;
  while (*colon == ' ' || *colon == '\t') colon++;
  size_t len = strlen(colon);
  if (len > 0 && colon[len-1] == '\n') colon[len-1] = '\0';
  return colon;
}

static ant_value_t os_cpus_linux(ant_t *js) {
  ant_value_t arr = Ant_Array_Create(js);
  char model[256] = "Unknown";
  double speed = 0;
  int ncpu = 0;
  
  FILE *fp = fopen("/proc/cpuinfo", "r");
  if (!fp) goto read_stat;
  
  char line[256];
  while (fgets(line, sizeof(line), fp)) {
    if (strncmp(line, "processor", 9) == 0) { ncpu++; continue; }
    if (strncmp(line, "model name", 10) == 0) {
      char *val = parse_colon_value(line);
      if (val) strncpy(model, val, sizeof(model) - 1);
      continue;
    }
    if (strncmp(line, "cpu MHz", 7) == 0) {
      char *val = parse_colon_value(line);
      if (val) speed = atof(val);
    }
  }
  fclose(fp);
  
read_stat:
  fp = fopen("/proc/stat", "r");
  if (!fp) return arr;
  
  int cpu_idx = 0;
  while (fgets(line, sizeof(line), fp) && cpu_idx < ncpu) {
    if (strncmp(line, "cpu", 3) != 0 || line[3] < '0' || line[3] > '9') continue;
    
    unsigned long user, nice, sys, idle, iowait, irq, softirq;
    if (sscanf(line, "cpu%*d %lu %lu %lu %lu %lu %lu %lu", &user, &nice, &sys, &idle, &iowait, &irq, &softirq) < 4)
      continue;
    
    cpu_times_t times = {
      .user = (double)user * 10,
      .nice = (double)nice * 10,
      .sys  = (double)sys * 10,
      .idle = (double)idle * 10,
      .irq  = (double)(irq + softirq) * 10
    };
    
    ant_value_t result = push_cpu_entry(js, arr, model, speed, &times);
    if (is_err(result)) { fclose(fp); return result; }
    cpu_idx++;
  }
  
  fclose(fp);
  return arr;
}
#endif

static ant_value_t os_cpus(ant_params_t) {
  (void)args; (void)nargs;
#ifdef __APPLE__
  return os_cpus_darwin(js);
#elif defined(__linux__)
  return os_cpus_linux(js);
#else
  return Ant_Array_Create(js);
#endif
}

static ant_value_t os_loadavg(ant_params_t) {
  (void)args; (void)nargs;
  ant_value_t arr = Ant_Array_Create(js);
  
#if defined(__APPLE__) || defined(__linux__)
  double loadavg[3];
  if (getloadavg(loadavg, 3) == 3) {
    js_arr_push(js, arr, Ant_Value_FromNumber(loadavg[0]));
    js_arr_push(js, arr, Ant_Value_FromNumber(loadavg[1]));
    js_arr_push(js, arr, Ant_Value_FromNumber(loadavg[2]));
  } else {
    js_arr_push(js, arr, Ant_Value_FromNumber(0));
    js_arr_push(js, arr, Ant_Value_FromNumber(0));
    js_arr_push(js, arr, Ant_Value_FromNumber(0));
  }
#else
  js_arr_push(js, arr, Ant_Value_FromNumber(0));
  js_arr_push(js, arr, Ant_Value_FromNumber(0));
  js_arr_push(js, arr, Ant_Value_FromNumber(0));
#endif
  
  return arr;
}

#ifdef _WIN32

static ant_value_t os_networkInterfaces(ant_params_t) {
  (void)args; (void)nargs;
  ant_value_t result = Ant_Object_Create(js);
  
  ULONG bufLen = 15000;
  PIP_ADAPTER_ADDRESSES pAddresses = NULL;
  PIP_ADAPTER_ADDRESSES pCurrAddresses = NULL;
  PIP_ADAPTER_UNICAST_ADDRESS pUnicast = NULL;
  
  pAddresses = (IP_ADAPTER_ADDRESSES *)malloc(bufLen);
  if (!pAddresses) return result;
  
  ULONG ret = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, NULL, pAddresses, &bufLen);
  if (ret == ERROR_BUFFER_OVERFLOW) {
    free(pAddresses);
    pAddresses = (IP_ADAPTER_ADDRESSES *)malloc(bufLen);
    if (!pAddresses) return result;
    ret = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, NULL, pAddresses, &bufLen);
  }
  
  if (ret != NO_ERROR) {
    free(pAddresses);
    return result;
  }
  
  for (pCurrAddresses = pAddresses; pCurrAddresses; pCurrAddresses = pCurrAddresses->Next) {
    ant_value_t iface_arr = js_get(js, result, pCurrAddresses->AdapterName);
    if (!is_special_object(iface_arr)) {
      iface_arr = Ant_Array_Create(js);
      js_set(js, result, pCurrAddresses->AdapterName, iface_arr);
    }
    
    char mac_str[18] = "00:00:00:00:00:00";
    if (pCurrAddresses->PhysicalAddressLength == 6) {
      snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
        pCurrAddresses->PhysicalAddress[0], pCurrAddresses->PhysicalAddress[1],
        pCurrAddresses->PhysicalAddress[2], pCurrAddresses->PhysicalAddress[3],
        pCurrAddresses->PhysicalAddress[4], pCurrAddresses->PhysicalAddress[5]);
    }
    
    bool internal = (pCurrAddresses->IfType == IF_TYPE_SOFTWARE_LOOPBACK);
    
    for (pUnicast = pCurrAddresses->FirstUnicastAddress; pUnicast; pUnicast = pUnicast->Next) {
      ant_value_t entry = Ant_Object_Create(js);
      js_set(js, entry, "mac", js_mkstr(js, mac_str, strlen(mac_str)));
      js_set(js, entry, "internal", Ant_Value_FromBool(internal));
      
      char addr_str[INET6_ADDRSTRLEN] = "";
      struct sockaddr *sa = pUnicast->Address.lpSockaddr;
      
      if (sa->sa_family == AF_INET) {
        struct sockaddr_in *sa4 = (struct sockaddr_in *)sa;
        inet_ntop(AF_INET, &sa4->sin_addr, addr_str, sizeof(addr_str));
        js_set(js, entry, "address", js_mkstr(js, addr_str, strlen(addr_str)));
        js_set(js, entry, "family", js_mkstr(js, "IPv4", 4));
        js_set(js, entry, "netmask", js_mkstr(js, "", 0));
        js_set(js, entry, "cidr", js_mkstr(js, addr_str, strlen(addr_str)));
      } else if (sa->sa_family == AF_INET6) {
        struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)sa;
        inet_ntop(AF_INET6, &sa6->sin6_addr, addr_str, sizeof(addr_str));
        js_set(js, entry, "address", js_mkstr(js, addr_str, strlen(addr_str)));
        js_set(js, entry, "family", js_mkstr(js, "IPv6", 4));
        js_set(js, entry, "netmask", js_mkstr(js, "", 0));
        js_set(js, entry, "scopeid", Ant_Value_FromNumber((double)sa6->sin6_scope_id));
        js_set(js, entry, "cidr", js_mkstr(js, addr_str, strlen(addr_str)));
      } else {
        continue;
      }
      
      js_arr_push(js, iface_arr, entry);
    }
  }
  
  free(pAddresses);
  return result;
}

static ant_value_t os_userInfo(ant_params_t) {
  (void)args; (void)nargs;
  ant_value_t info = Ant_Object_CreateBare(js);
  
  js_set(js, info, "uid", Ant_Value_FromNumber(-1));
  js_set(js, info, "gid", Ant_Value_FromNumber(-1));
  
  char username[UNLEN + 1];
  DWORD username_len = sizeof(username);
  if (GetUserNameA(username, &username_len)) {
    js_set(js, info, "username", js_mkstr(js, username, strlen(username)));
  } else {
    js_set(js, info, "username", js_mkstr(js, "", 0));
  }
  
  const char *userprofile = getenv("USERPROFILE");
  if (userprofile) {
    js_set(js, info, "homedir", js_mkstr(js, userprofile, strlen(userprofile)));
  } else {
    js_set(js, info, "homedir", js_mkstr(js, "", 0));
  }
  
  js_set(js, info, "shell", Ant_Value_Null);
  
  return info;
}

static ant_value_t os_getPriority(ant_params_t) {
  (void)args; (void)nargs;
  return Ant_Value_FromNumber(0);
}

static ant_value_t os_setPriority(ant_params_t) {
  (void)args; (void)nargs;
  return Ant_Value_Undefined;
}

#else

static void format_mac_address(char *buf, size_t buflen, unsigned char *addr) {
  snprintf(buf, buflen, "%02x:%02x:%02x:%02x:%02x:%02x",
           addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

static int calc_prefix_len(struct sockaddr *netmask, int family) {
  int prefix = 0;
  if (family == AF_INET) {
    uint32_t mask = ntohl(((struct sockaddr_in *)netmask)->sin_addr.s_addr);
    while (mask & 0x80000000) { prefix++; mask <<= 1; }
  } else if (family == AF_INET6) {
    uint8_t *bytes = ((struct sockaddr_in6 *)netmask)->sin6_addr.s6_addr;
    for (int i = 0; i < 16; i++) {
      uint8_t b = bytes[i];
      while (b & 0x80) { prefix++; b <<= 1; }
      if (b) break;
    }
  }
  return prefix;
}

static void add_iface_entry(ant_t *js, ant_value_t iface_arr, struct ifaddrs *ifa, int family) {
  char addr_str[INET6_ADDRSTRLEN] = "";
  char netmask_str[INET6_ADDRSTRLEN] = "";
  char cidr[128];
  bool internal = (ifa->ifa_flags & IFF_LOOPBACK) != 0;
  
  ant_value_t entry = Ant_Object_Create(js);
  js_set(js, entry, "mac", js_mkstr(js, "00:00:00:00:00:00", 17));
  js_set(js, entry, "internal", Ant_Value_FromBool(internal));
  
  if (family == AF_INET) {
    struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
    inet_ntop(AF_INET, &sa->sin_addr, addr_str, sizeof(addr_str));
    if (ifa->ifa_netmask)
      inet_ntop(AF_INET, &((struct sockaddr_in *)ifa->ifa_netmask)->sin_addr, netmask_str, sizeof(netmask_str));
    
    int prefix = ifa->ifa_netmask ? calc_prefix_len(ifa->ifa_netmask, AF_INET) : 0;
    snprintf(cidr, sizeof(cidr), "%s/%d", addr_str, prefix);
    
    js_set(js, entry, "address", js_mkstr(js, addr_str, strlen(addr_str)));
    js_set(js, entry, "netmask", js_mkstr(js, netmask_str, strlen(netmask_str)));
    js_set(js, entry, "family", js_mkstr(js, "IPv4", 4));
    js_set(js, entry, "cidr", js_mkstr(js, cidr, strlen(cidr)));
    goto push;
  }
  
  if (family == AF_INET6) {
    struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)ifa->ifa_addr;
    inet_ntop(AF_INET6, &sa6->sin6_addr, addr_str, sizeof(addr_str));
    if (ifa->ifa_netmask)
      inet_ntop(AF_INET6, &((struct sockaddr_in6 *)ifa->ifa_netmask)->sin6_addr, netmask_str, sizeof(netmask_str));
    
    int prefix = ifa->ifa_netmask ? calc_prefix_len(ifa->ifa_netmask, AF_INET6) : 0;
    snprintf(cidr, sizeof(cidr), "%s/%d", addr_str, prefix);
    
    js_set(js, entry, "address", js_mkstr(js, addr_str, strlen(addr_str)));
    js_set(js, entry, "netmask", js_mkstr(js, netmask_str, strlen(netmask_str)));
    js_set(js, entry, "family", js_mkstr(js, "IPv6", 4));
    js_set(js, entry, "scopeid", Ant_Value_FromNumber((double)sa6->sin6_scope_id));
    js_set(js, entry, "cidr", js_mkstr(js, cidr, strlen(cidr)));
    goto push;
  }
  
  return;
push:
  js_arr_push(js, iface_arr, entry);
}

static unsigned char *get_mac_bytes(struct ifaddrs *ifa) {
#ifdef __APPLE__
  if (ifa->ifa_addr->sa_family != AF_LINK) return NULL;
  struct sockaddr_dl *sdl = (struct sockaddr_dl *)ifa->ifa_addr;
  return (sdl->sdl_alen == 6) ? (unsigned char *)LLADDR(sdl) : NULL;
#elif defined(__linux__)
  if (ifa->ifa_addr->sa_family != AF_PACKET) return NULL;
  struct sockaddr_ll *sll = (struct sockaddr_ll *)ifa->ifa_addr;
  return (sll->sll_halen == 6) ? sll->sll_addr : NULL;
#else
  (void)ifa;
  return NULL;
#endif
}

static void apply_mac_to_iface(ant_t *js, ant_value_t result, const char *name, unsigned char *mac_bytes) {
  char mac_str[18];
  format_mac_address(mac_str, sizeof(mac_str), mac_bytes);
  
  ant_value_t iface_arr = js_get(js, result, name);
  if (!is_special_object(iface_arr)) return;
  
  ant_value_t len_val = js_get(js, iface_arr, "length");
  int len = (vtype(len_val) == kTypeNumber) ? (int)Ant_Value_AsNumberUnchecked(len_val) : 0;
  
  for (int i = 0; i < len; i++) {
    char idx[16];
    snprintf(idx, sizeof(idx), "%d", i);
    ant_value_t entry = js_get(js, iface_arr, idx);
    if (is_special_object(entry)) {
      js_set(js, entry, "mac", js_mkstr(js, mac_str, strlen(mac_str)));
    }
  }
}

static ant_value_t os_networkInterfaces(ant_params_t) {
  (void)args; (void)nargs;
  ant_value_t result = Ant_Object_Create(js);
  
  struct ifaddrs *addrs, *ifa;
  if (getifaddrs(&addrs) != 0) return result;
  
  for (ifa = addrs; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr) continue;
    
    int family = ifa->ifa_addr->sa_family;
    if (family != AF_INET && family != AF_INET6) continue;
    
    ant_value_t iface_arr = js_get(js, result, ifa->ifa_name);
    if (!is_special_object(iface_arr)) {
      iface_arr = Ant_Array_Create(js);
      js_set(js, result, ifa->ifa_name, iface_arr);
    }
    
    add_iface_entry(js, iface_arr, ifa, family);
  }
  
  for (ifa = addrs; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr) continue;
    
    unsigned char *mac_bytes = get_mac_bytes(ifa);
    if (mac_bytes)
      apply_mac_to_iface(js, result, ifa->ifa_name, mac_bytes);
  }
  
  freeifaddrs(addrs);
  return result;
}

static ant_value_t os_userInfo(ant_params_t) {
  (void)args; (void)nargs;
  ant_value_t info = Ant_Object_CreateBare(js);
  
  uid_t uid = getuid();
  gid_t gid = getgid();
  struct passwd *pw = getpwuid(uid);
  
  js_set(js, info, "uid", Ant_Value_FromNumber((double)uid));
  js_set(js, info, "gid", Ant_Value_FromNumber((double)gid));
  
  if (pw) {
    if (pw->pw_name) js_set(js, info, "username", js_mkstr(js, pw->pw_name, strlen(pw->pw_name)));
    else js_set(js, info, "username", js_mkstr(js, "", 0));
    
    if (pw->pw_dir) js_set(js, info, "homedir", js_mkstr(js, pw->pw_dir, strlen(pw->pw_dir)));
    else js_set(js, info, "homedir", js_mkstr(js, "", 0));
    
    if (pw->pw_shell) js_set(js, info, "shell", js_mkstr(js, pw->pw_shell, strlen(pw->pw_shell)));
    else js_set(js, info, "shell", Ant_Value_Null);
  } else {
    js_set(js, info, "username", js_mkstr(js, "", 0));
    js_set(js, info, "homedir", js_mkstr(js, "", 0));
    js_set(js, info, "shell", Ant_Value_Null);
  }
  
  return info;
}

static ant_value_t os_getPriority(ant_params_t) {
  int pid = 0;
  if (nargs > 0 && vtype(args[0]) == kTypeNumber) {
    pid = (int)Ant_Value_AsNumberUnchecked(args[0]);
  }
  
  errno = 0;
  int priority = getpriority(PRIO_PROCESS, pid);
  if (errno != 0) {
    return js_mkerr(js, "Failed to get priority: %s", strerror(errno));
  }
  return Ant_Value_FromNumber((double)priority);
}

static ant_value_t os_setPriority(ant_params_t) {
  int pid = 0;
  int priority = 0;
  
  if (nargs == 1) {
    if (vtype(args[0]) != kTypeNumber) {
      return js_mkerr_typed(js, JS_ERR_TYPE, "priority must be a number");
    }
    priority = (int)Ant_Value_AsNumberUnchecked(args[0]);
  } else if (nargs >= 2) {
    if (vtype(args[0]) != kTypeNumber || vtype(args[1]) != kTypeNumber) {
      return js_mkerr_typed(js, JS_ERR_TYPE, "pid and priority must be numbers");
    }
    pid = (int)Ant_Value_AsNumberUnchecked(args[0]);
    priority = (int)Ant_Value_AsNumberUnchecked(args[1]);
  } else {
    return js_mkerr_typed(js, JS_ERR_TYPE, "setPriority requires at least 1 argument");
  }
  
  if (setpriority(PRIO_PROCESS, pid, priority) != 0) {
    return js_mkerr(js, "Failed to set priority: %s", strerror(errno));
  }
  return Ant_Value_Undefined;
}

#endif

static void add_signal_constants(ant_t *js, ant_value_t signals) {
#ifdef SIGHUP
  js_set(js, signals, "SIGHUP", Ant_Value_FromNumber(SIGHUP));
#endif
#ifdef SIGINT
  js_set(js, signals, "SIGINT", Ant_Value_FromNumber(SIGINT));
#endif
#ifdef SIGQUIT
  js_set(js, signals, "SIGQUIT", Ant_Value_FromNumber(SIGQUIT));
#endif
#ifdef SIGILL
  js_set(js, signals, "SIGILL", Ant_Value_FromNumber(SIGILL));
#endif
#ifdef SIGTRAP
  js_set(js, signals, "SIGTRAP", Ant_Value_FromNumber(SIGTRAP));
#endif
#ifdef SIGABRT
  js_set(js, signals, "SIGABRT", Ant_Value_FromNumber(SIGABRT));
#endif
#ifdef SIGIOT
  js_set(js, signals, "SIGIOT", Ant_Value_FromNumber(SIGIOT));
#endif
#ifdef SIGBUS
  js_set(js, signals, "SIGBUS", Ant_Value_FromNumber(SIGBUS));
#endif
#ifdef SIGFPE
  js_set(js, signals, "SIGFPE", Ant_Value_FromNumber(SIGFPE));
#endif
#ifdef SIGKILL
  js_set(js, signals, "SIGKILL", Ant_Value_FromNumber(SIGKILL));
#endif
#ifdef SIGUSR1
  js_set(js, signals, "SIGUSR1", Ant_Value_FromNumber(SIGUSR1));
#endif
#ifdef SIGUSR2
  js_set(js, signals, "SIGUSR2", Ant_Value_FromNumber(SIGUSR2));
#endif
#ifdef SIGSEGV
  js_set(js, signals, "SIGSEGV", Ant_Value_FromNumber(SIGSEGV));
#endif
#ifdef SIGPIPE
  js_set(js, signals, "SIGPIPE", Ant_Value_FromNumber(SIGPIPE));
#endif
#ifdef SIGALRM
  js_set(js, signals, "SIGALRM", Ant_Value_FromNumber(SIGALRM));
#endif
#ifdef SIGTERM
  js_set(js, signals, "SIGTERM", Ant_Value_FromNumber(SIGTERM));
#endif
#ifdef SIGCHLD
  js_set(js, signals, "SIGCHLD", Ant_Value_FromNumber(SIGCHLD));
#endif
#ifdef SIGCONT
  js_set(js, signals, "SIGCONT", Ant_Value_FromNumber(SIGCONT));
#endif
#ifdef SIGSTOP
  js_set(js, signals, "SIGSTOP", Ant_Value_FromNumber(SIGSTOP));
#endif
#ifdef SIGTSTP
  js_set(js, signals, "SIGTSTP", Ant_Value_FromNumber(SIGTSTP));
#endif
#ifdef SIGTTIN
  js_set(js, signals, "SIGTTIN", Ant_Value_FromNumber(SIGTTIN));
#endif
#ifdef SIGTTOU
  js_set(js, signals, "SIGTTOU", Ant_Value_FromNumber(SIGTTOU));
#endif
#ifdef SIGURG
  js_set(js, signals, "SIGURG", Ant_Value_FromNumber(SIGURG));
#endif
#ifdef SIGXCPU
  js_set(js, signals, "SIGXCPU", Ant_Value_FromNumber(SIGXCPU));
#endif
#ifdef SIGXFSZ
  js_set(js, signals, "SIGXFSZ", Ant_Value_FromNumber(SIGXFSZ));
#endif
#ifdef SIGVTALRM
  js_set(js, signals, "SIGVTALRM", Ant_Value_FromNumber(SIGVTALRM));
#endif
#ifdef SIGPROF
  js_set(js, signals, "SIGPROF", Ant_Value_FromNumber(SIGPROF));
#endif
#ifdef SIGWINCH
  js_set(js, signals, "SIGWINCH", Ant_Value_FromNumber(SIGWINCH));
#endif
#ifdef SIGIO
  js_set(js, signals, "SIGIO", Ant_Value_FromNumber(SIGIO));
#endif
#ifdef SIGSYS
  js_set(js, signals, "SIGSYS", Ant_Value_FromNumber(SIGSYS));
#endif
}

static void add_errno_constants(ant_t *js, ant_value_t errn) {
  js_set(js, errn, "E2BIG", Ant_Value_FromNumber(E2BIG));
  js_set(js, errn, "EACCES", Ant_Value_FromNumber(EACCES));
  js_set(js, errn, "EADDRINUSE", Ant_Value_FromNumber(EADDRINUSE));
  js_set(js, errn, "EADDRNOTAVAIL", Ant_Value_FromNumber(EADDRNOTAVAIL));
  js_set(js, errn, "EAFNOSUPPORT", Ant_Value_FromNumber(EAFNOSUPPORT));
  js_set(js, errn, "EAGAIN", Ant_Value_FromNumber(EAGAIN));
  js_set(js, errn, "EALREADY", Ant_Value_FromNumber(EALREADY));
  js_set(js, errn, "EBADF", Ant_Value_FromNumber(EBADF));
  js_set(js, errn, "EBUSY", Ant_Value_FromNumber(EBUSY));
  js_set(js, errn, "ECANCELED", Ant_Value_FromNumber(ECANCELED));
  js_set(js, errn, "ECHILD", Ant_Value_FromNumber(ECHILD));
  js_set(js, errn, "ECONNABORTED", Ant_Value_FromNumber(ECONNABORTED));
  js_set(js, errn, "ECONNREFUSED", Ant_Value_FromNumber(ECONNREFUSED));
  js_set(js, errn, "ECONNRESET", Ant_Value_FromNumber(ECONNRESET));
  js_set(js, errn, "EDEADLK", Ant_Value_FromNumber(EDEADLK));
  js_set(js, errn, "EDESTADDRREQ", Ant_Value_FromNumber(EDESTADDRREQ));
  js_set(js, errn, "EDOM", Ant_Value_FromNumber(EDOM));
  js_set(js, errn, "EEXIST", Ant_Value_FromNumber(EEXIST));
  js_set(js, errn, "EFAULT", Ant_Value_FromNumber(EFAULT));
  js_set(js, errn, "EFBIG", Ant_Value_FromNumber(EFBIG));
  js_set(js, errn, "EHOSTUNREACH", Ant_Value_FromNumber(EHOSTUNREACH));
  js_set(js, errn, "EINPROGRESS", Ant_Value_FromNumber(EINPROGRESS));
  js_set(js, errn, "EINTR", Ant_Value_FromNumber(EINTR));
  js_set(js, errn, "EINVAL", Ant_Value_FromNumber(EINVAL));
  js_set(js, errn, "EIO", Ant_Value_FromNumber(EIO));
  js_set(js, errn, "EISCONN", Ant_Value_FromNumber(EISCONN));
  js_set(js, errn, "EISDIR", Ant_Value_FromNumber(EISDIR));
  js_set(js, errn, "ELOOP", Ant_Value_FromNumber(ELOOP));
  js_set(js, errn, "EMFILE", Ant_Value_FromNumber(EMFILE));
  js_set(js, errn, "EMLINK", Ant_Value_FromNumber(EMLINK));
  js_set(js, errn, "EMSGSIZE", Ant_Value_FromNumber(EMSGSIZE));
  js_set(js, errn, "ENAMETOOLONG", Ant_Value_FromNumber(ENAMETOOLONG));
  js_set(js, errn, "ENETDOWN", Ant_Value_FromNumber(ENETDOWN));
  js_set(js, errn, "ENETRESET", Ant_Value_FromNumber(ENETRESET));
  js_set(js, errn, "ENETUNREACH", Ant_Value_FromNumber(ENETUNREACH));
  js_set(js, errn, "ENFILE", Ant_Value_FromNumber(ENFILE));
  js_set(js, errn, "ENOBUFS", Ant_Value_FromNumber(ENOBUFS));
  js_set(js, errn, "ENODEV", Ant_Value_FromNumber(ENODEV));
  js_set(js, errn, "ENOENT", Ant_Value_FromNumber(ENOENT));
  js_set(js, errn, "ENOEXEC", Ant_Value_FromNumber(ENOEXEC));
  js_set(js, errn, "ENOLCK", Ant_Value_FromNumber(ENOLCK));
  js_set(js, errn, "ENOMEM", Ant_Value_FromNumber(ENOMEM));
  js_set(js, errn, "ENOPROTOOPT", Ant_Value_FromNumber(ENOPROTOOPT));
  js_set(js, errn, "ENOSPC", Ant_Value_FromNumber(ENOSPC));
  js_set(js, errn, "ENOSYS", Ant_Value_FromNumber(ENOSYS));
  js_set(js, errn, "ENOTCONN", Ant_Value_FromNumber(ENOTCONN));
  js_set(js, errn, "ENOTDIR", Ant_Value_FromNumber(ENOTDIR));
  js_set(js, errn, "ENOTEMPTY", Ant_Value_FromNumber(ENOTEMPTY));
  js_set(js, errn, "ENOTSOCK", Ant_Value_FromNumber(ENOTSOCK));
  js_set(js, errn, "ENOTSUP", Ant_Value_FromNumber(ENOTSUP));
  js_set(js, errn, "ENOTTY", Ant_Value_FromNumber(ENOTTY));
  js_set(js, errn, "ENXIO", Ant_Value_FromNumber(ENXIO));
  js_set(js, errn, "EOPNOTSUPP", Ant_Value_FromNumber(EOPNOTSUPP));
  js_set(js, errn, "EOVERFLOW", Ant_Value_FromNumber(EOVERFLOW));
  js_set(js, errn, "EPERM", Ant_Value_FromNumber(EPERM));
  js_set(js, errn, "EPIPE", Ant_Value_FromNumber(EPIPE));
  js_set(js, errn, "EPROTONOSUPPORT", Ant_Value_FromNumber(EPROTONOSUPPORT));
  js_set(js, errn, "EPROTOTYPE", Ant_Value_FromNumber(EPROTOTYPE));
  js_set(js, errn, "ERANGE", Ant_Value_FromNumber(ERANGE));
  js_set(js, errn, "EROFS", Ant_Value_FromNumber(EROFS));
  js_set(js, errn, "ESPIPE", Ant_Value_FromNumber(ESPIPE));
  js_set(js, errn, "ESRCH", Ant_Value_FromNumber(ESRCH));
  js_set(js, errn, "ETIMEDOUT", Ant_Value_FromNumber(ETIMEDOUT));
  js_set(js, errn, "ETXTBSY", Ant_Value_FromNumber(ETXTBSY));
  js_set(js, errn, "EWOULDBLOCK", Ant_Value_FromNumber(EWOULDBLOCK));
  js_set(js, errn, "EXDEV", Ant_Value_FromNumber(EXDEV));
}

static void add_priority_constants(ant_t *js, ant_value_t priority) {
  js_set(js, priority, "PRIORITY_LOW", Ant_Value_FromNumber(19));
  js_set(js, priority, "PRIORITY_BELOW_NORMAL", Ant_Value_FromNumber(10));
  js_set(js, priority, "PRIORITY_NORMAL", Ant_Value_FromNumber(0));
  js_set(js, priority, "PRIORITY_ABOVE_NORMAL", Ant_Value_FromNumber(-7));
  js_set(js, priority, "PRIORITY_HIGH", Ant_Value_FromNumber(-14));
  js_set(js, priority, "PRIORITY_HIGHEST", Ant_Value_FromNumber(-20));
}

static void add_dlopen_constants(ant_t *js, ant_value_t dlopen_obj) {
#ifdef RTLD_LAZY
  js_set(js, dlopen_obj, "RTLD_LAZY", Ant_Value_FromNumber(RTLD_LAZY));
#endif
#ifdef RTLD_NOW
  js_set(js, dlopen_obj, "RTLD_NOW", Ant_Value_FromNumber(RTLD_NOW));
#endif
#ifdef RTLD_GLOBAL
  js_set(js, dlopen_obj, "RTLD_GLOBAL", Ant_Value_FromNumber(RTLD_GLOBAL));
#endif
#ifdef RTLD_LOCAL
  js_set(js, dlopen_obj, "RTLD_LOCAL", Ant_Value_FromNumber(RTLD_LOCAL));
#endif
#ifdef RTLD_DEEPBIND
  js_set(js, dlopen_obj, "RTLD_DEEPBIND", Ant_Value_FromNumber(RTLD_DEEPBIND));
#endif
}

ant_value_t os_library(ant_t *js) {
  ant_value_t lib = Ant_Object_CreateBare(js);
  
  js_set(js, lib, "EOL", js_mkstr(js, OS_EOL, strlen(OS_EOL)));
  js_set(js, lib, "devNull", js_mkstr(js, OS_DEVNULL, strlen(OS_DEVNULL)));
  
  js_set(js, lib, "arch", Ant_Function_Create(os_arch));
  js_set(js, lib, "platform", Ant_Function_Create(os_platform));
  js_set(js, lib, "type", Ant_Function_Create(os_type));
  js_set(js, lib, "release", Ant_Function_Create(os_release));
  js_set(js, lib, "version", Ant_Function_Create(os_version));
  js_set(js, lib, "machine", Ant_Function_Create(os_machine));
  js_set(js, lib, "hostname", Ant_Function_Create(os_hostname));
  js_set(js, lib, "homedir", Ant_Function_Create(os_homedir));
  js_set(js, lib, "tmpdir", Ant_Function_Create(os_tmpdir));
  js_set(js, lib, "endianness", Ant_Function_Create(os_endianness));
  js_set(js, lib, "uptime", Ant_Function_Create(os_uptime));
  js_set(js, lib, "totalmem", Ant_Function_Create(os_totalmem));
  js_set(js, lib, "freemem", Ant_Function_Create(os_freemem));
  js_set(js, lib, "availableParallelism", Ant_Function_Create(os_availableParallelism));
  js_set(js, lib, "cpus", Ant_Function_Create(os_cpus));
  js_set(js, lib, "loadavg", Ant_Function_Create(os_loadavg));
  js_set(js, lib, "networkInterfaces", Ant_Function_Create(os_networkInterfaces));
  js_set(js, lib, "userInfo", Ant_Function_CreateWithArity(os_userInfo, 1));
  js_set(js, lib, "getPriority", Ant_Function_CreateWithArity(os_getPriority, 1));
  js_set(js, lib, "setPriority", Ant_Function_CreateWithArity(os_setPriority, 2));
  
  ant_value_t constants = Ant_Object_CreateBare(js);
  ant_value_t signals = Ant_Object_CreateBare(js);
  ant_value_t errn = Ant_Object_CreateBare(js);
  ant_value_t priority = Ant_Object_CreateBare(js);
  ant_value_t dlopen_obj = Ant_Object_CreateBare(js);
  
  add_signal_constants(js, signals);
  add_errno_constants(js, errn);
  add_priority_constants(js, priority);
  add_dlopen_constants(js, dlopen_obj);
  
  js_set(js, constants, "signals", signals);
  js_set(js, constants, "errno", errn);
  js_set(js, constants, "priority", priority);
  js_set(js, constants, "dlopen", dlopen_obj);
  js_set(js, constants, "UV_UDP_REUSEADDR", Ant_Value_FromNumber(4));
  js_set(js, lib, "constants", constants);
  js_set_tostringtag(js, lib, js_mkstr(js, "os", 2));

  return lib;
}
