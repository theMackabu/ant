#ifndef ANT_UTILS_H
#define ANT_UTILS_H
#define ARGTABLE_COUNT 10

#include <stdlib.h>
#include <stdint.h>

uint64_t hash_key(const char *key, size_t len);
int is_typescript_file(const char *filename);
int ant_version(void *argtable[]);

#endif