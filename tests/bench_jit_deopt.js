// Benchmark: JIT deoptimization & recompilation
//
// Scenarios:
//   1. Single bailout → recompile → regain JIT speed
//   2. Repeated bailouts → permanent ban
//   3. No bailout baseline (pure numeric hot loop)
//   4. Periodic type instability across rounds
//   5. Correctness across all JIT states
//   6. Multi-op arithmetic chain (ADD/SUB/MUL/DIV) bailout recovery
//   7. Large hot loop — JIT vs interpreter throughput
//   8. Rapid-fire bailouts (many in quick succession)
//   9. Comparison operators bailout recovery
//  10. Closure with upvalue — bailout and recovery

var get_clock = typeof performance !== 'undefined' ? performance.now : Date.now;
var JIT_THRESHOLD = 100;
var RECOMPILE_DELAY = 50;
var ITER = 500000;

function timeit(fn, label) {
  var best = Infinity;
  for (var t = 0; t < 3; t++) {
    var t0 = get_clock();
    var result = fn();
    var elapsed = get_clock() - t0;
    if (elapsed < best) best = elapsed;
  }
  console.log('  ' + label + ': ' + best.toFixed(2) + ' ms');
  return best;
}

function warmup(fn, n) {
  for (var i = 0; i < (n || JIT_THRESHOLD + 10); i++) fn(i, i + 1);
}

// ============================================================
// Scenario 1: Single bailout then recompile
// ============================================================
console.log('--- S1: single bailout + recompile ---');

function addS1(a, b) { return a + b; }
warmup(addS1);

var s1_before = timeit(function() {
  var s = 0;
  for (var i = 0; i < ITER; i++) s = addS1(s, 1);
  return s;
}, 'before bailout');

addS1('bail', 'out');
warmup(addS1, RECOMPILE_DELAY + 10);

var s1_after = timeit(function() {
  var s = 0;
  for (var i = 0; i < ITER; i++) s = addS1(s, 1);
  return s;
}, 'after recompile');

// ============================================================
// Scenario 2: Exhaust deopts → permanent ban
// ============================================================
console.log('');
console.log('--- S2: permanent ban ---');

function addS2(a, b) { return a + b; }

for (var deopt = 0; deopt < 4; deopt++) {
  warmup(addS2);
  addS2('x' + deopt, 'y');
  warmup(addS2, RECOMPILE_DELAY + 10);
}
// should be banned now regardless of strategy
for (var i = 0; i < JIT_THRESHOLD * 3; i++) addS2(i, i + 1);

var s2 = timeit(function() {
  var s = 0;
  for (var i = 0; i < ITER; i++) s = addS2(s, 1);
  return s;
}, 'banned (interpreter)');

// ============================================================
// Scenario 3: Pure numeric baseline
// ============================================================
console.log('');
console.log('--- S3: pure numeric baseline ---');

function addS3(a, b) { return a + b; }
warmup(addS3);

var s3 = timeit(function() {
  var s = 0;
  for (var i = 0; i < ITER; i++) s = addS3(s, 1);
  return s;
}, 'JIT baseline');

// ============================================================
// Scenario 4: Periodic type instability
// ============================================================
console.log('');
console.log('--- S4: periodic type instability ---');

function addS4(a, b) { return a + b; }

var s4_times = [];
for (var round = 0; round < 8; round++) {
  warmup(addS4);
  var t = timeit(function() {
    var s = 0;
    for (var i = 0; i < ITER; i++) s = addS4(s, 1);
    return s;
  }, 'round ' + round);
  s4_times.push(t);
  addS4('round', String(round));
  warmup(addS4, RECOMPILE_DELAY + 10);
}

// ============================================================
// Scenario 5: Correctness across all JIT states
// ============================================================
console.log('');
console.log('--- S5: correctness ---');

function compute(a, b) { return a + b; }
var allCorrect = true;

warmup(compute);
if (compute(20, 22) !== 42) { allCorrect = false; console.log('  FAIL: numeric JIT'); }
if (compute('hello', ' world') !== 'hello world') { allCorrect = false; console.log('  FAIL: string bailout'); }
if (compute(100, -58) !== 42) { allCorrect = false; console.log('  FAIL: numeric post-bailout'); }
warmup(compute, RECOMPILE_DELAY + 10);
if (compute(20, 22) !== 42) { allCorrect = false; console.log('  FAIL: numeric recompile'); }
if (compute('foo', 'bar') !== 'foobar') { allCorrect = false; console.log('  FAIL: string bailout 2'); }
warmup(compute, RECOMPILE_DELAY + 10);
if (compute(1, 1) !== 2) { allCorrect = false; console.log('  FAIL: numeric recompile 2'); }
if (compute('a', 'b') !== 'ab') { allCorrect = false; console.log('  FAIL: string bailout 3'); }
for (var i = 0; i < 500; i++) compute(i, 1);
if (compute(20, 22) !== 42) { allCorrect = false; console.log('  FAIL: numeric after ban'); }
console.log('  all correct: ' + allCorrect);

