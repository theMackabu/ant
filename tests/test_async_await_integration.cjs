// Integration test for async/await functionality
console.log('=== Async/Await Integration Tests ===');

// Test 1: Async function that returns a value, consumed with await
console.log('\nTest 1: Async function consumed with await');
async function getData() {
  return 'data from async';
}

async function consumeData() {
  const data = await getData();
  console.log('Consumed: ' + data);
  return data;
}

consumeData().then(v => console.log('Integration Test 1: ' + v));

// Test 2: Chain of async/await functions
console.log('\nTest 2: Chain of async/await');
async function step1() {
  const val = await Promise.resolve(10);
  return val * 2;
}

async function step2() {
  const val = await step1();
  return val + 5;
}

async function step3() {
  const val = await step2();
  return val * 3;
}

step3().then(v => console.log('Integration Test 2: ' + v));

// Test 3: Await with Promise.resolve and transformation
console.log('\nTest 3: Await with transformations');
async function transform() {
  const a = await Promise.resolve(5);
  const b = await Promise.resolve(10);
  const c = await Promise.resolve(a + b);
  return c * 2;
}

transform().then(v => console.log('Integration Test 3: ' + v));

// Test 4: Async arrow with await
console.log('\nTest 4: Async arrow with await');
const asyncArrow = async (x) => {
  const doubled = await Promise.resolve(x * 2);
  return doubled + 10;
};

asyncArrow(5).then(v => console.log('Integration Test 4: ' + v));

// Test 5: Nested async calls with await
console.log('\nTest 5: Nested async with await');
async function inner() {
  return await Promise.resolve('inner value');
}

async function middle() {
  const val = await inner();
  return 'middle wraps: ' + val;
}

async function outer() {
  const val = await middle();
  return 'outer wraps: ' + val;
}

outer().then(v => console.log('Integration Test 5: ' + v));

// Test 6: Await in conditional branches
console.log('\nTest 6: Conditional await');
async function conditionalAwait(flag) {
  if (flag) {
    const val = await Promise.resolve(100);
    return val;
  } else {
    const val = await Promise.resolve(200);
    return val;
  }
}

conditionalAwait(true).then(v => console.log('Integration Test 6a: ' + v));
conditionalAwait(false).then(v => console.log('Integration Test 6b: ' + v));

// Test 7: Multiple awaits with operations
console.log('\nTest 7: Multiple awaits with math');
async function calculate() {
  const x = await Promise.resolve(3);
  const y = await Promise.resolve(4);
  const z = await Promise.resolve(5);
  return x * x + y * y;
}

calculate().then(v => console.log('Integration Test 7: ' + v));

// Test 8: Await with object manipulation
console.log('\nTest 8: Await with objects');
async function objectTest() {
  const obj = await Promise.resolve({ name: 'test', value: 42 });
  obj.value = obj.value * 2;
  return obj.value;
}

objectTest().then(v => console.log('Integration Test 8: ' + v));

// Test 9: Await with array manipulation
console.log('\nTest 9: Await with arrays');
async function arrayTest() {
  const arr = await Promise.resolve([1, 2, 3]);
  const first = arr[0];
  const second = arr[1];
  return first + second;
}

arrayTest().then(v => console.log('Integration Test 9: ' + v));

// Test 10: Await in async method
console.log('\nTest 10: Await in object method');
const calculator = {
  multiplier: 3,
  asyncMultiply: async function(n) {
    const base = await Promise.resolve(n);
    return base * this.multiplier;
  }
};

calculator.asyncMultiply(7).then(v => console.log('Integration Test 10: ' + v));

// Test 11: Immediate await vs deferred
console.log('\nTest 11: Immediate vs deferred await');
async function immediate() {
  return await Promise.resolve('immediate');
}

async function deferred() {
  const promise = Promise.resolve('deferred');
  const result = await promise;
  return result;
}

immediate().then(v => console.log('Integration Test 11a: ' + v));
deferred().then(v => console.log('Integration Test 11b: ' + v));

// Test 12: Await non-promise values directly
console.log('\nTest 12: Await non-promises');
async function awaitNonPromise() {
  const a = await 10;
  const b = await 'string';
  const c = await true;
  return a;
}

awaitNonPromise().then(v => console.log('Integration Test 12: ' + v));

// Test 13: Complex expression with await
console.log('\nTest 13: Complex await expression');
async function complexExpr() {
  const result = (await Promise.resolve(5)) + (await Promise.resolve(3)) * 2;
  return result;
}

complexExpr().then(v => console.log('Integration Test 13: ' + v));

// Test 14: Await with string concatenation
console.log('\nTest 14: Await with string ops');
async function stringOps() {
  const part1 = await Promise.resolve('Hello');
  const part2 = await Promise.resolve(' ');
  const part3 = await Promise.resolve('Await!');
  return part1 + part2 + part3;
}

stringOps().then(v => console.log('Integration Test 14: ' + v));

// Test 15: Return statement with await
console.log('\nTest 15: Direct return await');
async function directReturn() {
  return await Promise.resolve('direct return');
}

directReturn().then(v => console.log('Integration Test 15: ' + v));

console.log('\n=== All integration tests initiated ===');
