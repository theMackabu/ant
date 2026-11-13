#include <compat.h> // IWYU pragma: keep

#include "protocol.h"

typedef struct {
  uint8_t *data;
  size_t len;
  size_t cap;
} sandbox_helper_buffer_t;

typedef struct {
  const uint8_t *p;
  const uint8_t *end;
} sandbox_helper_reader_t;

static bool sandbox_helper_append(sandbox_helper_buffer_t *buf, const void *data, size_t len) {
  if (!buf || (!data && len > 0) || buf->len > ANT_SANDBOX_FRAME_MAX_SIZE - len) return false;
  size_t needed = buf->len + len;
  if (needed > buf->cap) {
    size_t cap = buf->cap ? buf->cap * 2 : 512;
    while (cap < needed) cap *= 2;
    uint8_t *next = realloc(buf->data, cap);
    if (!next) return false;
    buf->data = next;
    buf->cap = cap;
  }
  if (len > 0) memcpy(buf->data + buf->len, data, len);
  buf->len = needed;
  return true;
}

static bool sandbox_helper_append_u8(sandbox_helper_buffer_t *buf, uint8_t value) {
  return sandbox_helper_append(buf, &value, sizeof(value));
}

static bool sandbox_helper_append_u16(sandbox_helper_buffer_t *buf, uint16_t value) {
  uint8_t raw[2] = { (uint8_t)value, (uint8_t)(value >> 8) };
  return sandbox_helper_append(buf, raw, sizeof(raw));
}

static bool sandbox_helper_append_u32(sandbox_helper_buffer_t *buf, uint32_t value) {
  uint8_t raw[4] = {
    (uint8_t)value, (uint8_t)(value >> 8),
    (uint8_t)(value >> 16), (uint8_t)(value >> 24),
  };
  return sandbox_helper_append(buf, raw, sizeof(raw));
}

static bool sandbox_helper_append_u64(sandbox_helper_buffer_t *buf, uint64_t value) {
  uint8_t raw[8];
  for (unsigned i = 0; i < 8; i++) raw[i] = (uint8_t)(value >> (i * 8));
  return sandbox_helper_append(buf, raw, sizeof(raw));
}

static bool sandbox_helper_append_string(sandbox_helper_buffer_t *buf, const char *value) {
  size_t len = value ? strlen(value) : 0;
  return len <= UINT32_MAX && sandbox_helper_append_u32(buf, (uint32_t)len) &&
    sandbox_helper_append(buf, value, len);
}

uint8_t *sandbox_helper_encode_config(const ant_sandbox_vm_config_t *config, size_t *len_out) {
  if (!config || !config->image_path || !config->kernel_path || !len_out ||
      config->mount_count > UINT32_MAX || config->forward_count > UINT32_MAX ||
      config->allow_host_port_count > UINT32_MAX) return NULL;
  sandbox_helper_buffer_t buf = {0};
  bool ok =
    sandbox_helper_append_string(&buf, config->image_path) &&
    sandbox_helper_append_string(&buf, config->kernel_path) &&
    sandbox_helper_append_u32(&buf, config->capabilities) &&
    sandbox_helper_append_u8(&buf, config->network_enabled ? 1 : 0) &&
    sandbox_helper_append_u32(&buf, config->cpu_count) &&
    sandbox_helper_append_u64(&buf, config->memory_size) &&
    sandbox_helper_append_u32(&buf, config->timeout_ms) &&
    sandbox_helper_append_u32(&buf, config->boot_timeout_ms) &&
    sandbox_helper_append_u32(&buf, config->cpu_time_ms) &&
    sandbox_helper_append_u8(&buf, config->verbose ? 1 : 0) &&
    sandbox_helper_append_u32(&buf, (uint32_t)config->mount_count);
  for (size_t i = 0; ok && i < config->mount_count; i++) {
    ok = sandbox_helper_append_string(&buf, config->mounts[i].host_path) &&
      sandbox_helper_append_string(&buf, config->mounts[i].guest_path) &&
      sandbox_helper_append_string(&buf, config->mounts[i].tag) &&
      sandbox_helper_append_u8(&buf, config->mounts[i].readonly ? 1 : 0);
  }
  ok = ok && sandbox_helper_append_u32(&buf, (uint32_t)config->forward_count);
  for (size_t i = 0; ok && i < config->forward_count; i++) {
    ok = sandbox_helper_append_u16(&buf, config->forwards[i].host_port) &&
      sandbox_helper_append_u16(&buf, config->forwards[i].guest_port);
  }
  ok = ok &&
    sandbox_helper_append_u8(&buf, config->allow_host_all ? 1 : 0) &&
    sandbox_helper_append_u8(&buf, config->allow_lan ? 1 : 0) &&
    sandbox_helper_append_u32(&buf, (uint32_t)config->allow_host_port_count);
  for (size_t i = 0; ok && i < config->allow_host_port_count; i++)
    ok = sandbox_helper_append_u16(&buf, config->allow_host_ports[i]);
  if (!ok) { free(buf.data); return NULL; }
  *len_out = buf.len;
  return buf.data;
}

