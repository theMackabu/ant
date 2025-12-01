// Test Promise display with various value types
console.log('=== Promise Display Test ===');

// Test 1: Number
console.log('\nTest 1: Promise with number');
const p1 = Promise.resolve(42);
console.log('Promise.resolve(42): ' + p1);

// Test 2: String
console.log('\nTest 2: Promise with string');
const p2 = Promise.resolve('hello');
console.log('Promise.resolve("hello"): ' + p2);

// Test 3: Boolean (true)
console.log('\nTest 3: Promise with boolean');
const p3 = Promise.resolve(true);
console.log('Promise.resolve(true): ' + p3);

// Test 4: Boolean (false)
const p4 = Promise.resolve(false);
console.log('Promise.resolve(false): ' + p4);

// Test 5: Null
console.log('\nTest 5: Promise with null');
const p5 = Promise.resolve(null);
console.log('Promise.resolve(null): ' + p5);

// Test 6: Undefined
console.log('\nTest 6: Promise with undefined');
const p6 = Promise.resolve(undefined);
console.log('Promise.resolve(undefined): ' + p6);

// Test 7: Object
console.log('\nTest 7: Promise with object');
const p7 = Promise.resolve({ x: 1, y: 2 });
console.log('Promise.resolve({x:1,y:2}): ' + p7);

// Test 8: Array
console.log('\nTest 8: Promise with array');
const p8 = Promise.resolve([1, 2, 3]);
console.log('Promise.resolve([1,2,3]): ' + p8);

// Test 9: Rejected promise
console.log('\nTest 9: Rejected promise');
const p9 = Promise.reject('error message');
console.log('Promise.reject("error message"): ' + p9);

// Test 10: Pending promise
console.log('\nTest 10: Pending promise');
const p10 = new Promise(resolve => {
  setTimeout(() => {
    resolve(100);
    console.log('After timeout, promise is: ' + p10);
  }, 100);
});
console.log('Before timeout, promise is: ' + p10);

// Test 11: Async function result
console.log('\nTest 11: Async function result');
async function getValue() {
  return 999;
}
const p11 = getValue();
console.log('Async function returned: ' + p11);

// Test 12: Async function with object
console.log('\nTest 12: Async function with object');
async function getObject() {
  return { value: 42, name: 'test' };
}
const p12 = getObject();
console.log('Async object function: ' + p12);

// Test 13: Async function with array
console.log('\nTest 13: Async function with array');
async function getArray() {
  return [10, 20, 30];
}
const p13 = getArray();
console.log('Async array function: ' + p13);

// Test 14: Promise chaining visibility
console.log('\nTest 14: Promise in then handler');
Promise.resolve(5).then(v => {
  const inner = Promise.resolve(v * 2);
  console.log('Inner promise: ' + inner);
  return inner;
});

console.log('\n=== Synchronous code finished ===');
