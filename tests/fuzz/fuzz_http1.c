// HTTP/1 request parser (src/http/http1_parser.c).
//
// Input: one split-seed byte, then raw request bytes (possibly pipelined).
// Property: a request stream parses the same however it arrives. The one-shot
// ant_http1_parse_request is the oracle for
//   - ant_http1_conn_parser_execute, fed growing prefixes of one buffer (the
//     Ant.serve path), and
//   - ant_http1_conn_parser_feed, fed disjoint chunks with the body streamed
//     back per chunk (the node:http parser path).
// Each path parses messages until one is incomplete or fails, resetting
// between messages like their callers do.
#include "fuzz.h"
#include "http/http1_parser.h"

#define MAX_MESSAGES 8

typedef struct {
  ant_http1_parse_result_t result;
  ant_http1_parsed_request_t req;
  uint8_t *body;
  size_t body_len;
  size_t consumed;
} message_t;

typedef struct {
  message_t msgs[MAX_MESSAGES];
  int count;
} stream_t;

static bool same_opt_str(const char *a, const char *b) {
  if (!a || !b) return a == b;
  return strcmp(a, b) == 0;
}

static void check_same_message(const message_t *a, const message_t *b) {
  FUZZ_CHECK(a->result == b->result);
  if (a->result != ANT_HTTP1_PARSE_OK) return;

  const ant_http1_parsed_request_t *x = &a->req, *y = &b->req;
  FUZZ_CHECK(strcmp(x->method, y->method) == 0);
  FUZZ_CHECK(strcmp(x->target, y->target) == 0);
  FUZZ_CHECK(same_opt_str(x->host, y->host));
  FUZZ_CHECK(same_opt_str(x->content_type, y->content_type));
  FUZZ_CHECK(x->content_length == y->content_length);
  FUZZ_CHECK(x->http_major == y->http_major && x->http_minor == y->http_minor);
  FUZZ_CHECK(x->canonical_host == y->canonical_host);
  FUZZ_CHECK(x->absolute_target == y->absolute_target);
  FUZZ_CHECK(x->keep_alive == y->keep_alive);

  const ant_http_header_t *h = x->headers, *k = y->headers;
  for (; h && k; h = h->next, k = k->next) {
    FUZZ_CHECK(strcmp(h->name, k->name) == 0);
    FUZZ_CHECK(strcmp(h->value, k->value) == 0);
  }
  FUZZ_CHECK(!h && !k);

  FUZZ_CHECK(a->body_len == b->body_len);
  FUZZ_CHECK(a->body_len == 0 || memcmp(a->body, b->body, a->body_len) == 0);
  FUZZ_CHECK(a->consumed == b->consumed);
}

static void stream_free(stream_t *s) {
  for (int i = 0; i < s->count; i++) {
    ant_http1_free_parsed_request(&s->msgs[i].req);
    free(s->msgs[i].body);
  }
}

static void parse_one_shot(const char *data, size_t len, stream_t *out) {
  size_t off = 0;
  while (out->count < MAX_MESSAGES) {
    message_t *m = &out->msgs[out->count++];
    m->result = ant_http1_parse_request(data + off, len - off, 0, &m->req, NULL, NULL);
    if (m->result != ANT_HTTP1_PARSE_OK) return;

    m->consumed = m->req.consumed_len;
    m->body = m->req.body;
    m->body_len = m->req.body_len;
    m->req.body = NULL;
    m->req.body_len = 0;

    FUZZ_CHECK(m->consumed > 0 && m->consumed <= len - off);
    off += m->consumed;
    if (off == len) return;
  }
}

