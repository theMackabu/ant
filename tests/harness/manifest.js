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
    'test_node_http_incoming_message_readable.cjs',
    'test_stream_readable_to_web.cjs'
  ];
  for (const f of REGRESSION_TESTS) list.push({ group: 'tests', type: 'test', name: `tests/${f}`, entry: `tests/${f}` });

  list.push(
    { group: 'servers', type: 'server', name: 'hono', entry: 'examples/npm/hono/src/index.ts' },
    { group: 'servers', type: 'server', name: 'express', entry: 'examples/npm/express/index.cjs' },
    { group: 'servers', type: 'server', name: 'h3', entry: 'examples/npm/h3' },
    { group: 'servers', type: 'server', name: 'elysia', entry: 'examples/npm/elysia' }
  );

  const scrubPid = [[/\b\d{3,7}\b/g, '<pid>']];
  list.push(
    { group: 'examples', type: 'snapshot', name: 'smoke', entry: 'examples/npm/smoke/index.js' },
    { group: 'examples', type: 'snapshot', name: 'djot', entry: 'examples/npm/djot/index.ts' },
    { group: 'examples', type: 'snapshot', name: 'esbuild', entry: 'examples/npm/esbuild/index.ts' },
    { group: 'examples', type: 'snapshot', name: 'rolldown', entry: 'examples/npm/rolldown/index.ts' },
    { group: 'examples', type: 'snapshot', name: 'react', entry: 'examples/npm/react/index.js' },
    { group: 'examples', type: 'snapshot', name: 'preact', entry: 'examples/npm/preact/index.js' },
    { group: 'examples', type: 'snapshot', name: 'tar', entry: 'examples/npm/tar/index.js', scrub: [[/\d+ bytes/g, '<n> bytes']] },
    { group: 'examples', type: 'snapshot', name: 'lua', entry: 'examples/npm/lua/lua.ts' },
    { group: 'examples', type: 'snapshot', name: 'pidtree', entry: 'examples/npm/pidtree/index.js', scrub: scrubPid },
    { group: 'examples', type: 'skip', name: 'discord', reason: 'needs a live Discord token' },
    { group: 'examples', type: 'skip', name: 'undici', reason: 'excluded for now' },
    { group: 'examples', type: 'skip', name: 'jiti', reason: 'excluded for now' }
  );

  const nowMs = Date.now();
  const ms = (name, re, min = 0.0001, max = 120000) => ({ name, re, min, max });
  list.push(
    {
      group: 'demos',
      type: 'demo',
      name: 'event_loop',
      entry: 'examples/demo/event_loop.js',
      checks: [
        ms('iterations/sec (millions)', /([\d.,]+)M event loop iterations\/sec/, 0.01, 100000),
        ms('measured window seconds', /in ([\d.]+)s\)/, 3, 60)
      ]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'events',
      entry: 'examples/demo/events.js',
      checks: [{ name: 'login wall-clock near now', re: /Login time: (\d+)/, min: nowMs - 60000, max: nowMs + 3600000 }]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'fizzbuzz',
      entry: 'examples/demo/fizzbuzz.js',
      checks: [ms('per-impl duration ms', /: ([\d.]+)ms for n=1000/)]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'microbench',
      entry: 'examples/demo/microbench.js',
      checks: [{ name: 'simple add ms', re: /simple add: ([\d.]+)ms/, min: 0, max: 120000 }]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'microbench2',
      entry: 'examples/demo/microbench2.js',
      checks: [ms('bench duration ms', /: ([\d.]+)ms \(result/)]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'pi',
      entry: 'examples/demo/pi.js',
      checks: [{ name: 'pi value', re: /≈ (3\.[\d]+)/, min: 3.14158, max: 3.14161 }, ms('compute time ms', /Time: ([\d.]+) ms/)]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'triples',
      entry: 'examples/demo/triples.js',
      checks: [{ name: 'triple count for c<=200', re: /Found (\d+) Pythagorean/, min: 127, max: 127 }]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'uptime',
      entry: 'examples/demo/uptime.js',
      checks: [
        { name: 'uptime days', re: /up (\d+) days/, min: 0, max: 3650 },
        { name: 'load average', re: /load averages: ([\d.]+)/, min: 0, max: 500 }
      ]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'fib_iterative',
      entry: 'examples/demo/fibonacci_iterative.js',
      checks: [ms('fib(5000) time ms', /Time: ([\d.]+) ms/)]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'fib_memo',
      entry: 'examples/demo/fibonacci_memo.js',
      checks: [{ name: 'fib(35) value', re: /fibonacci\(35\) = (\d+)/, min: 9227465, max: 9227465 }, ms('time ms', /Time: ([\d.]+) ms/)]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'fib_tco',
      entry: 'examples/demo/fibonacci_tco.js',
      checks: [
        { name: 'fib(35) value', re: /fibonacci\(35\) = (\d+)/, min: 9227465, max: 9227465 },
        ms('per-call µs', /([\d.]+) µs\/call/, 0.00001, 10000)
      ]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'fib_iter_tco',
      entry: 'examples/demo/fibonacci_iterative_tco.js',
      checks: [
        { name: 'fib(1476) mantissa', re: /fibonacci\(1476\) = (1\.3069892237633987)e\+308/, min: 1.3, max: 1.31 },
        ms('time ms', /Time: ([\d.]+) ms/)
      ]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'fib_iter_tco_double',
      entry: 'examples/demo/fibonacci_iterative_tco_double.js',
      checks: [
        { name: 'fib(5000) mantissa', re: /fibonacci\(5000\) ~= (3\.8789684543883243)e\+1044/, min: 3.8, max: 3.9 },
        ms('time ms', /Time: ([\d.]+) ms/)
      ]
    },
    {
      group: 'demos',
      type: 'demo',
      name: 'fib_iter_tco_number',
      entry: 'examples/demo/fibonacci_iterative_tco_number.js',
      checks: [
        { name: 'fib(5000) mantissa', re: /fibonacci\(5000\) ~= (3\.8789684543883243)e\+1044/, min: 3.8, max: 3.9 },
        ms('time ms', /Time: ([\d.]+) ms/)
      ]
    }
  );

  list.push({
    group: 'perf',
    type: 'demo',
    name: 'bench_dec',
    entry: 'tests/bench_dec.js',
    checks: [ms('duration ms', /([\d.]+)ms/, 100, 600)]
  });

  list.push(
    { group: 'oha', type: 'oha', name: 'hono rps', entry: 'examples/npm/hono/src/index.ts', refRps: 25000, minRps: 17500 },
    { group: 'oha', type: 'oha', name: 'express rps', entry: 'examples/npm/express/index.cjs', refRps: 15000, minRps: 10500 },
    { group: 'oha', type: 'oha', name: 'h3 rps', entry: 'examples/npm/h3', refRps: 15000, minRps: 10500 },
    { group: 'oha', type: 'oha', name: 'elysia rps', entry: 'examples/npm/elysia', refRps: 66000, minRps: 46000 }
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
