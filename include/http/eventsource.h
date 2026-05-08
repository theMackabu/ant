#ifndef ANT_HTTP_EVENTSOURCE_H
#define ANT_HTTP_EVENTSOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  char *data;
  char *event;
  char *id;
  uint32_t retry;
  bool has_retry;
} ant_sse_message_t;

typedef struct {
  char *line;
  size_t line_len;
  size_t line_cap;
  char *data;
  char *event;
  char *id;
  char *last_event_id;
  uint32_t retry;
  bool has_retry;
} ant_sse_parser_t;

typedef bool (*ant_sse_message_cb)(
  const ant_sse_message_t *message, 
  void *user_data
);

void ant_sse_message_clear(ant_sse_message_t *message);
void ant_sse_parser_init(ant_sse_parser_t *parser);
void ant_sse_parser_free(ant_sse_parser_t *parser);

char *ant_sse_format_comment(
  const char *comment,
  size_t *out_len
);

char *ant_sse_format_event(
  const char *data,
  const char *event,
  const char *id,
  const char *retry,
  size_t *out_len
);

bool ant_sse_parser_feed(
  ant_sse_parser_t *parser,
  const char *chunk, size_t len,
  ant_sse_message_cb cb,
  void *user_data
);

#endif
