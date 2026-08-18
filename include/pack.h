#ifndef ANT_PACK_H
#define ANT_PACK_H

#include <stdint.h>

#define ANT_PACK_MAGIC "ANTPACK\x01"
#define ANT_PACK_VERSION 1u
#define ANT_PACK_ENV_EXE "ANT_PACK_EXE"

typedef struct {
  char magic[8];
  uint32_t version;
  uint32_t reserved;
  uint64_t payload_offset;
  uint64_t payload_size;
  uint64_t original_size;
} ant_pack_footer_t;

#endif
