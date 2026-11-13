#include "download.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

enum { PAYLOAD_LEN = 300000 };

static unsigned char payload[PAYLOAD_LEN];
static unsigned char *gzipped;
static size_t gzipped_len;
static int listen_fd;
static int port;
static char dir[256];

static void write_all(int fd, const void *data, size_t len) {
  const unsigned char *p = data;
  while (len > 0) {
    ssize_t n = write(fd, p, len);
    if (n <= 0) return;
    p += n;
    len -= (size_t)n;
  }
}

static void respond(int fd, const char *status, const char *headers, const void *body, size_t len) {
  char head[512];
  int n = snprintf(
    head, sizeof(head), "HTTP/1.1 %s\r\nConnection: close\r\nContent-Length: %zu\r\n%s\r\n",
    status, len, headers
  );
  write_all(fd, head, (size_t)n);
  write_all(fd, body, len);
}

static void serve(int fd) {
  char req[2048] = {0};
  size_t got = 0;
  while (got < sizeof(req) - 1 && !strstr(req, "\r\n\r\n")) {
    ssize_t n = read(fd, req + got, sizeof(req) - 1 - got);
    if (n <= 0) return;
    got += (size_t)n;
    req[got] = '\0';
  }

  char path[128] = {0};
  sscanf(req, "GET %127s", path);

  char sized[64];
  snprintf(sized, sizeof(sized), "x-ant-uncompressed-size: %d\r\n", PAYLOAD_LEN);

  if (strcmp(path, "/plain") == 0) respond(fd, "200 OK", "", payload, PAYLOAD_LEN);
  else if (strcmp(path, "/hop") == 0) respond(fd, "302 Found", "Location: /plain\r\n", "", 0);
  else if (strcmp(path, "/loop") == 0) respond(fd, "302 Found", "Location: /loop\r\n", "", 0);
  else if (strcmp(path, "/gz") == 0) respond(fd, "200 OK", sized, gzipped, gzipped_len);
  else if (strcmp(path, "/gz-hop") == 0) respond(fd, "302 Found", "Location: /gz\r\n", "", 0);
  else if (strcmp(path, "/gz-short") == 0) respond(fd, "200 OK", sized, gzipped, gzipped_len / 2);
  else respond(fd, "404 Not Found", "", "", 0);
}

static void *server_main(void *arg) {
  (void)arg;
  for (;;) {
    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) return NULL;
    serve(fd);
    shutdown(fd, SHUT_WR);
    char drain[256];
    while (read(fd, drain, sizeof(drain)) > 0) {}
    close(fd);
  }
}

