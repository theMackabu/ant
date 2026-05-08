#include <arpa/inet.h>
#include <ctype.h>
#include <openssl/evp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <uv.h>

#include "base64.h"
#include "http/websocket.h"

const char *ant_ws_find_header(const ant_http_header_t *headers, const char *name) {
  for (const ant_http_header_t *entry = headers; entry; entry = entry->next) 
    if (entry->name && name && strcasecmp(entry->name, name) == 0) return entry->value;
  return NULL;
}

bool ant_ws_header_contains_token(const char *value, const char *token) {
  size_t token_len = token ? strlen(token) : 0;
  const char *p = value;

  if (!value || !token || token_len == 0) return false;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == ',') p++;
    const char *start = p;
    while (*p && *p != ',') p++;
    const char *end = p;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
    if ((size_t)(end - start) == token_len && strncasecmp(start, token, token_len) == 0) return true;
    if (*p == ',') p++;
  }
  
  return false;
}

bool ant_ws_validate_client_handshake(const ant_http_header_t *headers, const char **key_out) {
  const char *upgrade = ant_ws_find_header(headers, "upgrade");
  const char *connection = ant_ws_find_header(headers, "connection");
  const char *version = ant_ws_find_header(headers, "sec-websocket-version");
  const char *key = ant_ws_find_header(headers, "sec-websocket-key");
  
  size_t decoded_len = 0;
  uint8_t *decoded = NULL;

  if (key_out) *key_out = NULL;
  if (!upgrade || strcasecmp(upgrade, "websocket") != 0) return false;
  if (!ant_ws_header_contains_token(connection, "upgrade")) return false;
  if (!version || strcmp(version, "13") != 0) return false;
  if (!key || strlen(key) < 16) return false;

  decoded = ant_base64_decode(key, strlen(key), &decoded_len);
  if (!decoded || decoded_len != 16) {
    free(decoded);
    return false;
  }

  free(decoded);
  if (key_out) *key_out = key;
  
  return true;
}

char *ant_ws_accept_key(const char *client_key) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  
  size_t out_len = 0;
  size_t key_len = client_key ? strlen(client_key) : 0;
  size_t input_len = key_len + strlen(ANT_WS_GUID);
  
  char *input = malloc(input_len);
  char *encoded = NULL;

  if (!client_key || !input) {
    free(input);
    return NULL;
  }

  memcpy(input, client_key, key_len);
  memcpy(input + key_len, ANT_WS_GUID, strlen(ANT_WS_GUID));

  if (EVP_Digest(input, input_len, digest, &digest_len, EVP_sha1(), NULL) != 1) {
    free(input);
    return NULL;
  }

  free(input);
  encoded = ant_base64_encode(digest, digest_len, &out_len);
  if (encoded) encoded[out_len] = '\0';
  
  return encoded;
}

void ant_ws_frame_clear(ant_ws_frame_t *frame) {
  if (!frame) return;
  free(frame->payload);
  memset(frame, 0, sizeof(*frame));
}

static uint64_t ant_ws_read_u64_be(const uint8_t *data) {
  uint64_t value = 0;
  for (int i = 0; i < 8; i++) value = (value << 8) | data[i];
  return value;
}