// ============================================================
// Scenario 6: Multi-op arithmetic chain — bailout recovery
// ============================================================
console.log('');
console.log('--- S6: arithmetic chain bailout ---');

function mathChain(a, b, c) {
  var x = a + b;
  var y = x * c;
  var z = y - a;
  return z / b;
}

for (var i = 1; i < JIT_THRESHOLD + 10; i++) mathChain(i, i + 1, 2);

var s6_before = timeit(function() {
  var s = 0;
  for (var i = 1; i <= ITER; i++) s = mathChain(i, i + 1, 2);
  return s;
}, 'chain before bailout');

// bailout: string in numeric chain triggers coercion
mathChain('10', 5, 3);

for (var i = 1; i < RECOMPILE_DELAY + 10; i++) mathChain(i, i + 1, 2);

var s6_after = timeit(function() {
  var s = 0;
  for (var i = 1; i <= ITER; i++) s = mathChain(i, i + 1, 2);
  return s;
}, 'chain after recompile');

// ============================================================
// Scenario 7: Large hot loop — throughput comparison
// ============================================================
console.log('');
console.log('--- S7: large hot loop throughput ---');

function hotSum(n) {
  var sum = 0;
  for (var i = 0; i < n; i++) sum = sum + i;
  return sum;
}

warmup(function(i) { hotSum(10); return i; });

var s7_jit = timeit(function() { return hotSum(1000000); }, 'JIT 1M loop');

// bailout
hotSum('not a number');
warmup(function(i) { hotSum(10); return i; }, RECOMPILE_DELAY + 10);

var s7_recover = timeit(function() { return hotSum(1000000); }, 'after bailout 1M');

// ============================================================
// Scenario 8: Rapid-fire bailouts
// ============================================================
console.log('');
console.log('--- S8: rapid-fire bailouts ---');

function addRapid(a, b) { return a + b; }

warmup(addRapid);

// fire 10 bailouts in quick succession, re-warming each time
for (var k = 0; k < 10; k++) {
  addRapid('rapid' + k, '!');
  warmup(addRapid, RECOMPILE_DELAY + 10);
}

var s8 = timeit(function() {
  var s = 0;
  for (var i = 0; i < ITER; i++) s = addRapid(s, 1);
  return s;
}, 'after 10 rapid bailouts');

// ============================================================
// Scenario 9: Comparison operators
// ============================================================
console.log('');
console.log('--- S9: comparison bailout ---');

function ltCmp(a, b) { return a < b; }

for (var i = 0; i < JIT_THRESHOLD + 10; i++) ltCmp(i, 100);

var s9_before = timeit(function() {
  var c = 0;
  for (var i = 0; i < ITER; i++) { if (ltCmp(i, ITER / 2)) c++; }
  return c;
}, 'LT before bailout');

ltCmp('abc', 'def');
for (var i = 0; i < RECOMPILE_DELAY + 10; i++) ltCmp(i, 100);

var s9_after = timeit(function() {
  var c = 0;
  for (var i = 0; i < ITER; i++) { if (ltCmp(i, ITER / 2)) c++; }
  return c;
}, 'LT after recompile');

// ============================================================
// Scenario 10: Closure with upvalue
// ============================================================
console.log('');
console.log('--- S10: closure upvalue bailout ---');

function makeAccum(init) {
  var total = init;
  return function(v) { total = total + v; return total; };
}

var accum = makeAccum(0);
for (var i = 0; i < JIT_THRESHOLD + 10; i++) accum(1);

var s10_before = timeit(function() {
  var a = makeAccum(0);
  for (var i = 0; i < JIT_THRESHOLD + 10; i++) a(1); // re-JIT the inner fn
  for (var i = 0; i < ITER; i++) a(1);
  return a(0);
}, 'closure before bailout');

// bailout: string into numeric accumulator
var accum2 = makeAccum(0);
for (var i = 0; i < JIT_THRESHOLD + 10; i++) accum2(1);
accum2(' world');
for (var i = 0; i < RECOMPILE_DELAY + 10; i++) accum2(1);

var s10_after = timeit(function() {
  var a = makeAccum(0);
  for (var i = 0; i < JIT_THRESHOLD + 10; i++) a(1);
  for (var i = 0; i < ITER; i++) a(1);
  return a(0);
}, 'closure after recompile');

// ============================================================
// Summary
// ============================================================
console.log('');
console.log('=== SUMMARY ===');
console.log('  S1 recovery: ' + (s1_after / s1_before * 100).toFixed(0) + '% of JIT speed');
console.log('  S3 baseline: ' + s3.toFixed(2) + ' ms');
console.log('  S2 banned:   ' + s2.toFixed(2) + ' ms (' + (s2 / s3).toFixed(1) + 'x slower than JIT)');
console.log('  S6 chain recovery: ' + (s6_after / s6_before * 100).toFixed(0) + '%');
console.log('  S7 loop recovery:  ' + (s7_recover / s7_jit * 100).toFixed(0) + '%');
console.log('  S9 cmp recovery:   ' + (s9_after / s9_before * 100).toFixed(0) + '%');
console.log('  S5 correctness: ' + allCorrect);
