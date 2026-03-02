// Test: reactor coroutine resume iteration safety.
//
// Verifies that the reactor loop handles coroutine queue mutations during
// execution correctly:
//   1. Coroutine A resolves a promise that wakes coroutine B — B may complete
//      and be freed while A is still in the queue.  The reactor must not
//      follow a dangling next pointer.
//   2. Many concurrent coroutines settling each other in chains.
//   3. A coroutine that spawns new coroutines during execution.

console.log('=== coro resume safety ===');

let passed = 0;
let failed = 0;
function assert(cond, msg) {
  if (cond) passed++;
  else { failed++; console.log('FAIL: ' + msg); }
}

// --- Test 1: A resolves B's promise, B completes during A's execution ---

async function test1() {
  let resolveB;
  const promiseB = new Promise(r => { resolveB = r; });

  const results = [];

  async function coroB() {
    const val = await promiseB;
    results.push('B:' + val);
  }

  async function coroA() {
    resolveB('from-A');
    // After resolving, B becomes ready and may run+complete
    // before A finishes this tick
    results.push('A:done');
  }

  // Start both — B will suspend on promiseB, A will resolve it
  const bDone = coroB();
  const aDone = coroA();
  await aDone;
  await bDone;

  assert(results.includes('A:done'), 'test1: A completed');
  assert(results.includes('B:from-A'), 'test1: B got value from A');
  console.log('test1: ok');
}

// --- Test 2: chain of coroutines settling each other ---

async function test2() {
  const N = 20;
  const resolvers = [];
  const gates = [];
  const results = [];

  // Create all promises upfront so resolvers exist before chain[0] runs
  for (let i = 0; i < N; i++) {
    gates.push(new Promise(r => { resolvers.push(r); }));
  }

  async function chainCoro(idx) {
    if (idx > 0) {
      const val = await gates[idx];
      results.push(idx + ':' + val);
    }
    if (idx + 1 < N) {
      resolvers[idx + 1]('from-' + idx);
    }
  }

  const promises = [];
  for (let i = 0; i < N; i++) {
    promises.push(chainCoro(i));
  }

  await Promise.all(promises);

  assert(results.length === N - 1, 'test2: all chain links settled (' + results.length + '/' + (N-1) + ')');
  console.log('test2: ok');
}

// --- Test 3: coroutine spawns new coroutines during execution ---

async function test3() {
  const results = [];

  async function spawner() {
    const children = [];
    for (let i = 0; i < 10; i++) {
      children.push((async () => {
        // Small delay to ensure we go through the event loop
        await new Promise(r => setTimeout(r, 1));
        results.push('child-' + i);
      })());
    }
    results.push('spawner-done');
    await Promise.all(children);
  }

  await spawner();
  assert(results.includes('spawner-done'), 'test3: spawner completed');
  assert(results.length === 11, 'test3: all children completed (' + results.length + '/11)');
  console.log('test3: ok');
}

// --- Test 4: mutual resolution — A and B settle each other's promises ---

async function test4() {
  let resolveForA, resolveForB;
  const promiseForA = new Promise(r => { resolveForA = r; });
  const promiseForB = new Promise(r => { resolveForB = r; });

  const results = [];

  async function coroA() {
    resolveForB('hello-from-A');
    const val = await promiseForA;
    results.push('A-got:' + val);
  }

  async function coroB() {
    resolveForA('hello-from-B');
    const val = await promiseForB;
    results.push('B-got:' + val);
  }

  await Promise.all([coroA(), coroB()]);

  assert(results.includes('A-got:hello-from-B'), 'test4: A got B\'s value');
  assert(results.includes('B-got:hello-from-A'), 'test4: B got A\'s value');
  console.log('test4: ok');
}

// --- run ---

async function main() {
  await test1();
  await test2();
  await test3();
  await test4();

  console.log('passed: ' + passed + ', failed: ' + failed);
  if (failed > 0) { console.log('FAIL'); process.exit(1); }
  else console.log('OK');
}

main();
