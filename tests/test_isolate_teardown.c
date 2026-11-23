#include "internal.h"
#include "bootstrap.h"
#include "reactor.h"
#include "runtime.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#define STACK_SIZE (8u << 20)

typedef struct {
  const char *name;
  const char *script;
} scenario_t;

static const scenario_t scenarios[] = {
  { "timers", "setInterval(() => {}, 1); setTimeout(() => {}, 100000); setImmediate(() => {});" },
  { "net",
    "import('node:net').then(net => {"
    "  const server = net.createServer(s => s.on('data', () => {})).listen(0, '127.0.0.1', () => {"
    "    const c = net.connect(server.address().port, '127.0.0.1', () => c.write('x'));"
    "  });"
    "});" },
  { "http",
    "import('node:http').then(http => {"
    "  const server = http.createServer((q, r) => {}).listen(0, '127.0.0.1', () => {"
    "    http.get('http://127.0.0.1:' + server.address().port + '/', () => {}).on('error', () => {});"
    "  });"
    "});" },
  { "fetch",
    "import('node:net').then(net => {"
    "  const server = net.createServer(() => {}).listen(0, '127.0.0.1', () => {"
    "    fetch('http://127.0.0.1:' + server.address().port + '/').catch(() => {});"
    "  });"
    "});" },
  { "fs",
    "import('node:fs').then(fs => {"
    "  fs.watch('.', () => {});"
    "  fs.watchFile('meson.build', { interval: 5 }, () => {});"
    "  fs.readFile('meson.build', () => {});"
    "  fs.promises.readdir('.').catch(() => {});"
    "});" },
  { "child",
    "import('node:child_process').then(cp => {"
    "  const p = cp.spawn('sleep', ['5']); p.on('error', () => {});"
    "  cp.exec('sleep 5', () => {});"
    "});" },
  { "dns", "import('node:dns').then(dns => { dns.lookup('localhost', () => {}); dns.promises.resolve4('localhost').catch(() => {}); });" },
  { "atomics", "const i = new Int32Array(new SharedArrayBuffer(4)); Atomics.waitAsync(i, 0, 0, 100000);" },
  { "abort", "AbortSignal.timeout(100000).addEventListener('abort', () => {});" },
  { "signals", "process.on('SIGUSR2', () => {});" },
  { "websocket",
    "import('node:net').then(net => {"
    "  const server = net.createServer(() => {}).listen(0, '127.0.0.1', () => {"
    "    const ws = new WebSocket('ws://127.0.0.1:' + server.address().port + '/'); ws.onerror = () => {};"
    "  });"
    "});" },
  { "stdio", "process.stdout.write(''); process.stderr.write('');" },
  { "serve",
    "const s = Ant.serve({ port: 0, hostname: '127.0.0.1', fetch: () => new Promise(() => {}) });"
    "fetch('http://127.0.0.1:' + s.port + '/').catch(() => {});" },
  { "tls",
    "import('node:net').then(net => import('node:tls').then(tls => {"
    "  const server = net.createServer(() => {}).listen(0, '127.0.0.1', () => {"
    "    const c = tls.connect({ port: server.address().port, host: '127.0.0.1', rejectUnauthorized: false }); c.on('error', () => {});"
    "  });"
    "}));" },
  { "readline",
    "import('node:readline').then(rl => import('node:stream').then(st => {"
    "  const input = new st.PassThrough(); const i = rl.createInterface({ input }); i.on('line', () => {}); input.write('a');"
    "}));" },
};

static char *argv0[] = { "test-isolate-teardown", NULL };

static ant_t *create(void *stack_base) {
  ant_t *js = Ant_Create(nullptr);
  assert(js);
  js_setstackbase(js, stack_base);
  js_setstacklimit(js, STACK_SIZE * 3 / 4);
  ant_runtime_init(js, 1, argv0, NULL);
  ant_bootstrap_modules(js);
  return js;
}

static void pump(ant_t *js, int passes) {
  for (int i = 0; i < passes; i++) {
    js_poll_events(js);
    ant_uv_run(js->loop, UV_RUN_NOWAIT);
    js_poll_events(js);
    uv_sleep(2);
  }
}

static void count_handle(uv_handle_t *handle, void *arg) {
  if (!uv_is_closing(handle)) ((int *)arg)[handle->type]++;
}

static void report_handles(const char *name, uv_loop_t *loop) {
  int counts[UV_HANDLE_TYPE_MAX] = {0};
  uv_walk(loop, count_handle, counts);
  if (!getenv("TEARDOWN_VERBOSE")) return;
  fprintf(stderr, "%s:", name);
  for (int t = 0; t < UV_HANDLE_TYPE_MAX; t++)
    if (counts[t]) fprintf(stderr, " %s=%d", uv_handle_type_name((uv_handle_type)t), counts[t]);
  fprintf(stderr, "\n");
}

static void *run_scenario(void *arg) {
  const scenario_t *sc = arg;
  volatile char stack_base;
  ant_t *js = create((void *)&stack_base);
  ant_value_t r = js_eval_bytecode_eval(js, sc->script, strlen(sc->script), false);
  if (is_err(r)) {
    fprintf(stderr, "%s: script failed\n", sc->name);
    exit(1);
  }
  pump(js, 25);
  report_handles(sc->name, js->loop);
  Ant_Destroy(js);
  ant_thread_cleanup();
  return NULL;
}

int main(int argc, char **argv) {
  volatile char stack_base;
  ant_t *main_js = create((void *)&stack_base);

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, STACK_SIZE);

  size_t count = sizeof(scenarios) / sizeof(scenarios[0]);
  for (size_t i = 0; i < count; i++) {
    if (argc > 1 && strcmp(argv[1], scenarios[i].name) != 0) continue;
    pthread_t t;
    assert(pthread_create(&t, &attr, run_scenario, (void *)&scenarios[i]) == 0);
    assert(pthread_join(t, NULL) == 0);
    printf("ok %s\n", scenarios[i].name);
    fflush(stdout);
  }

  Ant_Destroy(main_js);
  puts("PASS isolate teardown with pending handles");
}
