#include "hvf.h"

#if defined(__aarch64__)

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>

ant_hvf_api_t ant_hvf_api;

static pthread_once_t ant_hvf_api_once = PTHREAD_ONCE_INIT;
static void *ant_hvf_framework;
static int ant_hvf_api_result = -ENOSYS;

static void ant_hvf_load_api_once(void) {
  ant_hvf_framework = dlopen(
    "/System/Library/Frameworks/Hypervisor.framework/Hypervisor",
    RTLD_LAZY | RTLD_LOCAL
  );
  if (!ant_hvf_framework) {
    fprintf(stderr, "sandbox vm: failed to load Hypervisor.framework: %s\n", dlerror());
    return;
  }

#define HVF(name)                                                                             \
  ant_hvf_api.name = (__typeof__(ant_hvf_api.name))dlsym(ant_hvf_framework, "hv_" #name);     \
  if (!ant_hvf_api.name) {                                                                    \
    fprintf(stderr, "sandbox vm: Hypervisor.framework symbol hv_" #name " is unavailable\n"); \
    return;                                                                                   \
  }
#include "hvf_symbols.h"

  ant_hvf_api.vm_config_set_ipa_granule = (__typeof__(ant_hvf_api.vm_config_set_ipa_granule))
    dlsym(ant_hvf_framework, "hv_vm_config_set_ipa_granule");

  ant_hvf_api_result = 0;
}

int ant_hvf_load_api(void) {
  pthread_once(&ant_hvf_api_once, ant_hvf_load_api_once);
  return ant_hvf_api_result;
}

#endif
