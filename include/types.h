#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <stddef.h>

struct ant;

typedef struct ant ant_t;
typedef unsigned long long u64;

typedef size_t   jshdl_t;
typedef uint64_t jsoff_t;
typedef uint64_t jsval_t;

typedef size_t   ant_handle_t;
typedef uint64_t ant_offset_t;
typedef uint64_t ant_value_t;

#endif