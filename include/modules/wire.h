#ifndef ANT_WIRE_H
#define ANT_WIRE_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "types.h"
#include "modules/buffer.h"

typedef struct ant_port_end ant_port_end_t;

typedef struct ant_wire {
  _Atomic int refs;
  uint8_t *data;
  size_t len;
  size_t cap;
  ArrayBufferData **shared;
  uint32_t shared_count;
  uint32_t shared_cap;
  ArrayBufferData **moved;
  uint32_t moved_count;
  struct blob_storage **blobs;
  uint32_t blob_count;
  uint32_t blob_cap;
  ant_port_end_t **ports;
  uint32_t port_count;
  uint32_t port_cap;
} ant_wire_t;

ant_wire_t *ant_wire_retain(ant_wire_t *wire);
void ant_wire_release(ant_wire_t *wire);

ant_value_t ant_wire_serialize(
  ant_t *js, ant_value_t value, ant_value_t transfer_list,
  ant_value_t source_port, ant_wire_t **out
);

ant_value_t ant_wire_serialize_error(ant_t *js, ant_value_t error, ant_wire_t **out);
ant_value_t ant_wire_mark_uncloneable(ant_t *js, ant_value_t value);
ant_value_t ant_wire_deserialize(ant_t *js, ant_wire_t *wire, ant_value_t *ports_out);
ant_value_t ant_wire_mark_untransferable(ant_t *js, ant_value_t value);

bool ant_wire_is_untransferable(ant_t *js, ant_value_t value);
bool ant_wire_is_uncloneable(ant_t *js, ant_value_t value);

#endif
