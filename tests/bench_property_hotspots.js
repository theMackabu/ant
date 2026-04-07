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

const SCALE = parseScale();
const BENCH_WARMUP_RUNS = 2;
const BENCH_SAMPLE_RUNS = 7;
let benchSink = 0;

function scaledIters(base) {
  return Math.max(1, Math.floor(base * SCALE));
}

function section(title) {
  console.log('\n=== ' + title + ' ===');
}

function bench(label, fn, iters) {
  const warmupIters = Math.max(1, Math.min(iters, Math.max(10_000, (iters / 4) | 0)));
  for (let i = 0; i < BENCH_WARMUP_RUNS; i++) benchSink ^= fn(warmupIters) | 0;

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

  benchSink ^= result | 0;
  console.log(
    label +
      ': median ' + med.toFixed(2) + 'ms (' + opsPerMs + ' ops/ms)' +
      ', p95 ' + p95.toFixed(2) + 'ms' +
      ', min ' + min.toFixed(2) + 'ms' +
      ', max ' + max.toFixed(2) + 'ms' +
      ' result=' + result
  );
}

function benchCfuncProtoCallApply(n) {
  const fn = parseInt;
  let sum = 0;
  for (let i = 0; i < n; i++) {
    if (fn.call) sum++;
    if (fn.apply) sum++;
  }
  return sum;
}

function benchCfuncProtoBind(n) {
  const fn = parseInt;
  let sum = 0;
  for (let i = 0; i < n; i++) {
    if (fn.bind) sum++;
  }
  return sum;
}

const DATA_DESC = {
  value: 1,
  writable: true,
  enumerable: true,
  configurable: true,
};

function benchDefinePropertyFresh(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    const obj = {};
    Object.defineProperty(obj, 'x', DATA_DESC);
    sum += obj.x;
  }
  return sum;
}

const ACCESSOR_DESC = {
  get() { return 1; },
  set(v) { benchSink ^= v | 0; },
  enumerable: true,
  configurable: true,
};

function benchDefineAccessorFresh(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    const obj = {};
    Object.defineProperty(obj, 'x', ACCESSOR_DESC);
    sum += obj.x;
  }
  return sum;
}

function benchWithGet(n) {
  const scopeObj = { x: 1 };
  let sum = 0;
  with (scopeObj) {
    for (let i = 0; i < n; i++) sum += x;
  }
  return sum;
}

function makeReusableRangeIterable(limit) {
  return {
    [Symbol.iterator]() {
      let i = 0;
      const result = { done: false, value: 0 };
      return {
        next() {
          if (i < limit) {
            result.done = false;
            result.value = i++;
          } else {
            result.done = true;
            result.value = undefined;
          }
          return result;
        }
      };
    }
  };
}

function benchForOfReusableResult(n) {
  let sum = 0;
  for (const v of makeReusableRangeIterable(n)) sum += v;
  return sum;
}

function benchArrayLiteralCreate(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    const arr = [];
    sum += arr.length;
  }
  return sum;
}

function benchObjectLiteralCreate(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    const obj = {};
    if (obj) sum++;
  }
  return sum;
}

function benchStringProtoDotRead(n) {
  const str = '  hello  ';
  let sum = 0;
  for (let i = 0; i < n; i++) {
    if (str.trim) sum++;
    if (str.toUpperCase) sum++;
  }
  return sum;
}

function benchNumberProtoDotRead(n) {
  const num = 123.456;
  let sum = 0;
  for (let i = 0; i < n; i++) {
    if (num.toFixed) sum++;
    if (num.toString) sum++;
  }
  return sum;
}

function benchComputedMissingShortKey(n) {
  const obj = {};
  const key = 'missing';
  let sum = 0;
  for (let i = 0; i < n; i++) {
    if (obj[key] === undefined) sum++;
  }
  return sum;
}

function benchComputedMissingLongKey(n) {
  const obj = {};
  const key = 'missing_property_name_that_is_long_enough_to_force_a_heap_alloc_in_sv_getprop_fallback_len_0123456789';
  let sum = 0;
  for (let i = 0; i < n; i++) {
    if (obj[key] === undefined) sum++;
  }
  return sum;
}

console.log('property hotspot benchmark');
console.log('scale=' + SCALE);

section('1. lkp_proto T_CFUNC branch re-interns');
console.log('sanity parseInt.call=' + (typeof parseInt.call));
bench('cfunc_proto_call_apply', benchCfuncProtoCallApply, scaledIters(2_000_000));
bench('cfunc_proto_bind', benchCfuncProtoBind, scaledIters(1_500_000));

section('2. ensure_string_shape_slot miss path');
console.log('sanity defineProperty=' + (typeof Object.defineProperty));
bench('defineProperty_fresh_data', benchDefinePropertyFresh, scaledIters(300_000));
bench('defineProperty_fresh_accessor', benchDefineAccessorFresh, scaledIters(200_000));

section('3. with lookup existence-check plus re-fetch');
console.log('sanity with=' + benchWithGet(3));
bench('with_get_var', benchWithGet, scaledIters(1_500_000));

section('4. iterator done/value generic reads');
console.log('sanity for_of=' + benchForOfReusableResult(8));
bench('for_of_reusable_result', benchForOfReusableResult, scaledIters(600_000));

section('5. ctor.prototype lookup on allocation');
console.log('sanity literals=' + (benchArrayLiteralCreate(3) + benchObjectLiteralCreate(3)));
bench('array_literal_create', benchArrayLiteralCreate, scaledIters(2_000_000));
bench('object_literal_create', benchObjectLiteralCreate, scaledIters(2_000_000));

section('6. primitive proto reads through generic path');
console.log('sanity primitive=' + (benchStringProtoDotRead(2) + benchNumberProtoDotRead(2)));
bench('string_proto_dot_read', benchStringProtoDotRead, scaledIters(2_000_000));
bench('number_proto_dot_read', benchNumberProtoDotRead, scaledIters(2_000_000));

section('7. computed missing-key reads through sv_getprop_fallback_len');
console.log('sanity computed=' + (benchComputedMissingShortKey(2) + benchComputedMissingLongKey(2)));
bench('computed_missing_short_key', benchComputedMissingShortKey, scaledIters(2_000_000));
bench('computed_missing_long_key', benchComputedMissingLongKey, scaledIters(1_000_000));

console.log('\nbenchSink=' + benchSink);
