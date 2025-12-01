// Test await functionality
console.log('=== Await Tests ===');

// Test 1: Basic await with resolved promise
console.log('\nTest 1: Basic await with resolved promise');
async function test1() {
  const result = await Promise.resolve(42);
  console.log('Awaited resolved promise: ' + result);
  return result;
}
test1().then(v => console.log('Test 1 returned: ' + v));

// Test 2: Await with Promise.resolve string
console.log('\nTest 2: Await with string promise');
async function test2() {
  const msg = await Promise.resolve('hello world');
  console.log('Awaited string: ' + msg);
  return msg;
}
test2().then(v => console.log('Test 2 returned: ' + v));

// Test 3: Await non-promise value (should return value directly)
console.log('\nTest 3: Await non-promise value');
async function test3() {
  const value = await 100;
  console.log('Awaited non-promise: ' + value);
  return value;
}
test3().then(v => console.log('Test 3 returned: ' + v));

// Test 4: Multiple awaits in sequence
console.log('\nTest 4: Multiple sequential awaits');
async function test4() {
  const a = await Promise.resolve(10);
  const b = await Promise.resolve(20);
  const c = await Promise.resolve(30);
  const sum = a + b + c;
  console.log('Sum of awaited values: ' + sum);
  return sum;
}
test4().then(v => console.log('Test 4 returned: ' + v));

// Test 5: Await in expression
console.log('\nTest 5: Await in expression');
async function test5() {
  const result = (await Promise.resolve(5)) * 2;
  console.log('Awaited and multiplied: ' + result);
  return result;
}
test5().then(v => console.log('Test 5 returned: ' + v));

// Test 6: Await with conditional
console.log('\nTest 6: Await with conditional');
async function test6(flag) {
  if (flag) {
    const v = await Promise.resolve('true branch');
    return v;
  } else {
    const v = await Promise.resolve('false branch');
    return v;
  }
}
test6(true).then(v => console.log('Test 6 (true): ' + v));
test6(false).then(v => console.log('Test 6 (false): ' + v));

// Test 7: Async arrow function with await
console.log('\nTest 7: Async arrow function with await');
const test7 = async () => {
  const x = await Promise.resolve(7);
  return x * 7;
};
test7().then(v => console.log('Test 7 returned: ' + v));

// Test 8: Async arrow with parameter and await
console.log('\nTest 8: Async arrow with parameter');
const test8 = async (n) => {
  const result = await Promise.resolve(n);
  return result + 10;
};
test8(5).then(v => console.log('Test 8 returned: ' + v));

// Test 9: Await with object property
console.log('\nTest 9: Await with object');
async function test9() {
  const obj = await Promise.resolve({ x: 100, y: 200 });
  console.log('Awaited object.x: ' + obj.x);
  return obj.y;
}
test9().then(v => console.log('Test 9 returned: ' + v));

// Test 10: Await with array
console.log('\nTest 10: Await with array');
async function test10() {
  const arr = await Promise.resolve([1, 2, 3]);
  console.log('Awaited array[0]: ' + arr[0]);
  return arr[1];
}
test10().then(v => console.log('Test 10 returned: ' + v));

// Test 11: Nested async functions with await
console.log('\nTest 11: Nested async functions');
async function inner11() {
  return await Promise.resolve('inner result');
}
async function outer11() {
  const result = await inner11();
  return 'outer got: ' + result;
}
outer11().then(v => console.log('Test 11 returned: ' + v));

// Test 12: Await in async method
console.log('\nTest 12: Await in async method');
const obj12 = {
  value: 50,
  asyncMethod: async function() {
    const multiplier = await Promise.resolve(2);
    return this.value * multiplier;
  }
};
obj12.asyncMethod().then(v => console.log('Test 12 returned: ' + v));

// Test 13: Await with string concatenation
console.log('\nTest 13: Await with string operations');
async function test13() {
  const first = await Promise.resolve('Hello');
  const second = await Promise.resolve('World');
  return first + ' ' + second;
}
test13().then(v => console.log('Test 13 returned: ' + v));

// Test 14: Await with nested promise chains
console.log('\nTest 14: Await with chained values');
async function test14() {
  const a = await Promise.resolve(1);
  const b = await Promise.resolve(a + 1);
  const c = await Promise.resolve(b + 1);
  return c;
}
test14().then(v => console.log('Test 14 returned: ' + v));

// Test 15: Return await
console.log('\nTest 15: Return await');
async function test15() {
  return await Promise.resolve('returned from await');
}
test15().then(v => console.log('Test 15 returned: ' + v));

// Test 16: Await boolean value
console.log('\nTest 16: Await boolean');
async function test16() {
  const result = await Promise.resolve(true);
  if (result) {
    return 'boolean was true';
  }
  return 'boolean was false';
}
test16().then(v => console.log('Test 16 returned: ' + v));

// Test 17: Await null
console.log('\nTest 17: Await null');
async function test17() {
  const result = await Promise.resolve(null);
  return result;
}
test17().then(v => console.log('Test 17 returned (null): ' + v));

// Test 18: Await undefined
console.log('\nTest 18: Await undefined');
async function test18() {
  const result = await Promise.resolve(undefined);
  return result;
}
test18().then(v => console.log('Test 18 returned (undefined): ' + v));

// Test 19: Async function expression with await
console.log('\nTest 19: Async function expression');
const test19 = async function() {
  const x = await Promise.resolve(19);
  return x;
};
test19().then(v => console.log('Test 19 returned: ' + v));

// Test 20: Await with arithmetic operations
console.log('\nTest 20: Await with arithmetic');
async function test20() {
  const a = await Promise.resolve(10);
  const b = await Promise.resolve(5);
  return a + b * 2;
}
test20().then(v => console.log('Test 20 returned: ' + v));

console.log('\n=== Synchronous code finished ===');
