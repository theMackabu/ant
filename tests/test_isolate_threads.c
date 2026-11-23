#include "internal.h"
#include "bootstrap.h"
#include "reactor.h"
#include "runtime.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define THREADS 4
#define ROUNDS 3
#define STACK_SIZE (8u << 20)

static const char script[] =
  "globalThis.result = 0;\n"
  "function shape(i) { const o = {}; o['k' + (i % 97)] = i; o.v = i; return o; }\n"
  "function hot(n) { let s = 0; for (let i = 0; i < n; i++) s = (s + i * 3) | 0; return s; }\n"
  "class P { constructor(x) { this.x = x; } get y() { return this.x * 2; } }\n"
  "async function chain(n) { let t = 0; for (let i = 0; i < n; i++) t += await Promise.resolve(i); return t; }\n"
  "let keep = [];\n"
  "for (let r = 0; r < 40; r++) {\n"
  "  const batch = [];\n"
  "  for (let i = 0; i < 2000; i++) batch.push(shape(i), new P(i), [i, String(i)], { ['s' + r]: i });\n"
  "  keep.push(batch[r]);\n"
  "  result += hot(5000) & 0xff;\n"
  "  result += JSON.parse(JSON.stringify({ a: r, b: [1, 2, 3] })).a;\n"
  "  result += 'abc'.repeat(r).replace(/b/g, 'x').length;\n"
  "  const m = new Map(); for (let i = 0; i < 200; i++) m.set('m' + i, i); result += m.size;\n"
  "}\n"
  "for (let i = 0; i < 3000; i++) { const o = {}; o['u' + i + '_' + Math.random().toString(36).slice(2, 8)] = i; }\n"
  "result += crypto.randomUUID().length;\n"
  "/(a)(b)/.exec('xab'); result += RegExp.$1.length + RegExp.$2.length;\n"
  "sessionStorage.setItem('k', 'v'); result += sessionStorage.getItem('k').length; sessionStorage.removeItem('k');\n"
  "result += structuredClone({ a: [1, 2, { b: 3 }] }).a[2].b;\n"
  "result += new TextDecoder().decode(new TextEncoder().encode('hé')).length;\n"
  "result += new URL('https://a.b/c?d=1').searchParams.get('d') | 0;\n"
  "result += new Uint8Array(new SharedArrayBuffer(16)).length;\n"
  "chain(300).then(v => { result += v; });\n"
  "setTimeout(() => { result += keep.length; }, 1);\n";

typedef struct {
  int id;
  double results[ROUNDS];
} worker_t;

static char *argv0[] = { "test-isolate-threads", NULL };

static double run_once(void) {
  volatile char stack_base;
  ant_t *js = Ant_Create(nullptr);
  assert(js);
  js_setstackbase(js, (void *)&stack_base);
  js_setstacklimit(js, STACK_SIZE * 3 / 4);
  ant_runtime_init(js, 1, argv0, NULL);
  ant_bootstrap_modules(js);

  ant_value_t r = js_eval_bytecode_eval(js, script, sizeof(script) - 1, false);
  assert(!is_err(r));
  js_run_event_loop(js);

  ant_value_t result = js_get(js, js_glob(js), "result");
  assert(vtype(result) == kTypeNumber);
  double value = Ant_Value_AsNumberUnchecked(result);
  Ant_Destroy(js);
  return value;
}

static void *worker_main(void *arg) {
  worker_t *w = arg;
  for (int i = 0; i < ROUNDS; i++) w->results[i] = run_once();
  ant_thread_cleanup();
  return NULL;
}

int main(void) {
  double expected = 0;
  {
    worker_t solo = { .id = -1 };
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, STACK_SIZE);
    pthread_t t;
    assert(pthread_create(&t, &attr, worker_main, &solo) == 0);
    assert(pthread_join(t, NULL) == 0);
    expected = solo.results[0];
    assert(expected > 0);
  }

  worker_t workers[THREADS];
  pthread_t threads[THREADS];
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, STACK_SIZE);

  for (int i = 0; i < THREADS; i++) {
    workers[i] = (worker_t){ .id = i };
    assert(pthread_create(&threads[i], &attr, worker_main, &workers[i]) == 0);
  }
  for (int i = 0; i < THREADS; i++) assert(pthread_join(threads[i], NULL) == 0);

  for (int i = 0; i < THREADS; i++)
    for (int r = 0; r < ROUNDS; r++) {
      if (workers[i].results[r] != expected) {
        fprintf(stderr, "thread %d round %d: %.0f != %.0f\n", i, r, workers[i].results[r], expected);
        return 1;
      }
    }

  printf("PASS %d isolates on %d threads, %d rounds each, result %.0f\n", THREADS * ROUNDS, THREADS, ROUNDS, expected);
}
