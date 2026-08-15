#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tlsuv/connector.h>
#include <tlsuv/websocket.h>
#include <uv.h>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

static int g_failures = 0;

static void check(int ok, const char *message) {
  if (ok) {
    printf("ok: %s\n", message);
  } else {
    fprintf(stderr, "FAIL: %s\n", message);
    g_failures++;
  }
}

typedef struct fake_state_s {
  uv_timer_t timer;
  tlsuv_connect_cb cb;
  void *ctx;
  uv_os_sock_t deliver_sock;
  int deliver_status;
  int connect_calls;
  int cancel_calls;
  int completions;
  int timer_active;
} fake_state_t;

static fake_state_t g_fake;

static void fake_complete_cb(uv_timer_t *timer) {
  fake_state_t *f = timer->data;
  f->timer_active = 0;
  f->completions++;
  f->cb(f->deliver_sock, f->deliver_status, f->ctx);
}

static tlsuv_connector_req fake_connect(uv_loop_t *loop, const tlsuv_connector_t *self,
                                        const char *host, const char *port,
                                        tlsuv_connect_cb cb, void *ctx) {
  (void)loop;
  (void)self;
  (void)host;
  (void)port;
  g_fake.connect_calls++;
  g_fake.cb = cb;
  g_fake.ctx = ctx;
  return &g_fake;
}

static void fake_cancel(tlsuv_connector_req req) {
  fake_state_t *f = (fake_state_t *)(uintptr_t)req;
  f->cancel_calls++;
  f->timer_active = 1;
  uv_timer_start(&f->timer, fake_complete_cb, 5, 0);
}

static int fake_set_auth(tlsuv_connector_t *self, tlsuv_auth_t auth, const char *username, const char *password) {
  (void)self;
  (void)auth;
  (void)username;
  (void)password;
  return UV_ENOTSUP;
}

static void fake_free(void *self) {
  (void)self;
}

static const tlsuv_connector_t fake_connector = {
  .connect = fake_connect,
  .set_auth = fake_set_auth,
  .cancel = fake_cancel,
  .free = fake_free,
};

typedef struct scenario_s {
  int in_close_call;
  int close_cb_calls;
  int close_cb_sync;
  int close_cb_completions_seen;
  int conn_cb_calls;
  void *tr_at_close;
} scenario_t;

static scenario_t g_scn;

static void scn_conn_cb(uv_connect_t *req, int status) {
  (void)req;
  (void)status;
  g_scn.conn_cb_calls++;
}

static void scn_data_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  (void)stream;
  (void)nread;
  (void)buf;
}

static void scn_close_cb(uv_handle_t *handle) {
  tlsuv_websocket_t *ws = (tlsuv_websocket_t *)handle;
  g_scn.close_cb_calls++;
  g_scn.close_cb_sync = g_scn.in_close_call;
  g_scn.close_cb_completions_seen = g_fake.completions;
  g_scn.tr_at_close = ws->tr;
  free(ws);
}

static void run_scenario(const char *name, uv_os_sock_t deliver_sock, int deliver_status) {
  uv_loop_t loop;
  if (uv_loop_init(&loop) != 0) {
    fprintf(stderr, "FAIL: %s: uv_loop_init\n", name);
    g_failures++;
    return;
  }

  memset(&g_fake, 0, sizeof(g_fake));
  memset(&g_scn, 0, sizeof(g_scn));
  uv_timer_init(&loop, &g_fake.timer);
  g_fake.timer.data = &g_fake;
  g_fake.deliver_sock = deliver_sock;
  g_fake.deliver_status = deliver_status;

  tlsuv_websocket_t *ws = calloc(1, sizeof(*ws));
  uv_connect_t conn_req;
  memset(&conn_req, 0, sizeof(conn_req));

  check(tlsuv_websocket_init(&loop, ws) == 0, "websocket initialized");
  check(tlsuv_websocket_set_connector(ws, &fake_connector) == 0, "fake connector installed");
  check(tlsuv_websocket_connect(&conn_req, ws, "ws://127.0.0.1:1/", scn_conn_cb, scn_data_cb) == 0, "connect started");
  check(g_fake.connect_calls == 1, "fake connector received the connect request");

  g_scn.in_close_call = 1;
  tlsuv_websocket_close(ws, scn_close_cb);
  g_scn.in_close_call = 0;

  printf("-- %s --\n", name);
  check(g_scn.close_cb_calls == 0, "close callback did not run synchronously from tlsuv_websocket_close");
  check(g_fake.cancel_calls == 1, "connector cancel was requested exactly once");

  uv_run(&loop, UV_RUN_DEFAULT);

  check(g_fake.completions == 1, "connector cancellation completed exactly once");
  check(g_scn.close_cb_calls == 1, "close callback ran exactly once");
  check(g_scn.close_cb_sync == 0, "close callback was not synchronous");
  check(g_scn.close_cb_completions_seen == 1, "close callback ran only after connector completion");
  check(g_scn.conn_cb_calls == 0, "user connection callback was not invoked for a user-requested close");
  check(g_scn.tr_at_close == NULL, "no transport was installed on a closed websocket");

  uv_close((uv_handle_t *)&g_fake.timer, NULL);
  uv_run(&loop, UV_RUN_DEFAULT);
  check(uv_loop_close(&loop) == 0, "loop drained cleanly");
}

int main(void) {
  run_scenario("cancelled connect", (uv_os_sock_t)-1, UV_ECANCELED);

#ifndef _WIN32
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    fprintf(stderr, "FAIL: socketpair: %s\n", strerror(errno));
    return 1;
  }
  run_scenario("success after cancel", sv[0], 0);
  errno = 0;
  check(fcntl(sv[0], F_GETFD) == -1 && errno == EBADF, "raced socket was disposed (fd closed)");
  close(sv[1]);
#endif

  if (g_failures) {
    fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  printf("websocket connect-cancel lifetime test passed\n");
  return 0;
}
