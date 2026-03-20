function nowMs() {
  if (typeof performance !== 'undefined' && performance && typeof performance.now === 'function') {
    return performance.now();
  }
  return Date.now();
}

const BENCH_WARMUP_RUNS = 2;
const BENCH_SAMPLE_RUNS = 7;

function percentile(sorted, p) {
  if (sorted.length === 0) return 0;
  if (sorted.length === 1) return sorted[0];
  const pos = (sorted.length - 1) * p;
  const base = Math.floor(pos);
  const frac = pos - base;
  const next = sorted[base + 1];
  if (next === undefined) return sorted[base];
  return sorted[base] + (next - sorted[base]) * frac;
}

function stableNumericSort(values) {
  const out = values.slice();
  for (let i = 1; i < out.length; i++) {
    const x = out[i];
    let j = i - 1;
    while (j >= 0 && out[j] > x) {
      out[j + 1] = out[j];
      j--;
    }
    out[j + 1] = x;
  }
  return out;
}

function makeChain(depth) {
  function C() {}
  const root = { marker: 1 };
  C.prototype = root;

  let obj = Object.create(root);
  for (let i = 0; i < depth; i++) obj = Object.create(obj);
  return { C, obj, root };
}

function bench(label, fn, iters) {
  const warmupIters = Math.max(100_000, (iters / 4) | 0);
  for (let i = 0; i < BENCH_WARMUP_RUNS; i++) fn(warmupIters);

  let result = 0;
  const samples = [];
  for (let i = 0; i < BENCH_SAMPLE_RUNS; i++) {
    const t0 = nowMs();
    const r = fn(iters);
    const dt = nowMs() - t0;
    if (i === 0) result = r;
    samples.push(dt);
  }

  const sorted = stableNumericSort(samples);
  const min = sorted[0];
  const med = percentile(sorted, 0.5);
  const p95 = percentile(sorted, 0.95);
  const max = sorted[sorted.length - 1];
  const opsPerMs = med > 0 ? (iters / med).toFixed(2) : 'inf';

  console.log(
    label +
      ': median ' + med.toFixed(2) + 'ms (' + opsPerMs + ' ops/ms)' +
      ', p95 ' + p95.toFixed(2) + 'ms' +
      ', min ' + min.toFixed(2) + 'ms' +
      ', max ' + max.toFixed(2) + 'ms' +
      ' result=' + result
  );
}

function runDepth(depth) {
  const iters = 2_000_000;
  const { C, obj, root } = makeChain(depth);

  console.log('\n=== depth=' + depth + ' ===');
  console.log('sanity instanceof=' + (obj instanceof C));
  console.log('sanity isPrototypeOf=' + root.isPrototypeOf(obj));
  console.log('sanity marker=' + obj.marker);

  bench(
    'instanceof',
    n => {
      let c = 0;
      for (let i = 0; i < n; i++) if (obj instanceof C) c++;
      return c;
    },
    iters
  );

  bench(
    'isPrototypeOf',
    n => {
      let c = 0;
      for (let i = 0; i < n; i++) if (root.isPrototypeOf(obj)) c++;
      return c;
    },
    iters
  );

  bench(
    'prop_lookup',
    n => {
      let c = 0;
      for (let i = 0; i < n; i++) c += obj.marker;
      return c;
    },
    iters
  );
}

console.log('prototype-chain benchmark');
runDepth(8);
runDepth(24);
runDepth(40);
