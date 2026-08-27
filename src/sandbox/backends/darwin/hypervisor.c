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

#define ANT_HVF_LOAD(field, symbol) do { \
  ant_hvf_api.field = (__typeof__(ant_hvf_api.field))dlsym(ant_hvf_framework, symbol); \
  if (!ant_hvf_api.field) { \
    fprintf(stderr, "sandbox vm: Hypervisor.framework symbol %s is unavailable\n", symbol); \
    return; \
  } \
} while (0)

  ANT_HVF_LOAD(vcpus_exit, "hv_vcpus_exit");
  ANT_HVF_LOAD(vcpu_config_create, "hv_vcpu_config_create");
  ANT_HVF_LOAD(vcpu_config_get_feature_reg, "hv_vcpu_config_get_feature_reg");
  ANT_HVF_LOAD(vcpu_create, "hv_vcpu_create");
  ANT_HVF_LOAD(vcpu_run, "hv_vcpu_run");
  ANT_HVF_LOAD(vcpu_get_reg, "hv_vcpu_get_reg");
  ANT_HVF_LOAD(vcpu_get_sys_reg, "hv_vcpu_get_sys_reg");
  ANT_HVF_LOAD(vcpu_destroy, "hv_vcpu_destroy");
  ANT_HVF_LOAD(vcpu_set_reg, "hv_vcpu_set_reg");
  ANT_HVF_LOAD(vcpu_set_sys_reg, "hv_vcpu_set_sys_reg");
  ANT_HVF_LOAD(vcpu_set_vtimer_mask, "hv_vcpu_set_vtimer_mask");
  ANT_HVF_LOAD(vcpu_set_pending_interrupt, "hv_vcpu_set_pending_interrupt");
  ANT_HVF_LOAD(vm_config_create, "hv_vm_config_create");
  ANT_HVF_LOAD(vm_create, "hv_vm_create");
  ANT_HVF_LOAD(vm_unmap, "hv_vm_unmap");
  ANT_HVF_LOAD(vm_destroy, "hv_vm_destroy");
  ANT_HVF_LOAD(vm_map, "hv_vm_map");

#undef ANT_HVF_LOAD

  ant_hvf_api_result = 0;
}

int ant_hvf_load_api(void) {
  pthread_once(&ant_hvf_api_once, ant_hvf_load_api_once);
  return ant_hvf_api_result;
}

void *ant_hvf_sym(const char *name) {
  if (ant_hvf_load_api() != 0) return NULL;
  return dlsym(ant_hvf_framework, name);
}

#endif
