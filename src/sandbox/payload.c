#include <compat.h> // IWYU pragma: keep

#include "sandbox/sandbox.h"
#include "frame_reader.h"

bool ant_sandbox_result_payload_display(const void *payload, size_t payload_len, const char **out, size_t *out_len) {
  if (!payload || !out || !out_len || payload_len < 1) return false;
  
  sandbox_frame_reader_t r = { payload, (const uint8_t *)payload + payload_len };
  uint8_t type = *r.p++;
  
  if (type == ANT_SANDBOX_VALUE_BOOL) {
    if (r.p >= r.end) return false;
    r.p++;
  } else if (type == ANT_SANDBOX_VALUE_NUMBER) {
    uint64_t bits;
    if (!sandbox_read_u64(&r, &bits)) return false;
  } else if (type == ANT_SANDBOX_VALUE_STRING) {
    const char *str;
    size_t len;
    if (!sandbox_read_string_view(&r, &str, &len)) return false;
  } else if (
    type != ANT_SANDBOX_VALUE_UNDEFINED && 
    type != ANT_SANDBOX_VALUE_NULL && type 
    != ANT_SANDBOX_VALUE_DISPLAY
  ) return false;
  
  return sandbox_read_string_view(&r, out, out_len) && r.p == r.end;
}

bool ant_sandbox_error_payload_display(const void *payload, size_t payload_len, const char **out, size_t *out_len) {
  if (!payload || !out || !out_len) return false;
  
  sandbox_frame_reader_t r = { payload, (const uint8_t *)payload + payload_len };
  const char *unused = NULL;
  size_t unused_len = 0;
  
  if (!sandbox_read_string_view(&r, &unused, &unused_len)) return false;
  if (!sandbox_read_string_view(&r, &unused, &unused_len)) return false;
  if (!sandbox_read_string_view(&r, &unused, &unused_len)) return false;
  
  return sandbox_read_string_view(&r, out, out_len) && r.p == r.end;
}
