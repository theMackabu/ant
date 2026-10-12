#ifndef ANT_STRINGIFY_H
#define ANT_STRINGIFY_H

#include "types.h"
#include "modules/blob.h"
#include "modules/buffer.h"

#include <stdbool.h>
#include <stddef.h>

size_t stringify_typedarray(ant_t *js, ant_value_t obj, const TypedArrayData *ta, char *buf, size_t len);
size_t stringify_arraybuffer(const ArrayBufferData *ab, char *buf, size_t len);
size_t stringify_dataview(const DataViewData *dv, char *buf, size_t len);
size_t stringify_blob(const blob_data_t *bd, bool is_file, char *buf, size_t len);

#endif