ant_ws_frame_result_t ant_ws_parse_frame(
  const uint8_t *data,
  size_t len,
  bool require_mask,
  ant_ws_frame_t *out
) {
  size_t pos = 2;
  uint64_t payload_len = 0;
  bool masked = false;
  uint8_t opcode = 0;

  if (!out) return ANT_WS_FRAME_PROTOCOL_ERROR;
  memset(out, 0, sizeof(*out));
  if (!data || len < 2) return ANT_WS_FRAME_INCOMPLETE;

  opcode = data[0] & 0x0f;
  masked = (data[1] & 0x80u) != 0;
  payload_len = data[1] & 0x7fu;

  if ((data[0] & 0x70u) != 0) return ANT_WS_FRAME_PROTOCOL_ERROR;
  if (require_mask && !masked) return ANT_WS_FRAME_PROTOCOL_ERROR;
  if (opcode >= 0x8 && ((data[0] & 0x80u) == 0 || payload_len > 125)) return ANT_WS_FRAME_PROTOCOL_ERROR;

  if (payload_len == 126) {
    if (len < pos + 2) return ANT_WS_FRAME_INCOMPLETE;
    payload_len = ((uint64_t)data[pos] << 8) | data[pos + 1];
    pos += 2;
  } else if (payload_len == 127) {
    if (len < pos + 8) return ANT_WS_FRAME_INCOMPLETE;
    payload_len = ant_ws_read_u64_be(data + pos);
    if (payload_len & (UINT64_C(1) << 63)) return ANT_WS_FRAME_PROTOCOL_ERROR;
    pos += 8;
  }

  if (payload_len > SIZE_MAX) return ANT_WS_FRAME_PROTOCOL_ERROR;
  if (masked) {
    if (len < pos + 4) return ANT_WS_FRAME_INCOMPLETE;
    memcpy(out->mask, data + pos, 4);
    pos += 4;
  }
  if (len < pos + (size_t)payload_len) return ANT_WS_FRAME_INCOMPLETE;

  out->payload = malloc((size_t)payload_len + 1);
  if (!out->payload) return ANT_WS_FRAME_PROTOCOL_ERROR;
  if (payload_len > 0) memcpy(out->payload, data + pos, (size_t)payload_len);
  out->payload[payload_len] = 0;
  if (masked) {
    for (size_t i = 0; i < (size_t)payload_len; i++) out->payload[i] ^= out->mask[i % 4];
  }

  out->fin = (data[0] & 0x80u) != 0;
  out->masked = masked;
  out->opcode = (ant_ws_opcode_t)opcode;
  out->payload_len = (size_t)payload_len;
  out->consumed_len = pos + (size_t)payload_len;
  
  return ANT_WS_FRAME_OK;
}

static void ant_ws_write_u64_be(uint8_t *out, uint64_t value) {
for (int i = 7; i >= 0; i--) {
  out[i] = (uint8_t)(value & 0xffu);
  value >>= 8;
}}

uint8_t *ant_ws_encode_frame(
  ant_ws_opcode_t opcode,
  const uint8_t *payload,
  size_t payload_len,
  bool mask,
  size_t *out_len
) {
  size_t header_len = 2;
  size_t mask_len = mask ? 4 : 0;
  uint8_t masking_key[4] = {0};
  uint8_t *out = NULL;
  size_t pos = 0;

  if (out_len) *out_len = 0;
  if (payload_len > UINT16_MAX) header_len += 8;
  else if (payload_len >= 126) header_len += 2;

  out = malloc(header_len + mask_len + payload_len);
  if (!out) return NULL;

  out[pos++] = 0x80u | (uint8_t)opcode;
  if (payload_len < 126) out[pos++] = (mask ? 0x80u : 0) | (uint8_t)payload_len;
  else if (payload_len <= UINT16_MAX) {
    out[pos++] = (mask ? 0x80u : 0) | 126u;
    out[pos++] = (uint8_t)(payload_len >> 8);
    out[pos++] = (uint8_t)(payload_len & 0xffu);
  } else {
    out[pos++] = (mask ? 0x80u : 0) | 127u;
    ant_ws_write_u64_be(out + pos, (uint64_t)payload_len);
    pos += 8;
  }

  if (mask) {
    if (uv_random(NULL, NULL, masking_key, sizeof(masking_key), 0, NULL) != 0) {
      free(out);
      return NULL;
    }
    memcpy(out + pos, masking_key, sizeof(masking_key));
    pos += sizeof(masking_key);
  }

  for (size_t i = 0; i < payload_len; i++) {
    uint8_t byte = payload ? payload[i] : 0;
    out[pos + i] = mask ? (byte ^ masking_key[i % 4]) : byte;
  }

  if (out_len) *out_len = pos + payload_len;
  return out;
}

uint8_t *ant_ws_encode_close_frame(uint16_t code, const char *reason, bool mask, size_t *out_len) {
  size_t reason_len = reason ? strlen(reason) : 0;
  if (reason_len > 123) reason_len = 123;

  uint8_t payload[125];
  payload[0] = (uint8_t)(code >> 8);
  payload[1] = (uint8_t)(code & 0xffu);
  
  if (reason_len > 0) memcpy(payload + 2, reason, reason_len);
  return ant_ws_encode_frame(ANT_WS_OPCODE_CLOSE, payload, 2 + reason_len, mask, out_len);
}
