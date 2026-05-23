#ifndef PKG_LOCKFILE_H
#define PKG_LOCKFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOCKFILE_MAGIC 0x504B474C
#define LOCKFILE_VERSION 1

typedef struct {
  uint32_t offset;
  uint32_t len;
} lockfile_string_ref_t;

inline lockfile_string_ref_t lockfile_string_ref_empty(void) {
  return (lockfile_string_ref_t){.offset = 0, .len = 0};
}

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t package_count;
  uint32_t dependency_count;
  uint32_t string_table_offset;
  uint32_t string_table_size;
  uint32_t package_array_offset;
  uint32_t dependency_array_offset;
  uint32_t hash_table_offset;
  uint32_t hash_table_size;
  uint8_t _reserved[24];
} lockfile_header_t;

typedef union {
  struct {
    uint8_t dev : 1;
    uint8_t optional : 1;
    uint8_t peer : 1;
    uint8_t bundled : 1;
    uint8_t has_bin : 1;
    uint8_t has_scripts : 1;
    uint8_t direct : 1;
    uint8_t _reserved : 1;
  };
  uint8_t raw;
} lockfile_package_flags_t;

typedef struct {
  lockfile_string_ref_t name;
  uint64_t version_major;
  uint64_t version_minor;
  uint64_t version_patch;
  lockfile_string_ref_t prerelease;
  uint8_t integrity[64];
  lockfile_string_ref_t tarball_url;
  lockfile_string_ref_t parent_path;
  uint32_t deps_start;
  uint32_t deps_count;
  lockfile_package_flags_t flags;
  uint8_t _padding[3];
} lockfile_package_t;

typedef union {
  struct {
    uint8_t peer : 1;
    uint8_t dev : 1;
    uint8_t optional : 1;
    uint8_t _reserved : 5;
  };
  uint8_t raw;
} lockfile_dependency_flags_t;

typedef struct {
  uint32_t package_index;
  lockfile_string_ref_t constraint;
  lockfile_dependency_flags_t flags;
  uint8_t _padding[3];
} lockfile_dependency_t;

typedef struct {
  uint32_t name_hash;
  uint32_t package_index;
} lockfile_hash_bucket_t;

// Static assertions to ensure exact alignment and size matching with Zig.
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(lockfile_header_t) == 64,
               "lockfile_header_t must be 64 bytes");
_Static_assert(sizeof(lockfile_package_t) == 136,
               "lockfile_package_t must be 136 bytes");
_Static_assert(sizeof(lockfile_dependency_t) == 16,
               "lockfile_dependency_t must be 16 bytes");
_Static_assert(sizeof(lockfile_hash_bucket_t) == 8,
               "lockfile_hash_bucket_t must be 8 bytes");
#endif

typedef struct {
  void *mapped_data;
  size_t mapped_size;
  const lockfile_header_t *header;
  const char *string_table;
  const lockfile_package_t *packages;
  const lockfile_dependency_t *dependencies;
  const lockfile_hash_bucket_t *hash_table;
} lockfile_t;

bool lockfile_open(const char *path, lockfile_t *lf);
void lockfile_close(lockfile_t *lf);

const lockfile_package_t *lockfile_lookup_package(const lockfile_t *lf,
                                                  const char *name);

// Helper to resolve a StringRef to a const char * slice (length is returned in
// out_len)
inline const char *lockfile_string_ref_slice(const lockfile_t *lf,
                                             lockfile_string_ref_t ref,
                                             size_t *out_len) {
  if (ref.offset >= lf->header->string_table_size) {
    *out_len = 0;
    return "";
  }
  size_t end = ref.offset + ref.len;
  if (end > lf->header->string_table_size) {
    end = lf->header->string_table_size;
  }
  *out_len = end - ref.offset;
  return lf->string_table + ref.offset;
}

// Writer representation
typedef struct {
  char *string_builder;
  size_t string_builder_size;
  size_t string_builder_capacity;

  lockfile_package_t *packages;
  size_t package_count;
  size_t package_capacity;

  lockfile_dependency_t *dependencies;
  size_t dependency_count;
  size_t dependency_capacity;
} lockfile_writer_t;

void lockfile_writer_init(lockfile_writer_t *writer);
void lockfile_writer_deinit(lockfile_writer_t *writer);

lockfile_string_ref_t lockfile_writer_intern_string(lockfile_writer_t *writer,
                                                    const char *str,
                                                    size_t len);
uint32_t lockfile_writer_add_package(lockfile_writer_t *writer,
                                     lockfile_package_t pkg);
void lockfile_writer_add_dependency(lockfile_writer_t *writer,
                                    lockfile_dependency_t dep);

bool lockfile_writer_write(lockfile_writer_t *writer, const char *path);

#endif // PKG_LOCKFILE_H
