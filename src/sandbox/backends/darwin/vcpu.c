#include "backend.h"
#include <mach/mach_time.h>

#if defined(__aarch64__)

static constexpr uint64_t ANT_HVF_WFI_MAX_WAIT_NS = 100000000ull;

int ant_hvf_set_reg(hv_vcpu_t vcpu, hv_reg_t reg, uint64_t value, const char *name) {
  int rc = ant_hvf_check(ant_hvf_api.vcpu_set_reg(vcpu, reg, value), name);
  return rc;
}

int ant_hvf_guest_cntvct(ant_hvf_vm_t *vm, uint64_t *out) {
  uint64_t offset = 0;
  
  int rc = ant_hvf_check(
    ant_hvf_api.vcpu_get_vtimer_offset(vm->vcpu, &offset),
    "hv_vcpu_get_vtimer_offset"
  );
  
  if (rc != 0) return rc;
  *out = mach_absolute_time() - offset;
  
  return 0;
}

int ant_hvf_init_vcpu(ant_hvf_vm_t *vm) {
  int rc = ant_hvf_set_reg(vm->vcpu, HV_REG_PC, vm->kernel_entry, "hv_vcpu_set_reg(PC)");
  if (rc != 0) return rc;
  
  rc = ant_hvf_set_reg(vm->vcpu, HV_REG_X0, ANT_HVF_DTB_BASE, "hv_vcpu_set_reg(X0)");
  if (rc != 0) return rc;
  
  rc = ant_hvf_set_reg(vm->vcpu, HV_REG_X1, 0, "hv_vcpu_set_reg(X1)");
  if (rc != 0) return rc;
  
  rc = ant_hvf_set_reg(vm->vcpu, HV_REG_CPSR, 0x3c5, "hv_vcpu_set_reg(CPSR)");
  if (rc != 0) return rc;
  
  rc = ant_hvf_check(ant_hvf_api.vcpu_set_sys_reg(vm->vcpu, HV_SYS_REG_MPIDR_EL1, 0), "hv_vcpu_set_sys_reg(MPIDR_EL1)");
  if (rc != 0) return rc;
  
  hv_return_t cntfrq_rc = ant_hvf_api.vcpu_set_sys_reg(vm->vcpu, ANT_HVF_SYS_REG_CNTFRQ_EL0, vm->cntfrq);
  
  if (cntfrq_rc == HV_SUCCESS) ant_hvf_verbosef(vm, "seeded CNTFRQ_EL0=%llu", (unsigned long long)vm->cntfrq);
  else ant_hvf_verbosef(vm, "CNTFRQ_EL0 sysreg seed unavailable rc=%d; using timer frequency from device tree", cntfrq_rc);
  
  return ant_hvf_check(ant_hvf_api.vcpu_set_vtimer_mask(vm->vcpu, false), "hv_vcpu_set_vtimer_mask(init)");
}

static void ant_hvf_idle_wait(ant_hvf_vm_t *vm, uint64_t ns) {
  struct timespec ts = {
    .tv_sec = (time_t)(ns / 1000000000ull),
    .tv_nsec = (long)(ns % 1000000000ull),
  };
  
  atomic_store_explicit(&vm->vcpu_running, true, memory_order_release);
  pthread_mutex_lock(&vm->idle_lock);
  
  if (!vm->idle_kicked && !atomic_load_explicit(&vm->vsock_wake_pending, memory_order_acquire))
    pthread_cond_timedwait_relative_np(&vm->idle_cond, &vm->idle_lock, &ts);
  
  vm->idle_kicked = false;
  pthread_mutex_unlock(&vm->idle_lock);
  atomic_store_explicit(&vm->vcpu_running, false, memory_order_release);
}

int ant_hvf_handle_wfx(ant_hvf_vm_t *vm) {
  bool wfe = (vm->vcpu_exit->exception.syndrome & ESR_WFX_TI_WFE) != 0;
  int rc = ant_hvf_advance_pc(vm->vcpu);
  
  if (rc != 0) return rc;
  if (wfe) return ant_hvf_sync_vtimer(vm);

  uint64_t ctl = 0;
  rc = ant_hvf_check(
    ant_hvf_api.vcpu_get_sys_reg(vm->vcpu, HV_SYS_REG_CNTV_CTL_EL0, &ctl),
    "hv_vcpu_get_sys_reg(CNTV_CTL_EL0)"
  );
  if (rc != 0) return rc;

  uint64_t ns = ANT_HVF_WFI_MAX_WAIT_NS;
  if ((ctl & 1u) != 0 && (ctl & 2u) == 0) {
    uint64_t cval = 0;
    rc = ant_hvf_check(
      ant_hvf_api.vcpu_get_sys_reg(vm->vcpu, HV_SYS_REG_CNTV_CVAL_EL0, &cval),
      "hv_vcpu_get_sys_reg(CNTV_CVAL_EL0)"
    );
    if (rc != 0) return rc;

    uint64_t now = 0;
    rc = ant_hvf_guest_cntvct(vm, &now);
    if (rc != 0) return rc;
    if (cval <= now) ns = 0;
    else if ((cval - now) < vm->cntfrq / 10) ns = ((cval - now) * 1000000000ull) / vm->cntfrq;
  }

  if (ns > 0) ant_hvf_idle_wait(vm, ns);
  return ant_hvf_sync_vtimer(vm);
}

