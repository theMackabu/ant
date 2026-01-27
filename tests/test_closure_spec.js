// test_closure.js - Test closure scoping across async boundaries

let results = [];

// Test 1: Sync push
results.push('1. sync');

// Test 2: setTimeout with closure
setTimeout(() => {
  results.push('4. timeout 50ms');
}, 50);

setTimeout(() => {
  results.push('3. timeout 10ms');
}, 10);

// Test 3: Promise with closure
Promise.resolve('2. promise').then(v => {
  results.push(v);
});

// Test 4: queueMicrotask with closure
queueMicrotask(() => {
  results.push('2. microtask');
});

// Test 5: Nested closure
const outer = 'outer-value';
setTimeout(() => {
  const inner = 'inner-value';
  setTimeout(() => {
    results.push(`5. nested: ${outer}, ${inner}`);
  }, 10);
}, 60);

// Final timeout to print results
setTimeout(() => {
  console.log('Results length:', results.length);
  console.log('Results:');
  for (let i = 0; i < results.length; i++) {
    console.log(' ', results[i]);
  }

  // Expected order:
  // 1. sync
  // 2. promise (microtask)
  // 2. microtask (microtask)
  // 3. timeout 10ms
  // 4. timeout 50ms
  // 5. nested: outer-value, inner-value

  console.log('\nExpected 6 items, got:', results.length);
  if (results.length === 6) {
    console.log('✓ PASS: All closures captured correctly');
  } else {
    console.log('✗ FAIL: Missing items - closure bug');
  }
}, 200);
