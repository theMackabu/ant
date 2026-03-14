#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <stddef.h>

typedef unsigned long long u64;

struct ant;
struct ant_object;
struct ant_shape;

typedef struct ant ant_t;
typedef struct ant_object ant_object_t;
typedef struct ant_shape  ant_shape_t;

typedef size_t   ant_handle_t;
typedef uint64_t ant_offset_t;
typedef uint64_t ant_value_t;

#endif