int ant_hvf_sync_vtimer(ant_hvf_vm_t *vm) {
  uint64_t ctl = 0;
  uint64_t cval = 0;
  
  if (ant_hvf_api.vcpu_get_sys_reg(vm->vcpu, HV_SYS_REG_CNTV_CTL_EL0, &ctl) != HV_SUCCESS ||
    ant_hvf_api.vcpu_get_sys_reg(vm->vcpu, HV_SYS_REG_CNTV_CVAL_EL0, &cval) != HV_SUCCESS
  ) return 0;
  
  if ((ctl & 1u) == 0 || (ctl & 2u) != 0) return 0;
  
  uint64_t now = 0;
  int rc = ant_hvf_guest_cntvct(vm, &now);
  
  if (rc != 0) return rc;
  if (now < cval) return 0;
  
  return ant_hvf_raise_vtimer(vm);
}

typedef struct {
  const char *name;
  hv_sys_reg_t reg;
} ant_hvf_report_reg_t;

static void ant_hvf_dump_sys_regs(ant_hvf_vm_t *vm, ant_hvf_dump_t *dump, const ant_hvf_report_reg_t *regs, size_t count) {
  for (size_t i = 0; i < count; i++) {
    uint64_t value = 0;
    ant_hvf_api.vcpu_get_sys_reg(vm->vcpu, regs[i].reg, &value);
    ant_hvf_dump_add(dump, regs[i].name, value);
  }
}

static uint64_t ant_hvf_dump_pc(ant_hvf_vm_t *vm, ant_hvf_dump_t *dump) {
  uint64_t pc = 0;
  ant_hvf_api.vcpu_get_reg(vm->vcpu, HV_REG_PC, &pc);
  ant_hvf_dump_add(dump, "pc", pc);
  return pc;
}

void ant_hvf_report_exception(ant_hvf_vm_t *vm) {
  static const ant_hvf_report_reg_t regs[] = {
    { "elr", HV_SYS_REG_ELR_EL1 },
    { "guest_esr", HV_SYS_REG_ESR_EL1 },
    { "far", HV_SYS_REG_FAR_EL1 },
    { "vbar", HV_SYS_REG_VBAR_EL1 },
  };

  ant_hvf_dump_t dump = {0};
  ant_hvf_dump_pc(vm, &dump);
  ant_hvf_dump_add(&dump, "esr", vm->vcpu_exit->exception.syndrome);
  ant_hvf_dump_add(&dump, "ipa", vm->vcpu_exit->exception.physical_address);
  ant_hvf_dump_add(&dump, "va", vm->vcpu_exit->exception.virtual_address);
  ant_hvf_dump_sys_regs(vm, &dump, regs, sizeof(regs) / sizeof(regs[0]));
  ant_hvf_dump_report(&dump, "unhandled guest exception at");
}

void ant_hvf_report_timeout(ant_hvf_vm_t *vm) {
  static const ant_hvf_report_reg_t regs[] = {
    { "cntv_ctl", HV_SYS_REG_CNTV_CTL_EL0 },
    { "cntv_cval", HV_SYS_REG_CNTV_CVAL_EL0 },
    { "cntvct", ANT_HVF_SYS_REG_CNTVCT_EL0 },
    { "cntfrq", ANT_HVF_SYS_REG_CNTFRQ_EL0 },
  };

  uint64_t kas_offset = 0;
  ant_hvf_guest_read(vm, ANT_HVF_NANOS_KAS_OFFSET_SYMBOL, &kas_offset, sizeof(kas_offset));

  ant_hvf_dump_t dump = {0};
  uint64_t pc = ant_hvf_dump_pc(vm, &dump);
  ant_hvf_dump_add(&dump, "low_pc", kas_offset ? pc - kas_offset : 0);
  ant_hvf_dump_add(&dump, "kas_offset", kas_offset);
  ant_hvf_dump_add(&dump, "last_exit", vm->last_exit_reason);
  ant_hvf_dump_add(&dump, "last_pc", vm->last_exit_pc);
  ant_hvf_dump_add(&dump, "last_esr", vm->last_exit_esr);
  ant_hvf_dump_add(&dump, "last_ipa", vm->last_exit_ipa);
  ant_hvf_dump_add(&dump, "last_va", vm->last_exit_va);
  ant_hvf_dump_sys_regs(vm, &dump, regs, sizeof(regs) / sizeof(regs[0]));
  ant_hvf_dump_report(&dump, "guest timed out at");
}

#endif
