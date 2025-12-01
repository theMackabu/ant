// Test Promise display with various value types
Ant.println('=== Promise Display Test ===');

// Test 1: Number
Ant.println('\nTest 1: Promise with number');
const p1 = Promise.resolve(42);
Ant.println('Promise.resolve(42): ' + p1);

// Test 2: String
Ant.println('\nTest 2: Promise with string');
const p2 = Promise.resolve('hello');
Ant.println('Promise.resolve("hello"): ' + p2);

// Test 3: Boolean (true)
Ant.println('\nTest 3: Promise with boolean');
const p3 = Promise.resolve(true);
Ant.println('Promise.resolve(true): ' + p3);

// Test 4: Boolean (false)
const p4 = Promise.resolve(false);
Ant.println('Promise.resolve(false): ' + p4);

// Test 5: Null
Ant.println('\nTest 5: Promise with null');
const p5 = Promise.resolve(null);
Ant.println('Promise.resolve(null): ' + p5);

// Test 6: Undefined
Ant.println('\nTest 6: Promise with undefined');
const p6 = Promise.resolve(undefined);
Ant.println('Promise.resolve(undefined): ' + p6);

// Test 7: Object
Ant.println('\nTest 7: Promise with object');
const p7 = Promise.resolve({ x: 1, y: 2 });
Ant.println('Promise.resolve({x:1,y:2}): ' + p7);

// Test 8: Array
Ant.println('\nTest 8: Promise with array');
const p8 = Promise.resolve([1, 2, 3]);
Ant.println('Promise.resolve([1,2,3]): ' + p8);

// Test 9: Rejected promise
Ant.println('\nTest 9: Rejected promise');
const p9 = Promise.reject('error message');
Ant.println('Promise.reject("error message"): ' + p9);

// Test 10: Pending promise
Ant.println('\nTest 10: Pending promise');
const p10 = new Promise((resolve) => {
  Ant.setTimeout(() => {
    resolve(100);
    Ant.println('After timeout, promise is: ' + p10);
  }, 100);
});
Ant.println('Before timeout, promise is: ' + p10);

// Test 11: Async function result
Ant.println('\nTest 11: Async function result');
async function getValue() {
  return 999;
}
const p11 = getValue();
Ant.println('Async function returned: ' + p11);

// Test 12: Async function with object
Ant.println('\nTest 12: Async function with object');
async function getObject() {
  return { value: 42, name: 'test' };
}
const p12 = getObject();
Ant.println('Async object function: ' + p12);

// Test 13: Async function with array
Ant.println('\nTest 13: Async function with array');
async function getArray() {
  return [10, 20, 30];
}
const p13 = getArray();
Ant.println('Async array function: ' + p13);

// Test 14: Promise chaining visibility
Ant.println('\nTest 14: Promise in then handler');
Promise.resolve(5).then(v => {
  const inner = Promise.resolve(v * 2);
  Ant.println('Inner promise: ' + inner);
  return inner;
});

Ant.println('\n=== Synchronous code finished ===');
