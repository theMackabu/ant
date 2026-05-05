function nowMs() {
  if (typeof performance !== 'undefined' && performance && typeof performance.now === 'function') {
    return performance.now();
  }
  return Date.now();
}

function parseScale() {
  if (typeof process === 'undefined' || !process || !process.argv) return 1;
  const raw = Number(process.argv[2]);
  return Number.isFinite(raw) && raw > 0 ? raw : 1;
}

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
    const value = out[i];
    let j = i - 1;
    while (j >= 0 && out[j] > value) {
      out[j + 1] = out[j];
      j--;
    }
    out[j + 1] = value;
  }
  return out;
}

const SCALE = parseScale();
const WARMUP_RUNS = 2;
const SAMPLE_RUNS = 7;
let sink = 0;

function scaledIters(base) {
  return Math.max(1, Math.floor(base * SCALE));
}

function bench(label, fn, iters) {
  const warmupIters = Math.max(1, Math.min(iters, Math.max(1000, (iters / 4) | 0)));
  for (let i = 0; i < WARMUP_RUNS; i++) sink ^= fn(warmupIters) | 0;

  let result = 0;
  const samples = [];
  for (let i = 0; i < SAMPLE_RUNS; i++) {
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

  sink ^= result | 0;
  console.log(
    label +
      ': median ' + med.toFixed(2) + 'ms (' + opsPerMs + ' ops/ms)' +
      ', p95 ' + p95.toFixed(2) + 'ms' +
      ', min ' + min.toFixed(2) + 'ms' +
      ', max ' + max.toFixed(2) + 'ms' +
      ' result=' + result
  );
}

function makePlainStringObject() {
  return {
    alpha: 1,
    beta: 2,
    gamma: 3,
    delta: 4,
    epsilon: 5,
    zeta: 6,
    eta: 7,
    theta: 8,
  };
}

function makeMixedIndexObject() {
  const obj = {
    2: true,
    0: true,
    1: true,
    ' ': true,
    9: true,
    D: true,
    B: true,
    '-1': true,
  };
  obj.A = true;
  obj[3] = true;
  'EFGHIJKLMNOPQRSTUVWXYZ'.split('').forEach(key => obj[key] = true);
  Object.defineProperty(obj, 'C', { value: true, enumerable: true });
  Object.defineProperty(obj, '4', { value: true, enumerable: true });
  delete obj[2];
  obj[2] = true;
  return obj;
}

const sym1 = Symbol('one');
const sym2 = Symbol('two');
const sym3 = Symbol('three');

function makeSymbolObject() {
  const obj = { 1: true, A: true };
  obj.B = true;
  obj[sym1] = true;
  obj[2] = true;
  obj[sym2] = true;
  Object.defineProperty(obj, 'C', { value: true, enumerable: true });
  Object.defineProperty(obj, sym3, { value: true, enumerable: true });
  Object.defineProperty(obj, 'D', { value: true, enumerable: true });
  return obj;
}

const plainObj = makePlainStringObject();
const mixedObj = makeMixedIndexObject();
const symbolObj = makeSymbolObject();

function consumeKeys(keys) {
  let total = keys.length;
  for (let i = 0; i < keys.length; i++) {
    const key = keys[i];
    total += typeof key === 'symbol' ? 17 : key.length;
  }
  return total;
}

function benchGetOwnPropertyNamesPlain(n) {
  let total = 0;
  for (let i = 0; i < n; i++) total += consumeKeys(Object.getOwnPropertyNames(plainObj));
  return total;
}

function benchGetOwnPropertyNamesMixed(n) {
  let total = 0;
  for (let i = 0; i < n; i++) total += consumeKeys(Object.getOwnPropertyNames(mixedObj));
  return total;
}

function benchReflectOwnKeysPlain(n) {
  let total = 0;
  for (let i = 0; i < n; i++) total += consumeKeys(Reflect.ownKeys(plainObj));
  return total;
}

function benchReflectOwnKeysMixed(n) {
  let total = 0;
  for (let i = 0; i < n; i++) total += consumeKeys(Reflect.ownKeys(symbolObj));
  return total;
}

function benchObjectAssignPlain(n) {
  let total = 0;
  for (let i = 0; i < n; i++) {
    const out = Object.assign({}, plainObj);
    total += out.alpha + out.theta;
  }
  return total;
}

function benchObjectAssignMixed(n) {
  let total = 0;
  for (let i = 0; i < n; i++) {
    const out = Object.assign({}, mixedObj);
    total += out[0] === true ? 1 : 0;
    total += out.C === true ? 1 : 0;
  }
  return total;
}

function benchForInPlain(n) {
  let total = 0;
  for (let i = 0; i < n; i++) {
    for (const key in plainObj) total += key.length;
  }
  return total;
}

function benchForInMixed(n) {
  let total = 0;
  for (let i = 0; i < n; i++) {
    for (const key in mixedObj) total += key.length;
  }
  return total;
}

function benchJsonStringifyPlain(n) {
  let total = 0;
  for (let i = 0; i < n; i++) total += JSON.stringify(plainObj).length;
  return total;
}

function benchJsonStringifyMixed(n) {
  let total = 0;
  for (let i = 0; i < n; i++) total += JSON.stringify(mixedObj).length;
  return total;
}

console.log('own-property order benchmark');
console.log('scale=' + SCALE);

bench('Object.getOwnPropertyNames plain', benchGetOwnPropertyNamesPlain, scaledIters(200000));
bench('Object.getOwnPropertyNames mixed-index', benchGetOwnPropertyNamesMixed, scaledIters(100000));
bench('Reflect.ownKeys plain', benchReflectOwnKeysPlain, scaledIters(200000));
bench('Reflect.ownKeys mixed-symbol', benchReflectOwnKeysMixed, scaledIters(100000));
bench('Object.assign plain', benchObjectAssignPlain, scaledIters(100000));
bench('Object.assign mixed-index', benchObjectAssignMixed, scaledIters(50000));
bench('for-in plain', benchForInPlain, scaledIters(100000));
bench('for-in mixed-index', benchForInMixed, scaledIters(50000));
bench('JSON.stringify plain', benchJsonStringifyPlain, scaledIters(50000));
bench('JSON.stringify mixed-index', benchJsonStringifyMixed, scaledIters(20000));

console.log('sink=' + sink);
