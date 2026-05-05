function nowMs() {
  if (typeof performance !== 'undefined' && performance && typeof performance.now === 'function') {
    return performance.now();
  }
  return Date.now();
}

function parseScale() {
  if (typeof process === 'undefined' || !process || !process.argv) return 1;
  var raw = Number(process.argv[2]);
  return Number.isFinite(raw) && raw > 0 ? raw : 1;
}

function percentile(sorted, p) {
  if (sorted.length === 0) return 0;
  if (sorted.length === 1) return sorted[0];
  var pos = (sorted.length - 1) * p;
  var base = Math.floor(pos);
  var frac = pos - base;
  var next = sorted[base + 1];
  if (next === undefined) return sorted[base];
  return sorted[base] + (next - sorted[base]) * frac;
}

function stableNumericSort(values) {
  var out = values.slice();
  for (var i = 1; i < out.length; i++) {
    var value = out[i];
    var j = i - 1;
    while (j >= 0 && out[j] > value) {
      out[j + 1] = out[j];
      j--;
    }
    out[j + 1] = value;
  }
  return out;
}

var SCALE = parseScale();
var WARMUP_RUNS = 2;
var SAMPLE_RUNS = 7;
var sink = 0;

function scaledIters(base) {
  return Math.max(1, Math.floor(base * SCALE));
}

function bench(label, fn, iters) {
  var warmupIters = Math.max(1, Math.min(iters, Math.max(1000, (iters / 4) | 0)));
  for (var i = 0; i < WARMUP_RUNS; i++) sink ^= fn(warmupIters) | 0;

  var result = 0;
  var samples = [];
  for (var j = 0; j < SAMPLE_RUNS; j++) {
    var t0 = nowMs();
    var r = fn(iters);
    var dt = nowMs() - t0;
    if (j === 0) result = r;
    samples.push(dt);
  }

  var sorted = stableNumericSort(samples);
  var min = sorted[0];
  var med = percentile(sorted, 0.5);
  var p95 = percentile(sorted, 0.95);
  var max = sorted[sorted.length - 1];
  var opsPerMs = med > 0 ? (iters / med).toFixed(2) : 'inf';

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

function makePlainObject() {
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
  var obj = {
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
  'EFGHIJKLMNOPQRSTUVWXYZ'.split('').forEach(function (key) {
    obj[key] = true;
  });
  Object.defineProperty(obj, 'C', { value: true, enumerable: true });
  Object.defineProperty(obj, '4', { value: true, enumerable: true });
  delete obj[2];
  obj[2] = true;
  return obj;
}

function makeShadowObject() {
  var proto = {};
  var names = 'ABCDEFGHIJKLMNOPQRST'.split('');
  for (var i = 0; i < names.length; i++) proto[names[i]] = i;

  var obj = Object.create(proto);
  obj.keepA = 1;
  obj.keepB = 2;
  obj.keepC = 3;
  for (var j = 0; j < names.length; j++) {
    Object.defineProperty(obj, names[j], {
      value: j,
      enumerable: false,
      configurable: true,
    });
  }
  return obj;
}

var plainObj = makePlainObject();
var mixedObj = makeMixedIndexObject();
var shadowObj = makeShadowObject();

function consumeKeys(keys) {
  var total = keys.length;
  for (var i = 0; i < keys.length; i++) total += keys[i].length;
  return total;
}

function benchObjectKeysPlain(n) {
  var total = 0;
  for (var i = 0; i < n; i++) total += consumeKeys(Object.keys(plainObj));
  return total;
}

function benchObjectKeysMixed(n) {
  var total = 0;
  for (var i = 0; i < n; i++) total += consumeKeys(Object.keys(mixedObj));
  return total;
}

function benchForInPlain(n) {
  var total = 0;
  for (var i = 0; i < n; i++) {
    for (var key in plainObj) total += key.length;
  }
  return total;
}

function benchForInMixed(n) {
  var total = 0;
  for (var i = 0; i < n; i++) {
    for (var key in mixedObj) total += key.length;
  }
  return total;
}

function benchForInShadow(n) {
  var total = 0;
  for (var i = 0; i < n; i++) {
    for (var key in shadowObj) total += key.length;
  }
  return total;
}

function benchForInFunctionShadow(n) {
  var total = 0;
  Object.prototype.length = 42;
  for (var i = 0; i < n; i++) {
    for (var key in Function) total += key.length;
  }
  delete Object.prototype.length;
  return total;
}

console.log('for-in and Object.keys benchmark');
console.log('scale=' + SCALE);

bench('Object.keys plain', benchObjectKeysPlain, scaledIters(200000));
bench('Object.keys mixed-index', benchObjectKeysMixed, scaledIters(100000));
bench('for-in plain', benchForInPlain, scaledIters(100000));
bench('for-in mixed-index', benchForInMixed, scaledIters(50000));
bench('for-in nonenum-shadow', benchForInShadow, scaledIters(50000));
bench('for-in Function shadow', benchForInFunctionShadow, scaledIters(50000));

console.log('sink=' + sink);
