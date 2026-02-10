// Stress test: GC compaction during tail-call optimization
// Verifies that tc.func, tc.closure_scope, and tc.args[] are properly
// rooted so GC compaction doesn't corrupt them mid-trampoline.

console.log('=== GC + Tail Call Stress Test ===\n');

function fmt(bytes) {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(2) + ' KB';
  return (bytes / 1024 / 1024).toFixed(2) + ' MB';
}

let failures = 0;
function assert(cond, msg) {
  if (!cond) {
    console.log('FAIL:', msg);
    failures++;
  }
}

// Inflate arena past 10MB so Ant.gc() actually compacts
function inflateArena() {
  let junk = [];
  for (let i = 0; i < 40000; i++) {
    junk.push({ idx: i, payload: 'padding_string_' + i + '_extra_data_to_bulk_up_the_arena_size' });
  }
  let used = Ant.stats().arenaUsed;
  assert(used >= 10 * 1024 * 1024, 'arena should be >= 10MB, got ' + fmt(used));
  console.log('  Arena inflated to', fmt(used));
  return junk;
}

// ---------------------------------------------------------------------------
// Test 1: Tail recursion passing heap objects as args, GC between iterations
// ---------------------------------------------------------------------------
console.log('Test 1: Tail-call args survive GC compaction');
let _garbage = inflateArena();

function tailWithObjects(n, obj) {
  if (n <= 0) return obj;
  Ant.gc();
  return tailWithObjects(n - 1, { value: obj.value + 1, prev: obj });
}

let result1 = tailWithObjects(200, { value: 0, prev: null });
assert(result1.value === 200, 'final value should be 200, got ' + result1.value);

let walk = result1;
let chainOk = true;
for (let i = 200; i >= 0; i--) {
  if (walk.value !== i) { chainOk = false; break; }
  walk = walk.prev;
}
assert(chainOk, 'object chain should be intact through all 200 links');
console.log('  Object args through tail calls: OK\n');

// ---------------------------------------------------------------------------
// Test 2: Mutual tail recursion with string args + GC pressure
// ---------------------------------------------------------------------------
console.log('Test 2: Mutual tail recursion with string args + GC');
_garbage = inflateArena();

function pingStr(n, s) {
  if (n <= 0) return s;
  Ant.gc();
  return pongStr(n - 1, s + 'p');
}
function pongStr(n, s) {
  if (n <= 0) return s;
  return pingStr(n - 1, s + 'o');
}

let result2 = pingStr(100, 'start:');
assert(result2.length === 106, 'string length should be 106, got ' + result2.length);
assert(result2.startsWith('start:'), 'should start with "start:"');
console.log('  Mutual tail recursion with strings: OK\n');

// ---------------------------------------------------------------------------
// Test 3: Closure scope survives GC during tail calls
// ---------------------------------------------------------------------------
console.log('Test 3: Closure scope survives GC during tail calls');
_garbage = inflateArena();

function makeAccumulator() {
  let captured = { sum: 0 };
  function loop(n) {
    if (n <= 0) return captured;
    captured.sum += n;
    Ant.gc();
    return loop(n - 1);
  }
  return loop;
}

let accResult = makeAccumulator()(500);
assert(accResult.sum === 500 * 501 / 2, 'sum should be 125250, got ' + accResult.sum);
console.log('  Closure scope through tail calls: OK\n');

// ---------------------------------------------------------------------------
// Test 4: Multi-arg tail calls with mixed heap types + GC
// ---------------------------------------------------------------------------
console.log('Test 4: Multi-arg tail calls with mixed types + GC');
_garbage = inflateArena();

function multiArg(n, arr, obj, str) {
  if (n <= 0) return { arr, obj, str };
  arr.push(n);
  Ant.gc();
  return multiArg(n - 1, arr, { v: obj.v + 1, inner: obj }, str + 'x');
}

let result4 = multiArg(100, [], { v: 0, inner: null }, '');
assert(result4.arr.length === 100, 'array should have 100 elements, got ' + result4.arr.length);
assert(result4.obj.v === 100, 'nested obj.v should be 100, got ' + result4.obj.v);
assert(result4.str.length === 100, 'string should have 100 chars, got ' + result4.str.length);
console.log('  Multi-arg mixed types: OK\n');

// ---------------------------------------------------------------------------
// Test 5: Deep tail recursion with GC every N iterations
// ---------------------------------------------------------------------------
console.log('Test 5: Deep tail recursion (50k) with periodic GC');
_garbage = inflateArena();

function deepTail(n, acc) {
  if (n <= 0) return acc;
  if (n % 500 === 0) Ant.gc();
  return deepTail(n - 1, acc + 1);
}

let result5 = deepTail(50000, 0);
assert(result5 === 50000, 'deepTail should return 50000, got ' + result5);
console.log('  Deep tail recursion with periodic GC: OK\n');

// ---------------------------------------------------------------------------
// Test 6: Tail call where callee is a different function (not self-recursion)
// ---------------------------------------------------------------------------
console.log('Test 6: Tail call to different functions + GC');
_garbage = inflateArena();

function step1(n, data) {
  if (n <= 0) return data;
  data.a++;
  Ant.gc();
  return step2(n - 1, data);
}
function step2(n, data) {
  if (n <= 0) return data;
  data.b++;
  return step3(n - 1, data);
}
function step3(n, data) {
  if (n <= 0) return data;
  data.c++;
  return step1(n - 1, data);
}

let result6 = step1(300, { a: 0, b: 0, c: 0 });
assert(result6.a === 100, 'a should be 100, got ' + result6.a);
assert(result6.b === 100, 'b should be 100, got ' + result6.b);
assert(result6.c === 100, 'c should be 100, got ' + result6.c);
console.log('  Cross-function tail calls: OK\n');

// ---------------------------------------------------------------------------
// Test 7: Array args allocated fresh each iteration + GC
// ---------------------------------------------------------------------------
console.log('Test 7: Fresh array allocation per tail-call iteration + GC');
_garbage = inflateArena();

function freshArrays(n, results) {
  if (n <= 0) return results;
  let data = new Array(50);
  for (let i = 0; i < 50; i++) data[i] = n;
  results.push(data);
  if (n % 10 === 0) Ant.gc();
  return freshArrays(n - 1, results);
}

let result7 = freshArrays(100, []);
assert(result7.length === 100, 'should have 100 arrays, got ' + result7.length);
assert(result7[0][0] === 100, 'first array should contain 100');
assert(result7[99][0] === 1, 'last array should contain 1');
console.log('  Fresh array per iteration: OK\n');

// ---------------------------------------------------------------------------
// Summary
// ---------------------------------------------------------------------------
_garbage = null;

console.log('=== Summary ===');
if (failures === 0) {
  console.log('All GC + TCO stress tests passed!');
} else {
  console.log('Failures:', failures);
}
