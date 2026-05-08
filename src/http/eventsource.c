#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http/eventsource.h"

void ant_sse_message_clear(ant_sse_message_t *message) {
  if (!message) return;
  free(message->data);
  free(message->event);
  free(message->id);
  memset(message, 0, sizeof(*message));
}

void ant_sse_parser_init(ant_sse_parser_t *parser) {
  if (parser) memset(parser, 0, sizeof(*parser));
}

static void parser_reset_event(ant_sse_parser_t *parser) {
  if (!parser) return;
  free(parser->data);
  free(parser->event);
  free(parser->id);
  parser->data = NULL;
  parser->event = NULL;
  parser->id = NULL;
}

void ant_sse_parser_free(ant_sse_parser_t *parser) {
  if (!parser) return;
  free(parser->line);
  free(parser->last_event_id);
  parser_reset_event(parser);
  memset(parser, 0, sizeof(*parser));
}

static bool append_bytes(char **buf, size_t *len, size_t *cap, const char *data, size_t data_len) {
  if (*len + data_len + 1 > *cap) {
    size_t next = *cap ? *cap * 2 : 64;
    while (*len + data_len + 1 > next) next *= 2;
    char *grown = realloc(*buf, next);
    if (!grown) return false;
    *buf = grown;
    *cap = next;
  }
  
  if (data_len > 0) memcpy(*buf + *len, data, data_len);
  *len += data_len;
  (*buf)[*len] = '\0';
  
  return true;
}

static bool set_string(char **slot, const char *value, size_t len) {
  char *copy = malloc(len + 1);
  if (!copy) return false;
  if (len > 0) memcpy(copy, value, len);
  
  copy[len] = '\0';
  free(*slot);
  *slot = copy;
  
  return true;
}

static bool append_data_field(ant_sse_parser_t *parser, const char *value, size_t len) {
  size_t cur_len = parser->data ? strlen(parser->data) : 0;
  size_t cap = cur_len + len + 2;
  char *next = realloc(parser->data, cap);
  
  if (!next) return false;
  parser->data = next;
  
  if (len > 0) memcpy(parser->data + cur_len, value, len);
  parser->data[cur_len + len] = '\n';
  parser->data[cur_len + len + 1] = '\0';
  
  return true;
}

static bool parse_retry(const char *value, size_t len, uint32_t *out) {
  uint32_t retry = 0;
  if (!value || len == 0) return false;
  
  for (size_t i = 0; i < len; i++) {
    if (value[i] < '0' || value[i] > '9') return false;
    uint32_t digit = (uint32_t)(value[i] - '0');
    if (retry > (UINT32_MAX - digit) / 10) return false;
    retry = retry * 10 + digit;
  }
  
  *out = retry;
  return true;
}

static bool dispatch_event(ant_sse_parser_t *parser, ant_sse_message_cb cb, void *user_data) {
  if (!parser->data) {
    parser_reset_event(parser);
    return true;
  }

  size_t data_len = strlen(parser->data);
  if (data_len > 0 && parser->data[data_len - 1] == '\n') parser->data[data_len - 1] = '\0';

  ant_sse_message_t message = {
    .data = parser->data,
    .event = parser->event,
    .id = parser->id,
    .retry = parser->retry,
    .has_retry = parser->has_retry,
  };

  if (message.id && !set_string(&parser->last_event_id, message.id, strlen(message.id))) return false;
  bool ok = cb ? cb(&message, user_data) : true;
  
  parser->data = NULL;
  parser->event = NULL;
  parser->id = NULL;
  ant_sse_message_clear(&message);
  
  return ok;
}

