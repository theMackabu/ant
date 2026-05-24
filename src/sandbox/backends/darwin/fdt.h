#pragma once

#include <compat.h> // IWYU pragma: keep

#include "sandbox_backend/forward.h"

#include <stddef.h>
#include <stdint.h>

#if defined(__aarch64__)

#define ANT_HVF_DTB_MAX 0x20000u
#define ANT_FDT_STRUCTURE_BYTES 8192u
#define ANT_FDT_STRINGS_BYTES 2048u
#define ANT_FDT_MAX_PROP_CELLS_32 32u
#define ANT_FDT_MAX_PROP_CELLS_64 16u

#define FDT_MAGIC 0xd00dfeedu
#define FDT_BEGIN_NODE 1u
#define FDT_END_NODE 2u
#define FDT_PROP 3u
#define FDT_END 9u

typedef struct {
  uint32_t magic;
  uint32_t totalsize;
  uint32_t off_dt_struct;
  uint32_t off_dt_strings;
  uint32_t off_mem_rsvmap;
  uint32_t version;
  uint32_t last_comp_version;
  uint32_t boot_cpuid_phys;
  uint32_t size_dt_strings;
  uint32_t size_dt_struct;
} ant_fdt_header_t;

typedef struct {
  unsigned char *structure;
  unsigned char *strings;
  size_t structure_len;
  size_t structure_cap;
  size_t strings_len;
  size_t strings_cap;
} ant_fdt_t;

int ant_fdt_init(ant_fdt_t *fdt);
void ant_fdt_free(ant_fdt_t *fdt);
int ant_fdt_reserve(ant_fdt_t *fdt, size_t len, unsigned char **out);
int ant_fdt_u32(ant_fdt_t *fdt, uint32_t val);
int ant_fdt_string_offset(ant_fdt_t *fdt, const char *name);
int ant_fdt_begin(ant_fdt_t *fdt, const char *name);
int ant_fdt_end(ant_fdt_t *fdt);
int ant_fdt_prop(ant_fdt_t *fdt, const char *name, const void *data, size_t len);
int ant_fdt_prop_null(ant_fdt_t *fdt, const char *name);
int ant_fdt_prop_string(ant_fdt_t *fdt, const char *name, const char *value);
int ant_fdt_prop_u32(ant_fdt_t *fdt, const char *name, uint32_t value);
int ant_fdt_prop_cells(ant_fdt_t *fdt, const char *name, const uint32_t *cells, size_t count);
int ant_fdt_prop_reg64(ant_fdt_t *fdt, const char *name, const uint64_t *cells, size_t count);
int ant_hvf_build_dtb(ant_hvf_vm_t *vm);

#endif
