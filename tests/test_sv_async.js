// Test basic async/await in bytecode VM

let passed = 0;
let failed = 0;

function test(name, actual, expected) {
  if (actual === expected) {
    console.log(`  ✓ ${name}`);
    passed++;
  } else {
    console.log(`  ✗ ${name}: expected ${expected}, got ${actual}`);
    failed++;
  }
}

// Test 1: async function returns a promise
async function simple() {
  return 42;
}

let p = simple();
test('async returns promise', typeof p, 'object');

// Test 2: await a resolved value
async function awaitValue() {
  let x = await 10;
  return x + 5;
}

// Test 3: await a promise
async function awaitPromise() {
  let p = new Promise((resolve) => {
    setTimeout(() => resolve(100), 10);
  });
  let val = await p;
  return val;
}

// Test 4: multiple awaits
async function multiAwait() {
  let a = await Promise.resolve(1);
  let b = await Promise.resolve(2);
  let c = await Promise.resolve(3);
  return a + b + c;
}

// Test 5: await in sequence
async function sequential() {
  let result = 0;
  for (let i = 0; i < 5; i++) {
    result += await Promise.resolve(i);
  }
  return result; // 0+1+2+3+4 = 10
}

// Test 6: async error handling
async function throwInAsync() {
  throw new Error('async error');
}

// Test 7: try/catch in async
async function tryCatchAsync() {
  try {
    await Promise.reject('rejected');
    return 'should not reach';
  } catch (e) {
    return 'caught: ' + e;
  }
}

// Run tests
console.log('Async/Await Tests\n');

simple().then(v => test('simple async return', v, 42));

awaitValue().then(v => test('await non-promise', v, 15));

awaitPromise().then(v => test('await promise', v, 100));

multiAwait().then(v => test('multiple awaits', v, 6));

sequential().then(v => test('sequential awaits', v, 10));

throwInAsync().catch(e => test('async throw rejects', e.message, 'async error'));

tryCatchAsync().then(v => test('try/catch in async', v, 'caught: rejected'));

setTimeout(() => {
  console.log(`\n${passed} passed, ${failed} failed`);
}, 200);
