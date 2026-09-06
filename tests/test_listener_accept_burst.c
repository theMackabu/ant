/* Unix accept-loop regression. Build against Ant's configured libuv:
 * meson compile -C build test-listener-accept-burst
 * ./build/test-listener-accept-burst
 */
#include <assert.h>
#include <stdio.h>
#include <uv.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>

#define CONNECTIONS 32
static uv_loop_t loop;
static uv_tcp_t listener;
static uv_tcp_t peers[CONNECTIONS];
static int accepted;
static int callbacks;
static int mode;

static void on_connection(uv_stream_t *server, int status) {
  assert(status == 0);
  callbacks++;
  if (mode == 1) return; /* Leave accepted_fd pending for the caller. */
  assert(uv_tcp_init(&loop, &peers[accepted]) == 0);
  assert(uv_accept(server, (uv_stream_t *)&peers[accepted]) == 0);
  uv_close((uv_handle_t *)&peers[accepted++], NULL);
  if (mode == 2) uv_close((uv_handle_t *)server, NULL);
}

static void run_case(int test_mode) {
  struct sockaddr_in addr;
  int addr_len = sizeof(addr);
  int clients[CONNECTIONS];
  accepted = callbacks = 0;
  mode = test_mode;
  assert(uv_loop_init(&loop) == 0);
  assert(uv_tcp_init(&loop, &listener) == 0);
  assert(uv_ip4_addr("127.0.0.1", 0, &addr) == 0);
  assert(uv_tcp_bind(&listener, (struct sockaddr *)&addr, 0) == 0);
  assert(uv_listen((uv_stream_t *)&listener, 128, on_connection) == 0);
  assert(uv_tcp_getsockname(&listener, (struct sockaddr *)&addr, &addr_len) == 0);
  /* Queue all handshakes before the server runs, avoiding timing assertions. */
  for (int i = 0; i < CONNECTIONS; i++) {
    clients[i] = socket(AF_INET, SOCK_STREAM, 0);
    assert(clients[i] >= 0);
    assert(connect(clients[i], (struct sockaddr *)&addr, sizeof(addr)) == 0);
  }
  uv_run(&loop, UV_RUN_NOWAIT);
  if (mode == 0) {
    fprintf(stderr, "accepted %d/%d in one loop pass\n", accepted, CONNECTIONS);
    assert(accepted == CONNECTIONS);
  } else if (mode == 1) {
    assert(callbacks == 1 && accepted == 0);
    uv_run(&loop, UV_RUN_NOWAIT);
    assert(callbacks == 1);
    assert(uv_tcp_init(&loop, &peers[accepted]) == 0);
    assert(uv_accept((uv_stream_t *)&listener, (uv_stream_t *)&peers[accepted]) == 0);
    uv_close((uv_handle_t *)&peers[accepted++], NULL);
    mode = 0;
    uv_run(&loop, UV_RUN_NOWAIT);
    assert(accepted == CONNECTIONS);
  } else {
    assert(callbacks == 1 && accepted == 1);
  }
  if (!uv_is_closing((uv_handle_t *)&listener))
    uv_close((uv_handle_t *)&listener, NULL);
  for (int i = 0; i < CONNECTIONS; i++) close(clients[i]);
  uv_run(&loop, UV_RUN_DEFAULT);
  assert(uv_loop_close(&loop) == 0);
}
#endif

int main(void) {
#ifndef _WIN32
  run_case(0);
  run_case(1);
  run_case(2);
  puts("listener accept burst tests passed");
#endif
  return 0;
}
