#include <compat.h> // IWYU pragma: keep

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <lmcons.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#else
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
#include "modules/symbol.h"

#ifdef _WIN32
#define OS_EOL "\r\n"
#define OS_DEVNULL "\\\\.\\nul"
#else
#define OS_EOL "\n"
#define OS_DEVNULL "/dev/null"
#endif

static jsval_t os_arch(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t os_platform(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t os_type(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t os_release(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t os_version(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t os_machine(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t os_hostname(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  char hostname[256];
  if (gethostname(hostname, sizeof(hostname)) == 0) {
    return js_mkstr(js, hostname, strlen(hostname));
  }
  return js_mkstr(js, "", 0);
}

static jsval_t os_homedir(struct js *js, jsval_t *args, int nargs) {
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

static jsval_t os_tmpdir(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  const char *tmpdir = getenv("TMPDIR");
  if (!tmpdir) tmpdir = getenv("TMP");
  if (!tmpdir) tmpdir = getenv("TEMP");
  if (!tmpdir) tmpdir = "/tmp";
  return js_mkstr(js, tmpdir, strlen(tmpdir));
}

static jsval_t os_endianness(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  uint16_t test = 1;
  if (*(uint8_t *)&test == 1) {
    return js_mkstr(js, "LE", 2);
  }
  return js_mkstr(js, "BE", 2);
}

static jsval_t os_uptime(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
#ifdef _WIN32
  return js_mknum((double)GetTickCount64() / 1000.0);
#elif defined(__APPLE__)
  struct timeval boottime;
  size_t len = sizeof(boottime);
  int mib[2] = { CTL_KERN, KERN_BOOTTIME };
  if (sysctl(mib, 2, &boottime, &len, NULL, 0) == 0) {
    time_t now = time(NULL);
    return js_mknum((double)(now - boottime.tv_sec));
  }
  return js_mknum(0);
#elif defined(__linux__)
  struct sysinfo info;
  if (sysinfo(&info) == 0) {
    return js_mknum((double)info.uptime);
  }
  return js_mknum(0);
#else
  return js_mknum(0);
#endif
}

static jsval_t os_totalmem(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
#ifdef _WIN32
  MEMORYSTATUSEX memInfo;
  memInfo.dwLength = sizeof(MEMORYSTATUSEX);
  if (GlobalMemoryStatusEx(&memInfo)) {
    return js_mknum((double)memInfo.ullTotalPhys);
  }
  return js_mknum(0);
#elif defined(__APPLE__)
  int64_t memsize;
  size_t len = sizeof(memsize);
  if (sysctlbyname("hw.memsize", &memsize, &len, NULL, 0) == 0) {
    return js_mknum((double)memsize);
  }
  return js_mknum(0);
#elif defined(__linux__)
  struct sysinfo info;
  if (sysinfo(&info) == 0) {
    return js_mknum((double)info.totalram * info.mem_unit);
  }
  return js_mknum(0);
#else
  return js_mknum(0);
#endif
}

static jsval_t os_freemem(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
#ifdef _WIN32
  MEMORYSTATUSEX memInfo;
  memInfo.dwLength = sizeof(MEMORYSTATUSEX);
  if (GlobalMemoryStatusEx(&memInfo)) {
    return js_mknum((double)memInfo.ullAvailPhys);
  }
  return js_mknum(0);
#elif defined(__APPLE__)
  vm_size_t page_size;
  mach_port_t mach_port = mach_host_self();
  vm_statistics64_data_t vm_stats;
  mach_msg_type_number_t count = sizeof(vm_stats) / sizeof(natural_t);
  
  if (host_page_size(mach_port, &page_size) == KERN_SUCCESS &&
      host_statistics64(mach_port, HOST_VM_INFO64, (host_info64_t)&vm_stats, &count) == KERN_SUCCESS) {
    return js_mknum((double)vm_stats.free_count * page_size);
  }
  return js_mknum(0);
#elif defined(__linux__)
  struct sysinfo info;
  if (sysinfo(&info) == 0) {
    return js_mknum((double)info.freeram * info.mem_unit);
  }
  return js_mknum(0);
#else
  return js_mknum(0);
#endif
}

static jsval_t os_availableParallelism(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
#ifdef _WIN32
  SYSTEM_INFO sysinfo;
  GetSystemInfo(&sysinfo);
  return js_mknum((double)sysinfo.dwNumberOfProcessors);
#elif defined(__APPLE__)
  int count;
  size_t size = sizeof(count);
  if (sysctlbyname("hw.ncpu", &count, &size, NULL, 0) == 0) {
    return js_mknum((double)count);
  }
  return js_mknum(1);
#elif defined(__linux__)
  long count = sysconf(_SC_NPROCESSORS_ONLN);
  return js_mknum(count > 0 ? (double)count : 1);
#else
  return js_mknum(1);
#endif
}

typedef struct {
  double user, nice, sys, idle, irq;
} cpu_times_t;

static void push_cpu_entry(struct js *js, jsval_t arr, const char *model, double speed, cpu_times_t *times) {
  jsval_t cpu = js_mkobj(js);
  js_set(js, cpu, "model", js_mkstr(js, model, strlen(model)));
  js_set(js, cpu, "speed", js_mknum(speed));
  
  jsval_t t = js_mkobj(js);
  js_set(js, t, "user", js_mknum(times->user));
  js_set(js, t, "nice", js_mknum(times->nice));
  js_set(js, t, "sys", js_mknum(times->sys));
  js_set(js, t, "idle", js_mknum(times->idle));
  js_set(js, t, "irq", js_mknum(times->irq));
  js_set(js, cpu, "times", t);
  
  js_arr_push(js, arr, cpu);
}

#ifdef __APPLE__
static jsval_t os_cpus_darwin(struct js *js) {
  jsval_t arr = js_mkarr(js);
  
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
  double speed = freq / 1000000.0;
  
  processor_cpu_load_info_data_t *load = (processor_cpu_load_info_data_t *)cpu_info;
  
  for (natural_t i = 0; i < ncpu; i++) {
    cpu_times_t times = {
      .user = (double)load[i].cpu_ticks[CPU_STATE_USER] * 10,
      .nice = (double)load[i].cpu_ticks[CPU_STATE_NICE] * 10,
      .sys  = (double)load[i].cpu_ticks[CPU_STATE_SYSTEM] * 10,
      .idle = (double)load[i].cpu_ticks[CPU_STATE_IDLE] * 10,
      .irq  = 0
    };
    push_cpu_entry(js, arr, model, speed, &times);
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

static jsval_t os_cpus_linux(struct js *js) {
  jsval_t arr = js_mkarr(js);
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
    push_cpu_entry(js, arr, model, speed, &times);
    cpu_idx++;
  }
  fclose(fp);
  return arr;
}
#endif

static jsval_t os_cpus(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
#ifdef __APPLE__
  return os_cpus_darwin(js);
#elif defined(__linux__)
  return os_cpus_linux(js);
#else
  return js_mkarr(js);
#endif
}

static jsval_t os_loadavg(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t arr = js_mkarr(js);
  
#if defined(__APPLE__) || defined(__linux__)
  double loadavg[3];
  if (getloadavg(loadavg, 3) == 3) {
    js_arr_push(js, arr, js_mknum(loadavg[0]));
    js_arr_push(js, arr, js_mknum(loadavg[1]));
    js_arr_push(js, arr, js_mknum(loadavg[2]));
  } else {
    js_arr_push(js, arr, js_mknum(0));
    js_arr_push(js, arr, js_mknum(0));
    js_arr_push(js, arr, js_mknum(0));
  }
#else
  js_arr_push(js, arr, js_mknum(0));
  js_arr_push(js, arr, js_mknum(0));
  js_arr_push(js, arr, js_mknum(0));
#endif
  
  return arr;
}

#ifdef _WIN32

static jsval_t os_networkInterfaces(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t result = js_mkobj(js);
  
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
    jsval_t iface_arr = js_get(js, result, pCurrAddresses->AdapterName);
    if (js_type(iface_arr) != JS_OBJ) {
      iface_arr = js_mkarr(js);
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
      jsval_t entry = js_mkobj(js);
      js_set(js, entry, "mac", js_mkstr(js, mac_str, strlen(mac_str)));
      js_set(js, entry, "internal", internal ? js_mktrue() : js_mkfalse());
      
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
        js_set(js, entry, "scopeid", js_mknum((double)sa6->sin6_scope_id));
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

static jsval_t os_userInfo(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t info = js_mkobj(js);
  
  js_set(js, info, "uid", js_mknum(-1));
  js_set(js, info, "gid", js_mknum(-1));
  
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
  
  js_set(js, info, "shell", js_mknull());
  
  return info;
}

static jsval_t os_getPriority(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  return js_mknum(0);
}

static jsval_t os_setPriority(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  return js_mkundef();
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

static void add_iface_entry(struct js *js, jsval_t iface_arr, struct ifaddrs *ifa, int family) {
  char addr_str[INET6_ADDRSTRLEN] = "";
  char netmask_str[INET6_ADDRSTRLEN] = "";
  char cidr[128];
  bool internal = (ifa->ifa_flags & IFF_LOOPBACK) != 0;
  
  jsval_t entry = js_mkobj(js);
  js_set(js, entry, "mac", js_mkstr(js, "00:00:00:00:00:00", 17));
  js_set(js, entry, "internal", internal ? js_mktrue() : js_mkfalse());
  
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
    js_set(js, entry, "scopeid", js_mknum((double)sa6->sin6_scope_id));
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

static void apply_mac_to_iface(struct js *js, jsval_t result, const char *name, unsigned char *mac_bytes) {
  char mac_str[18];
  format_mac_address(mac_str, sizeof(mac_str), mac_bytes);
  
  jsval_t iface_arr = js_get(js, result, name);
  if (js_type(iface_arr) != JS_OBJ) return;
  
  jsval_t len_val = js_get(js, iface_arr, "length");
  int len = (js_type(len_val) == JS_NUM) ? (int)js_getnum(len_val) : 0;
  
  for (int i = 0; i < len; i++) {
    char idx[16];
    snprintf(idx, sizeof(idx), "%d", i);
    jsval_t entry = js_get(js, iface_arr, idx);
    if (js_type(entry) == JS_OBJ)
      js_set(js, entry, "mac", js_mkstr(js, mac_str, strlen(mac_str)));
  }
}

static jsval_t os_networkInterfaces(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t result = js_mkobj(js);
  
  struct ifaddrs *addrs, *ifa;
  if (getifaddrs(&addrs) != 0) return result;
  
  for (ifa = addrs; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr) continue;
    
    int family = ifa->ifa_addr->sa_family;
    if (family != AF_INET && family != AF_INET6) continue;
    
    jsval_t iface_arr = js_get(js, result, ifa->ifa_name);
    if (js_type(iface_arr) != JS_OBJ) {
      iface_arr = js_mkarr(js);
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

static jsval_t os_userInfo(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t info = js_mkobj(js);
  
  uid_t uid = getuid();
  gid_t gid = getgid();
  struct passwd *pw = getpwuid(uid);
  
  js_set(js, info, "uid", js_mknum((double)uid));
  js_set(js, info, "gid", js_mknum((double)gid));
  
  if (pw) {
    if (pw->pw_name) js_set(js, info, "username", js_mkstr(js, pw->pw_name, strlen(pw->pw_name)));
    else js_set(js, info, "username", js_mkstr(js, "", 0));
    
    if (pw->pw_dir) js_set(js, info, "homedir", js_mkstr(js, pw->pw_dir, strlen(pw->pw_dir)));
    else js_set(js, info, "homedir", js_mkstr(js, "", 0));
    
    if (pw->pw_shell) js_set(js, info, "shell", js_mkstr(js, pw->pw_shell, strlen(pw->pw_shell)));
    else js_set(js, info, "shell", js_mknull());
  } else {
    js_set(js, info, "username", js_mkstr(js, "", 0));
    js_set(js, info, "homedir", js_mkstr(js, "", 0));
    js_set(js, info, "shell", js_mknull());
  }
  
  return info;
}

static jsval_t os_getPriority(struct js *js, jsval_t *args, int nargs) {
  int pid = 0;
  if (nargs > 0 && js_type(args[0]) == JS_NUM) {
    pid = (int)js_getnum(args[0]);
  }
  
  errno = 0;
  int priority = getpriority(PRIO_PROCESS, pid);
  if (errno != 0) {
    return js_mkerr(js, "Failed to get priority: %s", strerror(errno));
  }
  return js_mknum((double)priority);
}

static jsval_t os_setPriority(struct js *js, jsval_t *args, int nargs) {
  int pid = 0;
  int priority = 0;
  
  if (nargs == 1) {
    if (js_type(args[0]) != JS_NUM) {
      return js_mkerr_typed(js, JS_ERR_TYPE, "priority must be a number");
    }
    priority = (int)js_getnum(args[0]);
  } else if (nargs >= 2) {
    if (js_type(args[0]) != JS_NUM || js_type(args[1]) != JS_NUM) {
      return js_mkerr_typed(js, JS_ERR_TYPE, "pid and priority must be numbers");
    }
    pid = (int)js_getnum(args[0]);
    priority = (int)js_getnum(args[1]);
  } else {
    return js_mkerr_typed(js, JS_ERR_TYPE, "setPriority requires at least 1 argument");
  }
  
  if (setpriority(PRIO_PROCESS, pid, priority) != 0) {
    return js_mkerr(js, "Failed to set priority: %s", strerror(errno));
  }
  return js_mkundef();
}

#endif

static void add_signal_constants(struct js *js, jsval_t signals) {
#ifdef SIGHUP
  js_set(js, signals, "SIGHUP", js_mknum(SIGHUP));
#endif
#ifdef SIGINT
  js_set(js, signals, "SIGINT", js_mknum(SIGINT));
#endif
#ifdef SIGQUIT
  js_set(js, signals, "SIGQUIT", js_mknum(SIGQUIT));
#endif
#ifdef SIGILL
  js_set(js, signals, "SIGILL", js_mknum(SIGILL));
#endif
#ifdef SIGTRAP
  js_set(js, signals, "SIGTRAP", js_mknum(SIGTRAP));
#endif
#ifdef SIGABRT
  js_set(js, signals, "SIGABRT", js_mknum(SIGABRT));
#endif
#ifdef SIGIOT
  js_set(js, signals, "SIGIOT", js_mknum(SIGIOT));
#endif
#ifdef SIGBUS
  js_set(js, signals, "SIGBUS", js_mknum(SIGBUS));
#endif
#ifdef SIGFPE
  js_set(js, signals, "SIGFPE", js_mknum(SIGFPE));
#endif
#ifdef SIGKILL
  js_set(js, signals, "SIGKILL", js_mknum(SIGKILL));
#endif
#ifdef SIGUSR1
  js_set(js, signals, "SIGUSR1", js_mknum(SIGUSR1));
#endif
#ifdef SIGUSR2
  js_set(js, signals, "SIGUSR2", js_mknum(SIGUSR2));
#endif
#ifdef SIGSEGV
  js_set(js, signals, "SIGSEGV", js_mknum(SIGSEGV));
#endif
#ifdef SIGPIPE
  js_set(js, signals, "SIGPIPE", js_mknum(SIGPIPE));
#endif
#ifdef SIGALRM
  js_set(js, signals, "SIGALRM", js_mknum(SIGALRM));
#endif
#ifdef SIGTERM
  js_set(js, signals, "SIGTERM", js_mknum(SIGTERM));
#endif
#ifdef SIGCHLD
  js_set(js, signals, "SIGCHLD", js_mknum(SIGCHLD));
#endif
#ifdef SIGCONT
  js_set(js, signals, "SIGCONT", js_mknum(SIGCONT));
#endif
#ifdef SIGSTOP
  js_set(js, signals, "SIGSTOP", js_mknum(SIGSTOP));
#endif
#ifdef SIGTSTP
  js_set(js, signals, "SIGTSTP", js_mknum(SIGTSTP));
#endif
#ifdef SIGTTIN
  js_set(js, signals, "SIGTTIN", js_mknum(SIGTTIN));
#endif
#ifdef SIGTTOU
  js_set(js, signals, "SIGTTOU", js_mknum(SIGTTOU));
#endif
#ifdef SIGURG
  js_set(js, signals, "SIGURG", js_mknum(SIGURG));
#endif
#ifdef SIGXCPU
  js_set(js, signals, "SIGXCPU", js_mknum(SIGXCPU));
#endif
#ifdef SIGXFSZ
  js_set(js, signals, "SIGXFSZ", js_mknum(SIGXFSZ));
#endif
#ifdef SIGVTALRM
  js_set(js, signals, "SIGVTALRM", js_mknum(SIGVTALRM));
#endif
#ifdef SIGPROF
  js_set(js, signals, "SIGPROF", js_mknum(SIGPROF));
#endif
#ifdef SIGWINCH
  js_set(js, signals, "SIGWINCH", js_mknum(SIGWINCH));
#endif
#ifdef SIGIO
  js_set(js, signals, "SIGIO", js_mknum(SIGIO));
#endif
#ifdef SIGSYS
  js_set(js, signals, "SIGSYS", js_mknum(SIGSYS));
#endif
}

static void add_errno_constants(struct js *js, jsval_t errn) {
  js_set(js, errn, "E2BIG", js_mknum(E2BIG));
  js_set(js, errn, "EACCES", js_mknum(EACCES));
  js_set(js, errn, "EADDRINUSE", js_mknum(EADDRINUSE));
  js_set(js, errn, "EADDRNOTAVAIL", js_mknum(EADDRNOTAVAIL));
  js_set(js, errn, "EAFNOSUPPORT", js_mknum(EAFNOSUPPORT));
  js_set(js, errn, "EAGAIN", js_mknum(EAGAIN));
  js_set(js, errn, "EALREADY", js_mknum(EALREADY));
  js_set(js, errn, "EBADF", js_mknum(EBADF));
  js_set(js, errn, "EBUSY", js_mknum(EBUSY));
  js_set(js, errn, "ECANCELED", js_mknum(ECANCELED));
  js_set(js, errn, "ECHILD", js_mknum(ECHILD));
  js_set(js, errn, "ECONNABORTED", js_mknum(ECONNABORTED));
  js_set(js, errn, "ECONNREFUSED", js_mknum(ECONNREFUSED));
  js_set(js, errn, "ECONNRESET", js_mknum(ECONNRESET));
  js_set(js, errn, "EDEADLK", js_mknum(EDEADLK));
  js_set(js, errn, "EDESTADDRREQ", js_mknum(EDESTADDRREQ));
  js_set(js, errn, "EDOM", js_mknum(EDOM));
  js_set(js, errn, "EEXIST", js_mknum(EEXIST));
  js_set(js, errn, "EFAULT", js_mknum(EFAULT));
  js_set(js, errn, "EFBIG", js_mknum(EFBIG));
  js_set(js, errn, "EHOSTUNREACH", js_mknum(EHOSTUNREACH));
  js_set(js, errn, "EINPROGRESS", js_mknum(EINPROGRESS));
  js_set(js, errn, "EINTR", js_mknum(EINTR));
  js_set(js, errn, "EINVAL", js_mknum(EINVAL));
  js_set(js, errn, "EIO", js_mknum(EIO));
  js_set(js, errn, "EISCONN", js_mknum(EISCONN));
  js_set(js, errn, "EISDIR", js_mknum(EISDIR));
  js_set(js, errn, "ELOOP", js_mknum(ELOOP));
  js_set(js, errn, "EMFILE", js_mknum(EMFILE));
  js_set(js, errn, "EMLINK", js_mknum(EMLINK));
  js_set(js, errn, "EMSGSIZE", js_mknum(EMSGSIZE));
  js_set(js, errn, "ENAMETOOLONG", js_mknum(ENAMETOOLONG));
  js_set(js, errn, "ENETDOWN", js_mknum(ENETDOWN));
  js_set(js, errn, "ENETRESET", js_mknum(ENETRESET));
  js_set(js, errn, "ENETUNREACH", js_mknum(ENETUNREACH));
  js_set(js, errn, "ENFILE", js_mknum(ENFILE));
  js_set(js, errn, "ENOBUFS", js_mknum(ENOBUFS));
  js_set(js, errn, "ENODEV", js_mknum(ENODEV));
  js_set(js, errn, "ENOENT", js_mknum(ENOENT));
  js_set(js, errn, "ENOEXEC", js_mknum(ENOEXEC));
  js_set(js, errn, "ENOLCK", js_mknum(ENOLCK));
  js_set(js, errn, "ENOMEM", js_mknum(ENOMEM));
  js_set(js, errn, "ENOPROTOOPT", js_mknum(ENOPROTOOPT));
  js_set(js, errn, "ENOSPC", js_mknum(ENOSPC));
  js_set(js, errn, "ENOSYS", js_mknum(ENOSYS));
  js_set(js, errn, "ENOTCONN", js_mknum(ENOTCONN));
  js_set(js, errn, "ENOTDIR", js_mknum(ENOTDIR));
  js_set(js, errn, "ENOTEMPTY", js_mknum(ENOTEMPTY));
  js_set(js, errn, "ENOTSOCK", js_mknum(ENOTSOCK));
  js_set(js, errn, "ENOTSUP", js_mknum(ENOTSUP));
  js_set(js, errn, "ENOTTY", js_mknum(ENOTTY));
  js_set(js, errn, "ENXIO", js_mknum(ENXIO));
  js_set(js, errn, "EOPNOTSUPP", js_mknum(EOPNOTSUPP));
  js_set(js, errn, "EOVERFLOW", js_mknum(EOVERFLOW));
  js_set(js, errn, "EPERM", js_mknum(EPERM));
  js_set(js, errn, "EPIPE", js_mknum(EPIPE));
  js_set(js, errn, "EPROTONOSUPPORT", js_mknum(EPROTONOSUPPORT));
  js_set(js, errn, "EPROTOTYPE", js_mknum(EPROTOTYPE));
  js_set(js, errn, "ERANGE", js_mknum(ERANGE));
  js_set(js, errn, "EROFS", js_mknum(EROFS));
  js_set(js, errn, "ESPIPE", js_mknum(ESPIPE));
  js_set(js, errn, "ESRCH", js_mknum(ESRCH));
  js_set(js, errn, "ETIMEDOUT", js_mknum(ETIMEDOUT));
  js_set(js, errn, "ETXTBSY", js_mknum(ETXTBSY));
  js_set(js, errn, "EWOULDBLOCK", js_mknum(EWOULDBLOCK));
  js_set(js, errn, "EXDEV", js_mknum(EXDEV));
}

static void add_priority_constants(struct js *js, jsval_t priority) {
  js_set(js, priority, "PRIORITY_LOW", js_mknum(19));
  js_set(js, priority, "PRIORITY_BELOW_NORMAL", js_mknum(10));
  js_set(js, priority, "PRIORITY_NORMAL", js_mknum(0));
  js_set(js, priority, "PRIORITY_ABOVE_NORMAL", js_mknum(-7));
  js_set(js, priority, "PRIORITY_HIGH", js_mknum(-14));
  js_set(js, priority, "PRIORITY_HIGHEST", js_mknum(-20));
}

static void add_dlopen_constants(struct js *js, jsval_t dlopen_obj) {
#ifdef RTLD_LAZY
  js_set(js, dlopen_obj, "RTLD_LAZY", js_mknum(RTLD_LAZY));
#endif
#ifdef RTLD_NOW
  js_set(js, dlopen_obj, "RTLD_NOW", js_mknum(RTLD_NOW));
#endif
#ifdef RTLD_GLOBAL
  js_set(js, dlopen_obj, "RTLD_GLOBAL", js_mknum(RTLD_GLOBAL));
#endif
#ifdef RTLD_LOCAL
  js_set(js, dlopen_obj, "RTLD_LOCAL", js_mknum(RTLD_LOCAL));
#endif
#ifdef RTLD_DEEPBIND
  js_set(js, dlopen_obj, "RTLD_DEEPBIND", js_mknum(RTLD_DEEPBIND));
#endif
}

jsval_t os_library(struct js *js) {
  jsval_t lib = js_mkobj(js);
  
  js_set(js, lib, "EOL", js_mkstr(js, OS_EOL, strlen(OS_EOL)));
  js_set(js, lib, "devNull", js_mkstr(js, OS_DEVNULL, strlen(OS_DEVNULL)));
  
  js_set(js, lib, "arch", js_mkfun(os_arch));
  js_set(js, lib, "platform", js_mkfun(os_platform));
  js_set(js, lib, "type", js_mkfun(os_type));
  js_set(js, lib, "release", js_mkfun(os_release));
  js_set(js, lib, "version", js_mkfun(os_version));
  js_set(js, lib, "machine", js_mkfun(os_machine));
  js_set(js, lib, "hostname", js_mkfun(os_hostname));
  js_set(js, lib, "homedir", js_mkfun(os_homedir));
  js_set(js, lib, "tmpdir", js_mkfun(os_tmpdir));
  js_set(js, lib, "endianness", js_mkfun(os_endianness));
  js_set(js, lib, "uptime", js_mkfun(os_uptime));
  js_set(js, lib, "totalmem", js_mkfun(os_totalmem));
  js_set(js, lib, "freemem", js_mkfun(os_freemem));
  js_set(js, lib, "availableParallelism", js_mkfun(os_availableParallelism));
  js_set(js, lib, "cpus", js_mkfun(os_cpus));
  js_set(js, lib, "loadavg", js_mkfun(os_loadavg));
  js_set(js, lib, "networkInterfaces", js_mkfun(os_networkInterfaces));
  js_set(js, lib, "userInfo", js_mkfun(os_userInfo));
  js_set(js, lib, "getPriority", js_mkfun(os_getPriority));
  js_set(js, lib, "setPriority", js_mkfun(os_setPriority));
  
  jsval_t constants = js_mkobj(js);
  jsval_t signals = js_mkobj(js);
  jsval_t errn = js_mkobj(js);
  jsval_t priority = js_mkobj(js);
  jsval_t dlopen_obj = js_mkobj(js);
  
  add_signal_constants(js, signals);
  add_errno_constants(js, errn);
  add_priority_constants(js, priority);
  add_dlopen_constants(js, dlopen_obj);
  
  js_set(js, constants, "signals", signals);
  js_set(js, constants, "errno", errn);
  js_set(js, constants, "priority", priority);
  js_set(js, constants, "dlopen", dlopen_obj);
  js_set(js, constants, "UV_UDP_REUSEADDR", js_mknum(4));
  js_set(js, lib, "constants", constants);
  js_set(js, lib, get_toStringTag_sym_key(), js_mkstr(js, "os", 2));

  return lib;
}
