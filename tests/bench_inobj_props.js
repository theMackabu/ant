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

function initObj(o, n, base) {
  if (n >= 1) o.a = 1 + base;
  if (n >= 2) o.b = 2 + base;
  if (n >= 3) o.c = 3 + base;
  if (n >= 4) o.d = 4 + base;
  if (n >= 5) o.e = 5 + base;
  if (n >= 6) o.f = 6 + base;
  if (n >= 7) o.g = 7 + base;
  if (n >= 8) o.h = 8 + base;
}

function makeObj(n, base) {
  const o = {};
  initObj(o, n, base || 0);
  return o;
}

function readLast(obj, n, iters) {
  let sum = 0;
  switch (n) {
    case 1: for (let i = 0; i < iters; i++) sum += obj.a; break;
    case 4: for (let i = 0; i < iters; i++) sum += obj.d; break;
    case 5: for (let i = 0; i < iters; i++) sum += obj.e; break;
    case 8: for (let i = 0; i < iters; i++) sum += obj.h; break;
  }
  return sum;
}

function writeLast(obj, n, iters) {
  let sum = 0;
  switch (n) {
    case 1:
      for (let i = 0; i < iters; i++) { obj.a = i; sum += obj.a; }
      break;
    case 4:
      for (let i = 0; i < iters; i++) { obj.d = i; sum += obj.d; }
      break;
    case 5:
      for (let i = 0; i < iters; i++) { obj.e = i; sum += obj.e; }
      break;
    case 8:
      for (let i = 0; i < iters; i++) { obj.h = i; sum += obj.h; }
      break;
  }
  return sum;
}

function readFirst(obj, iters) {
  let sum = 0;
  for (let i = 0; i < iters; i++) sum += obj.a;
  return sum;
}

function createAndTouch(n, iters) {
  let sum = 0;
  for (let i = 0; i < iters; i++) {
    const o = {};
    initObj(o, n, i & 7);
    sum += o.a;
  }
  return sum;
}

function polyObjs5() {
  const o1 = {};
  o1.a = 1; o1.b = 2; o1.c = 3; o1.d = 4; o1.e = 5;
  const o2 = {};
  o2.b = 2; o2.a = 1; o2.c = 3; o2.d = 4; o2.e = 5;
  const o3 = {};
  o3.c = 3; o3.b = 2; o3.a = 1; o3.d = 4; o3.e = 5;
  const o4 = {};
  o4.d = 4; o4.c = 3; o4.b = 2; o4.a = 1; o4.e = 5;
  return [o1, o2, o3, o4];
}

function polyObjs8() {
  const o1 = makeObj(8, 0);
  const o2 = {};
  o2.b = 2; o2.a = 1; o2.c = 3; o2.d = 4; o2.e = 5; o2.f = 6; o2.g = 7; o2.h = 8;
  const o3 = {};
  o3.c = 3; o3.b = 2; o3.a = 1; o3.d = 4; o3.e = 5; o3.f = 6; o3.g = 7; o3.h = 8;
  const o4 = {};
  o4.d = 4; o4.c = 3; o4.b = 2; o4.a = 1; o4.e = 5; o4.f = 6; o4.g = 7; o4.h = 8;
  return [o1, o2, o3, o4];
}

function readPolyA(objs, iters) {
  let sum = 0;
  for (let i = 0; i < iters; i++) sum += objs[i & 3].a;
  return sum;
}

function runSize(n, readIters, writeIters, createIters) {
  const obj = makeObj(n, 0);
  console.log('\n=== props=' + n + ' ===');
  console.log('sanity a=' + obj.a);

  bench('read_first', iters => readFirst(obj, iters), readIters);
  bench('read_last', iters => readLast(obj, n, iters), readIters);
  bench('write_last', iters => writeLast(obj, n, iters), writeIters);
  bench('create_' + n, iters => createAndTouch(n, iters), createIters);
}

console.log('in-object own-property benchmark');
runSize(1, 4_000_000, 2_000_000, 500_000);
runSize(4, 4_000_000, 2_000_000, 500_000);
runSize(5, 4_000_000, 2_000_000, 500_000);
runSize(8, 4_000_000, 2_000_000, 500_000);

console.log('\n=== polymorphic ===');
bench('poly_read_a_5', iters => readPolyA(polyObjs5(), iters), 4_000_000);
bench('poly_read_a_8', iters => readPolyA(polyObjs8(), iters), 4_000_000);