static bool sandbox_helper_read(sandbox_helper_reader_t *r, void *out, size_t len) {
  if (!r || !out || len > (size_t)(r->end - r->p)) return false;
  memcpy(out, r->p, len);
  r->p += len;
  return true;
}

static bool sandbox_helper_read_u8(sandbox_helper_reader_t *r, uint8_t *out) {
  return sandbox_helper_read(r, out, sizeof(*out));
}

static bool sandbox_helper_read_u16(sandbox_helper_reader_t *r, uint16_t *out) {
  uint8_t raw[2];
  if (!sandbox_helper_read(r, raw, sizeof(raw))) return false;
  *out = (uint16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
  return true;
}

static bool sandbox_helper_read_u32(sandbox_helper_reader_t *r, uint32_t *out) {
  uint8_t raw[4];
  if (!sandbox_helper_read(r, raw, sizeof(raw))) return false;
  *out = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) |
    ((uint32_t)raw[2] << 16) | ((uint32_t)raw[3] << 24);
  return true;
}

static bool sandbox_helper_read_u64(sandbox_helper_reader_t *r, uint64_t *out) {
  uint8_t raw[8];
  if (!sandbox_helper_read(r, raw, sizeof(raw))) return false;
  *out = 0;
  for (unsigned i = 0; i < 8; i++) *out |= (uint64_t)raw[i] << (i * 8);
  return true;
}

static char *sandbox_helper_read_string(sandbox_helper_reader_t *r) {
  uint32_t len = 0;
  if (!sandbox_helper_read_u32(r, &len) || len > (size_t)(r->end - r->p)) return NULL;
  char *value = malloc((size_t)len + 1);
  if (!value) return NULL;
  memcpy(value, r->p, len);
  value[len] = '\0';
  r->p += len;
  return value;
}

void sandbox_helper_owned_config_free(sandbox_helper_owned_config_t *owned) {
  if (!owned) return;
  for (size_t i = 0; i < owned->config.mount_count; i++) {
    free((char *)owned->mounts[i].host_path);
    free((char *)owned->mounts[i].guest_path);
    free((char *)owned->mounts[i].tag);
  }
  free(owned->mounts);
  free(owned->forwards);
  free(owned->allow_host_ports);
  free(owned->image_path);
  free(owned->kernel_path);
  memset(owned, 0, sizeof(*owned));
}

