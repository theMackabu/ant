import { readdirSync } from 'node:fs';

const SPEC_SKIP = new Set(['run.js', 'helpers.js']);
const SPEC_MANUAL = new Set([]);

export function targets() {
  const list = [];

  for (const f of readdirSync('examples/spec').sort()) {
    if (!f.endsWith('.js') || SPEC_SKIP.has(f) || SPEC_MANUAL.has(f)) continue;
    list.push({ group: 'spec', type: 'spec', name: `spec/${f}`, entry: `examples/spec/${f}` });
  }

  const REGRESSION_TESTS = [
    'test_promise.cjs',
    'test_arguments_async.cjs',
    'test_upvalue_gc.cjs',
    'test_jit_derived_ctor.cjs',
    'test_jit_string_builder_snapshot.cjs',
    'test_template_self_append.cjs',
    'test_global_accessor_read.cjs',
    'test_jit_inline_call_errors.cjs',
    'test_string_length_accumulation.cjs',
    'test_node_http_incoming_message_readable.cjs',
    'test_stream_readable_to_web.cjs'
  ];
  for (const f of REGRESSION_TESTS) list.push({ group: 'tests', type: 'test', name: `tests/${f}`, entry: `tests/${f}` });

  list.push(
    { group: 'servers', type: 'server', name: 'hono', entry: 'examples/npm/hono/src/index.ts' },
    { group: 'servers', type: 'server', name: 'express', entry: 'examples/npm/express/index.cjs' },
    { group: 'servers', type: 'server', name: 'h3', entry: 'examples/npm/h3' },
    { group: 'servers', type: 'server', name: 'elysia1', entry: 'examples/npm/elysia1' },
    { group: 'servers', type: 'server', name: 'elysia2', entry: 'examples/npm/elysia2' }
  );

  const scrubPid = [[/\b\d{3,7}\b/g, '<pid>']];
  list.push(
    { group: 'examples', type: 'snapshot', name: 'smoke', entry: 'examples/npm/smoke/index.js' },
    { group: 'examples', type: 'snapshot', name: 'djot', entry: 'examples/npm/djot/index.ts' },
    { group: 'examples', type: 'snapshot', name: 'esbuild', entry: 'examples/npm/esbuild/index.ts' },
    {
      group: 'examples',
      type: 'snapshot',
      name: 'rolldown',
      entry: 'examples/npm/rolldown/index.ts',
      scrub: [[/(?:\.\.\/)+\.ant\/pkg\/cache\/[0-9a-f]+/g, '<ant-cache>']]
    },
    { group: 'examples', type: 'snapshot', name: 'react', entry: 'examples/npm/react/index.js' },
    { group: 'examples', type: 'snapshot', name: 'preact', entry: 'examples/npm/preact/index.js' },
    { group: 'examples', type: 'snapshot', name: 'tar', entry: 'examples/npm/tar/index.js', scrub: [[/\d+ bytes/g, '<n> bytes']] },
    { group: 'examples', type: 'snapshot', name: 'lua', entry: 'examples/npm/lua/lua.ts' },
    { group: 'examples', type: 'snapshot', name: 'pidtree', entry: 'examples/npm/pidtree/index.js', scrub: scrubPid },
    { group: 'examples', type: 'skip', name: 'discord', reason: 'needs a live Discord token' },
    { group: 'examples', type: 'skip', name: 'undici', reason: 'excluded for now' },
    { group: 'examples', type: 'skip', name: 'jiti', reason: 'excluded for now' }
  );

  const ms = (name, re, max, min = 0) => ({ name, re, min, max });
  const exact = (name, re, expected, count) => ({ name, re, expected, ...(count === undefined ? {} : { count }) });
  const nonNegative = (name, re) => ({ name, re, min: 0, max: Number.MAX_VALUE });
  list.push(
    {
      group: 'demos',
      type: 'demo',
      name: 'event_loop',
      entry: 'examples/demo/event_loop.js',
      checks: [
        { name: 'iterations/sec (millions)', re: /([\d.,]+)M event loop iterations\/sec/, min: 1, max: 20 },
        { name: 'measured window seconds', re: /in ([\d.]+)s\)/, min: 4, max: 6 }
      ]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'events',
      entry: 'examples/demo/events.js',
      checks: [
        { name: 'login wall-clock near now', re: /Login time: (\d+)/, nowToleranceMs: 60000 },
        exact('login username', /User logged in: (\w+)/, 'alice'),
        exact('click listener calls', /(Button clicked!)/, 'Button clicked!', 2),
        exact('once listener calls', /(Initialization running)/, 'Initialization running', 1),
        exact('once listener total', /Total init calls: (\d+)/, 1),
        exact('removed listener calls', /Handler called with: (.+)/, 'first call', 1),
        exact('build duration', /Build finished: (\d+)ms,/, 142),
        exact('build file count', /Build finished: \d+ms, (\d+) files/, 37)
      ]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'fizzbuzz',
      entry: 'examples/demo/fizzbuzz.js',
      checks: [
        exact('normal correctness', /normal: (true|false)/, 'true'),
        exact('bitwise correctness', /bitwise: (true|false)/, 'true'),
        exact('unrolled correctness', /ultimate: (true|false)/, 'true'),
        ms('per-impl duration ms', /: ([\d.]+)ms for n=1000/, 5)
      ]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'microbench',
      entry: 'examples/demo/microbench.js',
      checks: [exact('simple add result', /simple add: [\d.]+ms \(result: (\d+)\)/, '40000000000'), ms('simple add ms', /simple add: ([\d.]+)ms/, 100)]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'microbench2',
      entry: 'examples/demo/microbench2.js',
      checks: [
        exact('empty loop result', /warm empty loop: [\d.]+ms \(result: (\d+)\)/, '1999999'),
        exact('inline add result', /warm inline add: [\d.]+ms \(result: (\d+)\)/, '4000000000000'),
        exact('call add results', /(?:cold|warm) call add: [\d.]+ms \(result: (\d+)\)/, '4000000000000', 2),
        ms('bench duration ms', /: ([\d.]+)ms \(result/, 200)
      ]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'pi',
      entry: 'examples/demo/pi.js',
      checks: [{ name: 'pi value', re: /≈ (3\.[\d]+)/, min: 3.14158, max: 3.14161 }, ms('compute time ms', /Time: ([\d.]+) ms/, 100)]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'triples',
      entry: 'examples/demo/triples.js',
      checks: [{ name: 'triple count for c<=200', re: /Found (\d+) Pythagorean/, min: 127, max: 127 }, ms('compute time ms', /Time: ([\d.]+) ms/, 100)]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'uptime',
      entry: 'examples/demo/uptime.js',
      checks: [
        nonNegative('uptime days', /up (\d+) days/),
        { name: 'uptime hours', re: /days, (\d+):/, min: 0, max: 23 },
        { name: 'uptime minutes', re: /days, \d+:(\d{2})/, min: 0, max: 59 },
        nonNegative('logged-in users', /, (\d+) users?/),
        nonNegative('one-minute load average', /load averages: ([\d.]+)/),
        nonNegative('five-minute load average', /load averages: [\d.]+ ([\d.]+)/),
        nonNegative('fifteen-minute load average', /load averages: [\d.]+ [\d.]+ ([\d.]+)/)
      ]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'fib_iterative',
      entry: 'examples/demo/fibonacci_iterative.js',
      checks: [
        exact('fib(5000) prefix', /fibonacci\(5000\) = (38789684543883256337)\d{1005}85178408674382863125/, '38789684543883256337'),
        ms('fib(5000) time ms', /Time: ([\d.]+) ms/, 100)
      ]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'fib_memo',
      entry: 'examples/demo/fibonacci_memo.js',
      checks: [{ name: 'fib(35) value', re: /fibonacci\(35\) = (\d+)/, min: 9227465, max: 9227465 }, ms('time ms', /Time: ([\d.]+) ms/, 10)]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'fib_tco',
      entry: 'examples/demo/fibonacci_tco.js',
      checks: [
        { name: 'fib(35) value', re: /fibonacci\(35\) = (\d+)/, min: 9227465, max: 9227465 },
        ms('per-call µs', /([\d.]+) µs\/call/, 10)
      ]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'fib_iter_tco',
      entry: 'examples/demo/fibonacci_iterative_tco.js',
      checks: [
        { name: 'fib(1476) mantissa', re: /fibonacci\(1476\) = (1\.[\d]+)e\+308/, min: 1.3069, max: 1.3071 },
        ms('time ms', /Time: ([\d.]+) ms/, 10)
      ]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'fib_iter_tco_double',
      entry: 'examples/demo/fibonacci_iterative_tco_double.js',
      checks: [
        { name: 'fib(5000) mantissa', re: /fibonacci\(5000\) ~= (3\.[\d]+)e\+1044/, min: 3.8789, max: 3.879 },
        ms('time ms', /Time: ([\d.]+) ms/, 100)
      ]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'fib_iter_tco_number',
      entry: 'examples/demo/fibonacci_iterative_tco_number.js',
      checks: [
        { name: 'fib(5000) mantissa', re: /fibonacci\(5000\) ~= (3\.[\d]+)e\+1044/, min: 3.8789, max: 3.879 },
        ms('time ms', /Time: ([\d.]+) ms/, 50)
      ]
    }
  );

  list.push({
    group: 'perf',
    type: 'demo',
    name: 'bench_dec',
    entry: 'tests/bench_dec.js',
    checks: [ms('duration ms', /([\d.]+)ms/, 600, 100)]
  });

  list.push(
    { group: 'oha', type: 'oha', name: 'hono rps', entry: 'examples/npm/hono/src/index.ts', refRps: 32000, minRps: 22400 },
    { group: 'oha', type: 'oha', name: 'express rps', entry: 'examples/npm/express/index.cjs', refRps: 15000, minRps: 10500 },
    { group: 'oha', type: 'oha', name: 'h3 rps', entry: 'examples/npm/h3', refRps: 25000, minRps: 17500 },
    { group: 'oha', type: 'oha', name: 'elysia1 rps', entry: 'examples/npm/elysia1/bench-server.ts', refRps: 120000, minRps: 84000 },
    { group: 'oha', type: 'oha', name: 'elysia2 rps', entry: 'examples/npm/elysia2/bench-server.ts', refRps: 125000, minRps: 87500 }
  );

  list.push({
    group: 'network',
    defaultOff: true,
    type: 'snapshot',
    name: 'ky',
    entry: 'examples/npm/ky/index.js'
  });

  return list;
}
