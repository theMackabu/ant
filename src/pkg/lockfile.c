#include "lockfile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static uint32_t djb2_hash(const char *str, size_t len) {
  uint32_t hash = 5381;
  for (size_t i = 0; i < len; i++) {
    hash = ((hash << 5) + hash) + (uint8_t)str[i];
  }
  return hash;
}

bool lockfile_open(const char *path, lockfile_t *lf) {
  memset(lf, 0, sizeof(*lf));

#ifdef _WIN32
  FILE *f = fopen(path, "rb");
  if (!f)
    return false;

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (size < (long)sizeof(lockfile_header_t)) {
    fclose(f);
    return false;
  }

  void *data = malloc(size);
  if (!data) {
    fclose(f);
    return false;
  }

  if (fread(data, 1, size, f) != (size_t)size) {
    free(data);
    fclose(f);
    return false;
  }
  fclose(f);

  lf->mapped_data = data;
  lf->mapped_size = size;
#else
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return false;

  struct stat st;
  if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(lockfile_header_t)) {
    close(fd);
    return false;
  }

  void *data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);

  if (data == MAP_FAILED)
    return false;

  lf->mapped_data = data;
  lf->mapped_size = st.st_size;
#endif

  lf->header = (const lockfile_header_t *)lf->mapped_data;
  if (lf->header->magic != LOCKFILE_MAGIC ||
      lf->header->version != LOCKFILE_VERSION) {
    lockfile_close(lf);
    return false;
  }

  // Validate offsets
  size_t size_check = lf->mapped_size;
  if ((size_t)lf->header->string_table_offset + lf->header->string_table_size >
          size_check ||
      (size_t)lf->header->package_array_offset +
              lf->header->package_count * sizeof(lockfile_package_t) >
          size_check ||
      (size_t)lf->header->dependency_array_offset +
              lf->header->dependency_count * sizeof(lockfile_dependency_t) >
          size_check ||
      (size_t)lf->header->hash_table_offset +
              lf->header->hash_table_size * sizeof(lockfile_hash_bucket_t) >
          size_check) {
    lockfile_close(lf);
    return false;
  }

  lf->string_table =
      (const char *)lf->mapped_data + lf->header->string_table_offset;
  lf->packages = (const lockfile_package_t *)((const char *)lf->mapped_data +
                                              lf->header->package_array_offset);
  lf->dependencies =
      (const lockfile_dependency_t *)((const char *)lf->mapped_data +
                                      lf->header->dependency_array_offset);
  lf->hash_table =
      (const lockfile_hash_bucket_t *)((const char *)lf->mapped_data +
                                       lf->header->hash_table_offset);

  return true;
}

void lockfile_close(lockfile_t *lf) {
  if (lf->mapped_data) {
#ifdef _WIN32
    free(lf->mapped_data);
#else
    munmap(lf->mapped_data, lf->mapped_size);
#endif
  }
  memset(lf, 0, sizeof(*lf));
}

const lockfile_package_t *lockfile_lookup_package(const lockfile_t *lf,
                                                  const char *name) {
  if (lf->header->hash_table_size == 0)
    return NULL;

  size_t name_len = strlen(name);
  uint32_t hash = djb2_hash(name, name_len);
  uint32_t index = hash % lf->header->hash_table_size;
  uint32_t probes = 0;

  while (probes < lf->header->hash_table_size) {
    const lockfile_hash_bucket_t *bucket = &lf->hash_table[index];
    if (bucket->package_index == 0xFFFFFFFF) {
      return NULL;
    }

    if (bucket->name_hash == hash) {
      const lockfile_package_t *pkg = &lf->packages[bucket->package_index];
      size_t pkg_name_len;
      const char *pkg_name =
          lockfile_string_ref_slice(lf, pkg->name, &pkg_name_len);
      if (pkg_name_len == name_len && memcmp(pkg_name, name, name_len) == 0) {
        return pkg;
      }
    }
    index = (index + 1) % lf->header->hash_table_size;
    probes++;
  }

  return NULL;
}

void lockfile_writer_init(lockfile_writer_t *writer) {
  memset(writer, 0, sizeof(*writer));
}

void lockfile_writer_deinit(lockfile_writer_t *writer) {
  if (writer->string_builder)
    free(writer->string_builder);
  if (writer->packages)
    free(writer->packages);
  if (writer->dependencies)
    free(writer->dependencies);
  memset(writer, 0, sizeof(*writer));
}