bool sandbox_helper_decode_config(
  const uint8_t *data, size_t len,
  sandbox_helper_owned_config_t *owned
) {
  if (!data || !owned) return false;
  sandbox_helper_reader_t r = { data, data + len };
  uint8_t network = 0, verbose = 0, allow_host_all = 0, allow_lan = 0;
  uint32_t mount_count = 0, forward_count = 0, allow_host_port_count = 0;
  uint64_t memory_size = 0;
  owned->image_path = sandbox_helper_read_string(&r);
  owned->kernel_path = sandbox_helper_read_string(&r);
  if (!owned->image_path || !owned->kernel_path ||
      !sandbox_helper_read_u32(&r, &owned->config.capabilities) ||
      !sandbox_helper_read_u8(&r, &network) ||
      !sandbox_helper_read_u32(&r, &owned->config.cpu_count) ||
      !sandbox_helper_read_u64(&r, &memory_size) ||
      !sandbox_helper_read_u32(&r, &owned->config.timeout_ms) ||
      !sandbox_helper_read_u32(&r, &owned->config.boot_timeout_ms) ||
      !sandbox_helper_read_u32(&r, &owned->config.cpu_time_ms) ||
      !sandbox_helper_read_u8(&r, &verbose) ||
      !sandbox_helper_read_u32(&r, &mount_count)) goto fail;
  if (mount_count > ANT_SANDBOX_MAX_MOUNTS) goto fail;
  if (mount_count > 0) {
    owned->mounts = calloc(mount_count, sizeof(*owned->mounts));
    if (!owned->mounts) goto fail;
  }
  owned->config.mount_count = mount_count;
  for (uint32_t i = 0; i < mount_count; i++) {
    uint8_t readonly = 0;
    owned->mounts[i].host_path = sandbox_helper_read_string(&r);
    owned->mounts[i].guest_path = sandbox_helper_read_string(&r);
    owned->mounts[i].tag = sandbox_helper_read_string(&r);
    if (!owned->mounts[i].host_path || !owned->mounts[i].guest_path ||
        !owned->mounts[i].tag || !sandbox_helper_read_u8(&r, &readonly)) goto fail;
    owned->mounts[i].readonly = readonly != 0;
  }
  if (!sandbox_helper_read_u32(&r, &forward_count) || forward_count > ANT_SANDBOX_MAX_FORWARDS) goto fail;
  if (forward_count > 0) {
    owned->forwards = calloc(forward_count, sizeof(*owned->forwards));
    if (!owned->forwards) goto fail;
  }
  owned->config.forward_count = forward_count;
  for (uint32_t i = 0; i < forward_count; i++) {
    if (!sandbox_helper_read_u16(&r, &owned->forwards[i].host_port) ||
        !sandbox_helper_read_u16(&r, &owned->forwards[i].guest_port)) goto fail;
  }
  if (!sandbox_helper_read_u8(&r, &allow_host_all) ||
      !sandbox_helper_read_u8(&r, &allow_lan) ||
      !sandbox_helper_read_u32(&r, &allow_host_port_count) ||
      allow_host_port_count > ANT_SANDBOX_MAX_FORWARDS) goto fail;
  if (allow_host_port_count > 0) {
    owned->allow_host_ports = calloc(allow_host_port_count, sizeof(*owned->allow_host_ports));
    if (!owned->allow_host_ports) goto fail;
  }
  for (uint32_t i = 0; i < allow_host_port_count; i++) {
    if (!sandbox_helper_read_u16(&r, &owned->allow_host_ports[i])) goto fail;
  }
  if (r.p != r.end) goto fail;
  owned->config.image_path = owned->image_path;
  owned->config.kernel_path = owned->kernel_path;
  owned->config.mounts = owned->mounts;
  owned->config.network_enabled = network != 0;
  owned->config.forwards = owned->forwards;
  owned->config.allow_host_ports = owned->allow_host_ports;
  owned->config.allow_host_port_count = allow_host_port_count;
  owned->config.allow_host_all = allow_host_all != 0;
  owned->config.allow_lan = allow_lan != 0;
  owned->config.memory_size = memory_size;
  owned->config.verbose = verbose != 0;
  return true;

fail:
  sandbox_helper_owned_config_free(owned);
  return false;
}
