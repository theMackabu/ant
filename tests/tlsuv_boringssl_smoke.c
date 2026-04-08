#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tlsuv/http.h>
#include <tlsuv/tls_engine.h>
#include <tlsuv/tlsuv.h>
#include <uv.h>

typedef struct smoke_http_capture_s {
  int status;
  int finished;
  int eof_seen;
  char *body;
  size_t body_len;
  size_t body_cap;
} smoke_http_capture_t;

static void smoke_fail(const char *message) {
  fprintf(stderr, "FAIL: %s\n", message);
  exit(1);
}

static void smoke_check(int ok, const char *message) {
  if (!ok) smoke_fail(message);
  printf("ok: %s\n", message);
}

static void smoke_check_status(int actual, int expected, const char *body) {
  if (actual == expected) {
    printf("ok: HTTPS request returned %d\n", expected);
    return;
  }

  fprintf(stderr, "FAIL: HTTPS request returned %d (expected %d)\n", actual, expected);
  if (body && *body) {
    fprintf(stderr, "response body sample: %.200s\n", body);
  }
  exit(1);
}

static void smoke_http_close_cb(tlsuv_http_t *client) {
  (void)client;
}

static void smoke_http_body_cb(tlsuv_http_req_t *req, char *chunk, ssize_t len) {
  smoke_http_capture_t *cap = (smoke_http_capture_t *)req->data;

  if (len > 0) {
    size_t needed = cap->body_len + (size_t)len + 1;
    if (needed > cap->body_cap) {
      size_t next_cap = cap->body_cap ? cap->body_cap * 2 : 256;
      while (next_cap < needed) next_cap *= 2;
      char *next = (char *)realloc(cap->body, next_cap);
      if (!next) smoke_fail("out of memory growing HTTP body buffer");
      cap->body = next;
      cap->body_cap = next_cap;
    }

    memcpy(cap->body + cap->body_len, chunk, (size_t)len);
    cap->body_len += (size_t)len;
    cap->body[cap->body_len] = '\0';
    return;
  }

  if (len == UV_EOF) {
    cap->eof_seen = 1;
    cap->finished = 1;
    tlsuv_http_close(req->client, smoke_http_close_cb);
  }
}

static void smoke_http_resp_cb(tlsuv_http_resp_t *resp, void *ctx) {
  smoke_http_capture_t *cap = (smoke_http_capture_t *)ctx;
  cap->status = resp->code;
  resp->req->data = cap;
  resp->body_cb = smoke_http_body_cb;

  if (resp->code < 0) {
    cap->finished = 1;
    tlsuv_http_close(resp->req->client, smoke_http_close_cb);
  }
}

static void smoke_usage(const char *argv0) {
  fprintf(stderr, "usage: %s [--url https://localhost:8443] [--ca path] [--cert path] [--key path]\n", argv0);
}

