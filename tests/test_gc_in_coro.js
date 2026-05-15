const __gcPerfNow = () => (
  typeof performance !== 'undefined' && performance && typeof performance.now === 'function'
    ? performance.now()
    : Date.now()
);
const __gcPerfStart = __gcPerfNow();
function __gcPerfLog() {
  console.log(`[perf] runtime: ${(__gcPerfNow() - __gcPerfStart).toFixed(2)}ms`);
}

// Test: GC compaction runs correctly inside coroutines.

console.log('=== GC-in-coroutine test ===');

let passed = 0;
let failed = 0;

function assert(cond, msg) {
  if (cond) { passed++; }
  else { failed++; console.log('FAIL: ' + msg); }
}

function trace(msg) { console.log('[trace] ' + msg); }

// --- helpers ---

function allocBulk(n) {
  const arr = [];
  for (let i = 0; i < n; i++) {
    arr.push({ idx: i, data: 'item_' + i + '_' + 'x'.repeat(64) });
  }
  return arr;
}

function verifyBulk(arr, label) {
  for (let i = 0; i < arr.length; i++) {
    assert(arr[i].idx === i, label + ': idx mismatch at ' + i);
    assert(arr[i].data.startsWith('item_' + i + '_'), label + ': data mismatch at ' + i);
  }
}

// --- Test 1: pre-existing objects survive GC inside coroutine ---

trace('allocating preAlloc');
const preAlloc = allocBulk(200);
trace('preAlloc done, length=' + preAlloc.length);

async function test1() {
  trace('test1: start');
  verifyBulk(preAlloc, 'test1-preAlloc');
  trace('test1: preAlloc verified');

  const innerAlloc = allocBulk(200);
  verifyBulk(innerAlloc, 'test1-innerAlloc');
  verifyBulk(preAlloc, 'test1-preAlloc-after');
  trace('test1: done');
}

// --- Test 2: cross-coroutine references ---

const shared = { value: 'original', children: [] };

async function test2() {
  trace('test2: start');
  for (let i = 0; i < 100; i++) {
    shared.children.push({ ref: i, str: 'child_' + i });
  }

  assert(shared.value === 'original', 'test2: shared.value');
  assert(shared.children.length === 100, 'test2: children.length');
  for (let i = 0; i < 100; i++) {
    assert(shared.children[i].ref === i, 'test2: child ref ' + i);
  }
  trace('test2: done');
}

// --- Test 3: nested async (coroutine-in-coroutine) ---

async function inner(depth) {
  const obj = { depth, payload: 'nested_' + depth + '_' + 'y'.repeat(32) };
  if (depth > 0) {
    await inner(depth - 1);
  }
  assert(obj.depth === depth, 'test3: depth mismatch at ' + depth);
  assert(obj.payload.startsWith('nested_' + depth), 'test3: payload at ' + depth);
  return obj;
}

async function test3() {
  trace('test3: start');
  await inner(8);
  trace('test3: done');
}

// --- Test 4: allocation pressure inside coroutine (natural GC trigger) ---

async function test4() {
  trace('test4: start');
  const anchors = [];
  for (let wave = 0; wave < 20; wave++) {
    const batch = [];
    for (let i = 0; i < 500; i++) {
      batch.push({ wave, i, s: 'w' + wave + '_i' + i + '_' + 'z'.repeat(128) });
    }
    anchors.push(batch);
  }

  for (let wave = 0; wave < anchors.length; wave++) {
    const batch = anchors[wave];
    assert(batch.length === 500, 'test4: wave ' + wave + ' length');
    assert(batch[0].wave === wave, 'test4: wave ' + wave + ' tag');
    assert(batch[499].i === 499, 'test4: wave ' + wave + ' last idx');
  }
  trace('test4: done');
}

// --- Test 5: await + GC interleaving ---

function delay(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

async function test5() {
  trace('test5: start');
  const before = allocBulk(100);
  await delay(1);
  trace('test5: after first await');
  verifyBulk(before, 'test5-before-await');

  const after = allocBulk(100);
  await delay(1);
  trace('test5: after second await');
  verifyBulk(before, 'test5-before-final');
  verifyBulk(after, 'test5-after-final');
  trace('test5: done');
}

// --- run all ---

trace('calling main()');

async function main() {
  trace('main: start');
  await test1();
  trace('main: test1 complete');
  await test2();
  trace('main: test2 complete');
  await test3();
  trace('main: test3 complete');
  await test4();
  trace('main: test4 complete');
  await test5();
  trace('main: test5 complete');

  console.log('passed: ' + passed + ', failed: ' + failed);
  __gcPerfLog();
  if (failed > 0) {
    console.log('FAIL');
    process.exit(1);
  } else {
    console.log('OK');
  }
}

main();
trace('main() returned (promise pending)');