static bool parser_process_line(ant_sse_parser_t *parser, const char *line, size_t len, ant_sse_message_cb cb, void *user_data) {
  if (len > 0 && line[len - 1] == '\r') len--;
  if (len == 0) return dispatch_event(parser, cb, user_data);
  if (line[0] == ':') return true;

  const char *colon = memchr(line, ':', len);
  size_t field_len = colon ? (size_t)(colon - line) : len;
  
  const char *value = colon ? colon + 1 : "";
  size_t value_len = colon ? len - field_len - 1 : 0;
  
  if (value_len > 0 && value[0] == ' ') {
    value++;
    value_len--;
  }

  if (field_len == 4 && memcmp(line, "data", 4) == 0)
    return append_data_field(parser, value, value_len);
  if (field_len == 5 && memcmp(line, "event", 5) == 0)
    return set_string(&parser->event, value, value_len);
  if (field_len == 2 && memcmp(line, "id", 2) == 0) {
    if (memchr(value, '\0', value_len)) return true;
    return set_string(&parser->id, value, value_len);
  }
  
  if (field_len == 5 && memcmp(line, "retry", 5) == 0) {
  uint32_t retry = 0;
  if (parse_retry(value, value_len, &retry)) {
    parser->retry = retry;
    parser->has_retry = true;
  }}
  
  return true;
}

bool ant_sse_parser_feed(
  ant_sse_parser_t *parser,
  const char *chunk,
  size_t len,
  ant_sse_message_cb cb,
  void *user_data
) {
  if (!parser || (!chunk && len > 0)) return false;
  for (size_t i = 0; i < len; i++) {
    char ch = chunk[i];
    if (ch == '\n') {
      if (!parser_process_line(parser, parser->line ? parser->line : "", parser->line_len, cb, user_data)) return false;
      parser->line_len = 0;
      if (parser->line) parser->line[0] = '\0';
      continue;
    }
    
    if (parser->line_len + 2 > parser->line_cap) {
      size_t next_cap = parser->line_cap ? parser->line_cap * 2 : 128;
      char *next = realloc(parser->line, next_cap);
      if (!next) return false;
      parser->line = next;
      parser->line_cap = next_cap;
    }
    
    parser->line[parser->line_len++] = ch;
    parser->line[parser->line_len] = '\0';
  }
  
  return true;
}

static bool append_field_lines(char **buf, size_t *len, size_t *cap, const char *field, const char *value) {
  const char *start = value ? value : "";
  const char *p = start;

  do {
    const char *line_end = strchr(p, '\n');
    size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);
    if (line_len > 0 && p[line_len - 1] == '\r') line_len--;
    if (!append_bytes(buf, len, cap, field, strlen(field))) return false;
    if (!append_bytes(buf, len, cap, ": ", 2)) return false;
    if (!append_bytes(buf, len, cap, p, line_len)) return false;
    if (!append_bytes(buf, len, cap, "\n", 1)) return false;
    if (!line_end) break;
    p = line_end + 1;
  } while (*p);

  return true;
}

char *ant_sse_format_event(
  const char *data,
  const char *event,
  const char *id,
  const char *retry,
  size_t *out_len
) {
  char *buf = NULL;
  size_t len = 0;
  size_t cap = 0;

  if (out_len) *out_len = 0;
  if (event && *event && !append_field_lines(&buf, &len, &cap, "event", event)) goto oom;
  if (id && !append_field_lines(&buf, &len, &cap, "id", id)) goto oom;
  if (retry && *retry && !append_field_lines(&buf, &len, &cap, "retry", retry)) goto oom;
  if (!append_field_lines(&buf, &len, &cap, "data", data ? data : "")) goto oom;
  if (!append_bytes(&buf, &len, &cap, "\n", 1)) goto oom;

  if (out_len) *out_len = len;
  return buf;

oom:
  free(buf);
  return NULL;
}

char *ant_sse_format_comment(const char *comment, size_t *out_len) {
  char *buf = NULL;
  size_t len = 0;
  size_t cap = 0;
  const char *value = comment ? comment : "";
  const char *p = value;

  if (out_len) *out_len = 0;
  do {
    const char *line_end = strchr(p, '\n');
    size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);
    if (line_len > 0 && p[line_len - 1] == '\r') line_len--;
    if (!append_bytes(&buf, &len, &cap, ": ", 2)) goto oom;
    if (!append_bytes(&buf, &len, &cap, p, line_len)) goto oom;
    if (!append_bytes(&buf, &len, &cap, "\n", 1)) goto oom;
    if (!line_end) break;
    p = line_end + 1;
  } while (*p);
  if (!append_bytes(&buf, &len, &cap, "\n", 1)) goto oom;

  if (out_len) *out_len = len;
  return buf;

oom:
  free(buf);
  return NULL;
}