int main(int argc, char **argv) {
  const char *ca_path = "vendor/tlsuv/tests/certs/ca.pem";
  const char *cert_path = "vendor/tlsuv/tests/certs/server.crt";
  const char *key_path = "vendor/tlsuv/tests/certs/server.key";
  const char *url = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--url") == 0 && i + 1 < argc) {
      url = argv[++i];
    } else if (strcmp(argv[i], "--ca") == 0 && i + 1 < argc) {
      ca_path = argv[++i];
    } else if (strcmp(argv[i], "--cert") == 0 && i + 1 < argc) {
      cert_path = argv[++i];
    } else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
      key_path = argv[++i];
    } else if (strcmp(argv[i], "--help") == 0) {
      smoke_usage(argv[0]);
      return 0;
    } else {
      smoke_usage(argv[0]);
      return 1;
    }
  }

  tls_context *base = default_tls_context(NULL, 0);
  smoke_check(base != NULL, "created default TLS context");

  const char *version = base->version ? base->version() : NULL;
  smoke_check(version != NULL && strstr(version, "BoringSSL") != NULL, "tlsuv reports BoringSSL");
  printf("tls version: %s\n", version);

  smoke_check(base->generate_key != NULL, "generate_key is available");
  smoke_check(base->load_key != NULL, "load_key is available");
  smoke_check(base->load_cert != NULL, "load_cert is available");
  smoke_check(base->set_own_cert != NULL, "set_own_cert is available");
  smoke_check(base->generate_csr_to_pem != NULL, "generate_csr_to_pem is available");

  tlsuv_private_key_t generated = NULL;
  smoke_check(base->generate_key(&generated) == 0 && generated != NULL, "generated private key");

  char *generated_pem = NULL;
  size_t generated_pem_len = 0;
  smoke_check(generated->to_pem(generated, &generated_pem, &generated_pem_len) == 0, "generated key exports to PEM");
  smoke_check(generated_pem != NULL && generated_pem_len > 0, "generated key PEM is non-empty");

  tlsuv_private_key_t roundtrip = NULL;
  smoke_check(base->load_key(&roundtrip, generated_pem, generated_pem_len) == 0 && roundtrip != NULL, "generated key PEM reloads");

  char *csr_pem = NULL;
  size_t csr_pem_len = 0;
  smoke_check(
    base->generate_csr_to_pem(
      generated, &csr_pem, &csr_pem_len,
      "CN", "boringssl-smoke",
      "O", "ant",
      NULL
    ) == 0,
    "generated key can produce CSR"
  );
  smoke_check(csr_pem != NULL && csr_pem_len > 0, "CSR PEM is non-empty");

  tlsuv_private_key_t file_key = NULL;
  tlsuv_certificate_t file_cert = NULL;
  smoke_check(base->load_key(&file_key, key_path, strlen(key_path)) == 0 && file_key != NULL, "fixture private key loads");
  smoke_check(base->load_cert(&file_cert, cert_path, strlen(cert_path)) == 0 && file_cert != NULL, "fixture certificate loads");
  smoke_check(base->set_own_cert(base, file_key, file_cert) == 0, "fixture key/cert install on context");

  struct tm expiration = {0};
  smoke_check(file_cert->get_expiration != NULL, "certificate expiration accessor is available");
  smoke_check(file_cert->get_expiration(file_cert, &expiration) == 0, "certificate expiration decodes");

  if (url != NULL) {
    uv_loop_t *loop = uv_loop_new();
    smoke_check(loop != NULL, "created libuv loop");

    tls_context *http_tls = default_tls_context(ca_path, strlen(ca_path));
    smoke_check(http_tls != NULL, "created CA-backed TLS context");

    tlsuv_http_t client;
    memset(&client, 0, sizeof(client));
    smoke_http_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    smoke_check(tlsuv_http_init(loop, &client, url) == 0, "initialized tlsuv HTTP client");
    tlsuv_http_set_ssl(&client, http_tls);

    tlsuv_http_req_t *req = tlsuv_http_req(&client, "GET", "/get", smoke_http_resp_cb, &cap);
    smoke_check(req != NULL, "created HTTPS request");
    tlsuv_http_req_end(req);

    uv_run(loop, UV_RUN_DEFAULT);

    smoke_check(cap.finished, "HTTPS request finished");
    smoke_check_status(cap.status, 200, cap.body);
    smoke_check(cap.eof_seen, "HTTPS body reached EOF");
    smoke_check(cap.body != NULL && cap.body_len > 0, "HTTPS body is non-empty");
    printf("https body sample: %.80s\n", cap.body);

    http_tls->free_ctx(http_tls);
    if (uv_loop_close(loop) != 0) smoke_fail("failed to close libuv loop");
    free(loop);
    free(cap.body);
  } else {
    printf("skipped live HTTPS probe; rerun with --url https://localhost:8443 to exercise a real handshake\n");
    printf("tip: start vendor/tlsuv/tests/test_server/test-server.go with the existing cert fixtures first\n");
  }

  free(csr_pem);
  free(generated_pem);

  if (base && base->free_ctx) base->free_ctx(base);
  if (file_cert && file_cert->free) file_cert->free(file_cert);
  if (file_key && file_key->free) file_key->free(file_key);
  if (roundtrip && roundtrip->free) roundtrip->free(roundtrip);
  if (generated && generated->free) generated->free(generated);

  printf("BoringSSL smoke test passed\n");
  return 0;
}