lockfile_string_ref_t lockfile_writer_intern_string(lockfile_writer_t *writer,
                                                    const char *str,
                                                    size_t len) {
  if (len == 0) {
    return lockfile_string_ref_empty();
  }

  // Scan for existing string to deduplicate in lockfile string builder
  for (size_t offset = 0; offset < writer->string_builder_size;) {
    size_t remaining = writer->string_builder_size - offset;
    if (remaining < len) {
      break;
    }
    if (memcmp(writer->string_builder + offset, str, len) == 0) {
      // Ensure we match the whole string boundary if we want exact matches,
      // but simple prefix/substring reuse is also fine. Let's do exact match
      // to avoid partial overlap bugs (or scan by null terminator if
      // null-separated). Since lockfile.zig just does direct check: it doesn't
      // scan substrings, it uses a HashMap to intern. Let's implement string
      // table scanning properly or use a simple hash map. Scanning the builder
      // is slow if there are many strings, but lockfiles are relatively small.
      // To be safe and identical, let's scan.
      // Wait, is it null terminated? In lockfile.zig, the string table is just
      // concatenated strings. If we match exactly, we check if it matches the
      // key in string_offsets map. Let's check if the offsets map matches.
      // Since we want to be simple, we can just do exact check against
      // previously added strings.
    }
    offset++; // simplified
  }

  // To be robust and match Zig's Map, we can just scan for the string at
  // matching offsets or simple exact comparison. Let's do a simple lookup:
  for (size_t i = 0; i < writer->package_count; i++) {
    const lockfile_package_t *pkg = &writer->packages[i];
    // check if name matches
    if (pkg->name.len == len &&
        memcmp(writer->string_builder + pkg->name.offset, str, len) == 0) {
      return pkg->name;
    }
    if (pkg->prerelease.len == len &&
        memcmp(writer->string_builder + pkg->prerelease.offset, str, len) ==
            0) {
      return pkg->prerelease;
    }
    if (pkg->tarball_url.len == len &&
        memcmp(writer->string_builder + pkg->tarball_url.offset, str, len) ==
            0) {
      return pkg->tarball_url;
    }
    if (pkg->parent_path.len == len &&
        memcmp(writer->string_builder + pkg->parent_path.offset, str, len) ==
            0) {
      return pkg->parent_path;
    }
  }

  for (size_t i = 0; i < writer->dependency_count; i++) {
    const lockfile_dependency_t *dep = &writer->dependencies[i];
    if (dep->constraint.len == len &&
        memcmp(writer->string_builder + dep->constraint.offset, str, len) ==
            0) {
      return dep->constraint;
    }
  }

  // Not found, append
  if (writer->string_builder_size + len > writer->string_builder_capacity) {
    size_t new_cap = writer->string_builder_capacity == 0
                         ? 1024
                         : writer->string_builder_capacity * 2;
    while (writer->string_builder_size + len > new_cap) {
      new_cap *= 2;
    }
    char *new_builder = realloc(writer->string_builder, new_cap);
    if (!new_builder) {
      return lockfile_string_ref_empty();
    }
    writer->string_builder = new_builder;
    writer->string_builder_capacity = new_cap;
  }

  uint32_t offset = (uint32_t)writer->string_builder_size;
  memcpy(writer->string_builder + offset, str, len);
  writer->string_builder_size += len;

  return (lockfile_string_ref_t){.offset = offset, .len = (uint32_t)len};
}

uint32_t lockfile_writer_add_package(lockfile_writer_t *writer,
                                     lockfile_package_t pkg) {
  if (writer->package_count >= writer->package_capacity) {
    size_t new_cap =
        writer->package_capacity == 0 ? 32 : writer->package_capacity * 2;
    lockfile_package_t *new_packages =
        realloc(writer->packages, new_cap * sizeof(lockfile_package_t));
    if (!new_packages) {
      return 0; // Out of memory
    }
    writer->packages = new_packages;
    writer->package_capacity = new_cap;
  }

  uint32_t index = (uint32_t)writer->package_count;
  writer->packages[writer->package_count++] = pkg;
  return index;
}

void lockfile_writer_add_dependency(lockfile_writer_t *writer,
                                    lockfile_dependency_t dep) {
  if (writer->dependency_count >= writer->dependency_capacity) {
    size_t new_cap =
        writer->dependency_capacity == 0 ? 32 : writer->dependency_capacity * 2;
    lockfile_dependency_t *new_dependencies =
        realloc(writer->dependencies, new_cap * sizeof(lockfile_dependency_t));
    if (!new_dependencies) {
      return;
    }
    writer->dependencies = new_dependencies;
    writer->dependency_capacity = new_cap;
  }

  writer->dependencies[writer->dependency_count++] = dep;
}

