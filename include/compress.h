#ifndef ANT_COMPRESS_H
#define ANT_COMPRESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool ant_fs_compress_supported(void);
int ant_fs_compress(const char *path, uint64_t *out_logical, uint64_t *out_physical);

#endif