static void parse_execute(const char *data, size_t len, uint8_t seed, stream_t *out) {
  fuzz_splitter_t split = fuzz_splitter(seed);
  ant_http1_conn_parser_t cp;
  ant_http1_conn_parser_init(&cp);

  size_t off = 0;
  while (out->count < MAX_MESSAGES) {
    message_t *m = &out->msgs[out->count++];
    size_t avail = 0;
    m->result = ANT_HTTP1_PARSE_INCOMPLETE;

    while (avail < len - off) {
      avail += fuzz_next_chunk(&split, len - off - avail);
      ant_http1_parsed_request_t req;
      size_t consumed = 0;
      m->result = ant_http1_conn_parser_execute(&cp, data + off, avail, &req, &consumed);
      if (m->result == ANT_HTTP1_PARSE_OK) {
        m->req = req;
        m->consumed = consumed;
        m->body = req.body;
        m->body_len = req.body_len;
        m->req.body = NULL;
        m->req.body_len = 0;
        break;
      }
      ant_http1_free_parsed_request(&req);
      if (m->result == ANT_HTTP1_PARSE_ERROR) break;
    }

    if (m->result != ANT_HTTP1_PARSE_OK) break;
    FUZZ_CHECK(m->consumed > 0 && m->consumed <= avail);
    off += m->consumed;
    ant_http1_conn_parser_reset(&cp);
    if (off == len) break;
  }

  ant_http1_conn_parser_free(&cp);
}

static void body_append(message_t *m, const void *data, size_t len) {
  if (len == 0) return;
  m->body = realloc(m->body, m->body_len + len);
  if (!m->body) abort();
  memcpy(m->body + m->body_len, data, len);
  m->body_len += len;
}

static void parse_feed(const char *data, size_t len, uint8_t seed, stream_t *out) {
  fuzz_splitter_t split = fuzz_splitter(seed);
  ant_http1_conn_parser_t cp;
  ant_http1_conn_parser_init(&cp);

  size_t off = 0;
  while (out->count < MAX_MESSAGES) {
    message_t *m = &out->msgs[out->count++];
    size_t pos = off;
    m->result = ANT_HTTP1_PARSE_INCOMPLETE;

    while (pos < len) {
      size_t n = fuzz_next_chunk(&split, len - pos);
      // each chunk lives in its own allocation, as socket reads would
      char *chunk = fuzz_dup(data + pos, n);
      ant_http1_parsed_request_t head;
      size_t consumed = 0;
      m->result = ant_http1_conn_parser_feed(&cp, chunk, n, &head, &consumed);

      if (m->result != ANT_HTTP1_PARSE_ERROR) {
        if (head.method) {
          FUZZ_CHECK(!m->req.method);
          m->req = head;
        } else ant_http1_free_parsed_request(&head);
        if (cp.ctx.body_span) {
          FUZZ_CHECK(cp.ctx.body_span >= chunk && cp.ctx.body_span + cp.ctx.body_span_len <= chunk + n);
          body_append(m, cp.ctx.body_span, cp.ctx.body_span_len);
        } else body_append(m, cp.ctx.body.data, cp.ctx.body.len);
      } else ant_http1_free_parsed_request(&head);

      free(chunk);
      if (m->result == ANT_HTTP1_PARSE_ERROR) break;
      FUZZ_CHECK(consumed <= n);
      if (m->result == ANT_HTTP1_PARSE_OK) {
        m->consumed = pos - off + consumed;
        break;
      }
      FUZZ_CHECK(consumed == n);
      pos += n;
    }

    if (m->result != ANT_HTTP1_PARSE_OK) break;
    FUZZ_CHECK(m->req.method);
    off += m->consumed;
    if (off == len) break;
  }

  ant_http1_conn_parser_free(&cp);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  fuzz_input_t in = { data, size };
  uint8_t seed = fuzz_u8(&in);
  const char *req = (const char *)in.data;
  size_t len = in.size;

  stream_t oracle = {0}, executed = {0}, fed = {0};
  parse_one_shot(req, len, &oracle);
  parse_execute(req, len, seed, &executed);
  parse_feed(req, len, seed, &fed);

  FUZZ_CHECK(oracle.count == executed.count);
  FUZZ_CHECK(oracle.count == fed.count);
  for (int i = 0; i < oracle.count; i++) {
    check_same_message(&oracle.msgs[i], &executed.msgs[i]);
    check_same_message(&oracle.msgs[i], &fed.msgs[i]);
  }

  stream_free(&oracle);
  stream_free(&executed);
  stream_free(&fed);
  return 0;
}