static void start_server(void) {
  listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(listen_fd >= 0);
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  assert(bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  assert(listen(listen_fd, 16) == 0);
  socklen_t len = sizeof(addr);
  assert(getsockname(listen_fd, (struct sockaddr *)&addr, &len) == 0);
  port = ntohs(addr.sin_port);

  pthread_t thread;
  assert(pthread_create(&thread, NULL, server_main, NULL) == 0);
  pthread_detach(thread);
}

static void make_payload(void) {
  uint32_t state = 12345;
  for (size_t i = 0; i < PAYLOAD_LEN; i++) {
    state = state * 1103515245u + 12345u;
    payload[i] = (unsigned char)("abcdefgh"[(state >> 16) & 7]);
  }

  z_stream z = {0};
  assert(deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK);
  uLong bound = deflateBound(&z, PAYLOAD_LEN);
  gzipped = malloc(bound);
  assert(gzipped);
  z.next_in = payload;
  z.avail_in = PAYLOAD_LEN;
  z.next_out = gzipped;
  z.avail_out = (uInt)bound;
  assert(deflate(&z, Z_FINISH) == Z_STREAM_END);
  gzipped_len = bound - z.avail_out;
  deflateEnd(&z);
}

static const char *url(const char *path) {
  static char bufs[4][128];
  static int next;
  char *out = bufs[next++ & 3];
  snprintf(out, sizeof(bufs[0]), "http://127.0.0.1:%d%s", port, path);
  return out;
}

static const char *file(const char *name) {
  static char bufs[4][320];
  static int next;
  char *out = bufs[next++ & 3];
  snprintf(out, sizeof(bufs[0]), "%s/%s", dir, name);
  return out;
}

static bool exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

static bool holds_payload(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  unsigned char *data = malloc(PAYLOAD_LEN + 1);
  size_t n = fread(data, 1, PAYLOAD_LEN + 1, f);
  fclose(f);
  bool same = n == PAYLOAD_LEN && memcmp(data, payload, PAYLOAD_LEN) == 0;
  free(data);
  return same;
}

static void test_get_body(const char *path) {
  char err[256] = {0};
  char *body = NULL;
  size_t len = 0;
  int rc = ant_download_get(url(path), NULL, NULL, NULL, &body, &len, err, sizeof(err));
  if (rc != 0) fprintf(stderr, "download failed: %s\n", err);
  assert(rc == 0);
  assert(len == PAYLOAD_LEN && memcmp(body, payload, PAYLOAD_LEN) == 0);
  assert(body[len] == '\0');
  free(body);
}

static void test_get_fails(const char *path, const char *expected) {
  char err[256] = {0};
  char *body = NULL;
  size_t len = 0;
  assert(ant_download_get(url(path), NULL, NULL, NULL, &body, &len, err, sizeof(err)) != 0);
  assert(body == NULL && len == 0);
  if (!strstr(err, expected)) fprintf(stderr, "unexpected error: %s\n", err);
  assert(strstr(err, expected));
}

static void test_files_in_parallel(void) {
  const char *plain = file("plain.bin");
  const char *gz = file("gz.bin");
  ant_download_file_t files[] = {
    {.label = "plain", .path = plain, .url = url("/hop"), .expected_size = PAYLOAD_LEN},
    {
      .label = "gzip", .path = gz, .url = url("/gz-hop"), .gzip = true,
      .expected_size = PAYLOAD_LEN, .expected_gzip_size = gzipped_len,
    },
  };

  char err[256] = {0};
  int rc = ant_download_files(files, 2, err, sizeof(err));
  if (rc != 0) fprintf(stderr, "download failed: %s\n", err);
  assert(rc == 0);
  assert(holds_payload(plain) && holds_payload(gz));
  assert(!exists(file("plain.bin.tmp")) && !exists(file("gz.bin.tmp")));
}

static void test_existing_file_is_kept(void) {
  const char *kept = file("kept.bin");
  FILE *f = fopen(kept, "wb");
  assert(f && fputs("mine", f) >= 0 && fclose(f) == 0);

  ant_download_file_t files[] = {{.label = "kept", .path = kept, .url = url("/plain")}};
  char err[256] = {0};
  assert(ant_download_files(files, 1, err, sizeof(err)) == 0);

  struct stat st;
  assert(stat(kept, &st) == 0 && st.st_size == 4);
}

static void test_failure_installs_nothing(const char *name, const char *path, uint64_t gzip_size, const char *expected) {
  char good[64], bad[64];
  snprintf(good, sizeof(good), "%s-good.bin", name);
  snprintf(bad, sizeof(bad), "%s-bad.bin", name);
  const char *good_path = file(good);
  const char *bad_path = file(bad);

  ant_download_file_t files[] = {
    {.label = "good", .path = good_path, .url = url("/plain")},
    {
      .label = "bad", .path = bad_path, .url = url(path), .gzip = true,
      .expected_size = PAYLOAD_LEN, .expected_gzip_size = gzip_size,
    },
  };

  char err[256] = {0};
  assert(ant_download_files(files, 2, err, sizeof(err)) != 0);
  if (!strstr(err, expected)) fprintf(stderr, "unexpected error: %s\n", err);
  assert(strstr(err, expected));
  assert(!exists(good_path) && !exists(bad_path));

  char tmp[80];
  snprintf(tmp, sizeof(tmp), "%s.tmp", good);
  assert(!exists(file(tmp)));
  snprintf(tmp, sizeof(tmp), "%s.tmp", bad);
  assert(!exists(file(tmp)));
}

int main(void) {
  make_payload();
  start_server();

  const char *tmp = getenv("TMPDIR");
  snprintf(dir, sizeof(dir), "%s/ant-download-test-XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
  assert(mkdtemp(dir));

  test_get_body("/plain");
  test_get_body("/hop");
  test_get_fails("/missing", "HTTP 404");
  test_get_fails("/loop", "too many redirects");

  test_files_in_parallel();
  test_existing_file_is_kept();
  test_failure_installs_nothing("size", "/gz", gzipped_len + 1, "gzip download size mismatch");
  test_failure_installs_nothing("short", "/gz-short", 0, "gzip");
  test_failure_installs_nothing("missing", "/missing", 0, "HTTP 404");

  assert(ant_remove_tree(dir) == 0);
  printf("download: ok\n");
  return 0;
}