static uint32_t align_offset(uint32_t offset, uint32_t alignment) {
  uint32_t rem = offset % alignment;
  return (rem == 0) ? offset : offset + (alignment - rem);
}

bool lockfile_writer_write(lockfile_writer_t *writer, const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return false;

  uint32_t header_size = sizeof(lockfile_header_t);
  uint32_t string_table_offset = header_size;
  uint32_t string_table_size = (uint32_t)writer->string_builder_size;

  uint32_t package_array_offset = align_offset(
      string_table_offset + string_table_size, _Alignof(lockfile_package_t));
  uint32_t package_pad_size =
      package_array_offset - (string_table_offset + string_table_size);
  uint32_t package_array_size =
      (uint32_t)(writer->package_count * sizeof(lockfile_package_t));

  uint32_t dependency_array_offset =
      align_offset(package_array_offset + package_array_size,
                   _Alignof(lockfile_dependency_t));
  uint32_t dep_pad_size =
      dependency_array_offset - (package_array_offset + package_array_size);
  uint32_t dependency_array_size =
      (uint32_t)(writer->dependency_count * sizeof(lockfile_dependency_t));

  uint32_t hash_table_size = (uint32_t)(writer->package_count * 10 / 7);
  if (hash_table_size == 0)
    hash_table_size = 1;

  lockfile_hash_bucket_t *hash_table =
      malloc(hash_table_size * sizeof(lockfile_hash_bucket_t));
  if (!hash_table) {
    fclose(f);
    return false;
  }

  for (uint32_t i = 0; i < hash_table_size; i++) {
    hash_table[i] =
        (lockfile_hash_bucket_t){.name_hash = 0, .package_index = 0xFFFFFFFF};
  }

  for (size_t i = 0; i < writer->package_count; i++) {
    const lockfile_package_t *pkg = &writer->packages[i];
    size_t name_len = pkg->name.len;
    const char *name = writer->string_builder + pkg->name.offset;

    uint32_t hash = djb2_hash(name, name_len);
    uint32_t index = hash % hash_table_size;
    uint32_t probes = 0;

    while (hash_table[index].package_index != 0xFFFFFFFF &&
           probes < hash_table_size) {
      index = (index + 1) % hash_table_size;
      probes++;
    }

    if (probes >= hash_table_size) {
      free(hash_table);
      fclose(f);
      return false; // Table full error
    }

    hash_table[index] = (lockfile_hash_bucket_t){.name_hash = hash,
                                                 .package_index = (uint32_t)i};
  }

  uint32_t hash_table_offset =
      align_offset(dependency_array_offset + dependency_array_size,
                   _Alignof(lockfile_hash_bucket_t));
  uint32_t hash_pad_size =
      hash_table_offset - (dependency_array_offset + dependency_array_size);

  lockfile_header_t header = {
      .magic = LOCKFILE_MAGIC,
      .version = LOCKFILE_VERSION,
      .package_count = (uint32_t)writer->package_count,
      .dependency_count = (uint32_t)writer->dependency_count,
      .string_table_offset = string_table_offset,
      .string_table_size = string_table_size,
      .package_array_offset = package_array_offset,
      .dependency_array_offset = dependency_array_offset,
      .hash_table_offset = hash_table_offset,
      .hash_table_size = hash_table_size,
  };
  memset(header._reserved, 0, sizeof(header._reserved));

  fwrite(&header, 1, sizeof(header), f);
  if (string_table_size > 0) {
    fwrite(writer->string_builder, 1, string_table_size, f);
  }

  uint8_t zero_pad[16] = {0};

  if (package_pad_size > 0) {
    fwrite(zero_pad, 1, package_pad_size, f);
  }
  if (writer->package_count > 0) {
    fwrite(writer->packages, 1, package_array_size, f);
  }

  if (dep_pad_size > 0) {
    fwrite(zero_pad, 1, dep_pad_size, f);
  }
  if (writer->dependency_count > 0) {
    fwrite(writer->dependencies, 1, dependency_array_size, f);
  }

  if (hash_pad_size > 0) {
    fwrite(zero_pad, 1, hash_pad_size, f);
  }
  fwrite(hash_table, 1, hash_table_size * sizeof(lockfile_hash_bucket_t), f);

  free(hash_table);
  fclose(f);
  return true;
}
