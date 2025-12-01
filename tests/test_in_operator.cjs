// Test 'in' operator and for-in loops
console.log('=== In Operator and For-In Tests ===');

// Test 1: Basic 'in' operator with object
console.log('\nTest 1: Basic in operator');
const obj1 = { name: 'test', value: 42, flag: true };
console.log("'name' in obj1: " + ('name' in obj1));
console.log("'value' in obj1: " + ('value' in obj1));
console.log("'missing' in obj1: " + ('missing' in obj1));

// Test 2: 'in' operator with arrays
console.log('\nTest 2: In operator with arrays');
const arr = [10, 20, 30];
console.log("'0' in arr: " + ('0' in arr));
console.log("'1' in arr: " + ('1' in arr));
console.log("'5' in arr: " + ('5' in arr));
console.log("'length' in arr: " + ('length' in arr));

// Test 3: Basic for-in loop with object
console.log('\nTest 3: For-in loop with object');
const obj2 = { a: 1, b: 2, c: 3 };
let keys = '';
for (let key in obj2) {
  keys = keys + key + ',';
}
console.log('Keys: ' + keys);

// Test 4: For-in loop with const
console.log('\nTest 4: For-in with const');
const obj3 = { x: 10, y: 20 };
for (const prop in obj3) {
  console.log('Property: ' + prop + ' = ' + obj3[prop]);
}

// Test 5: For-in loop accumulating values
console.log('\nTest 5: For-in accumulating values');
const obj4 = { first: 5, second: 10, third: 15 };
let sum = 0;
for (let k in obj4) {
  sum = sum + obj4[k];
}
console.log('Sum of values: ' + sum);

// Test 6: For-in with break
console.log('\nTest 6: For-in with break');
const obj5 = { a: 1, b: 2, c: 3, d: 4 };
let count = 0;
for (let key in obj5) {
  count = count + 1;
  if (count === 2) {
    break;
  }
  console.log('Key: ' + key);
}
console.log('Stopped at count: ' + count);

// Test 7: For-in with continue
console.log('\nTest 7: For-in with continue');
const obj6 = { a: 1, b: 2, c: 3, d: 4 };
for (let key in obj6) {
  if (key === 'b' || key === 'd') {
    continue;
  }
  console.log('Processing: ' + key);
}

// Test 8: For-in with array
console.log('\nTest 8: For-in with array');
const arr2 = ['first', 'second', 'third'];
for (let idx in arr2) {
  console.log('Index ' + idx + ': ' + arr2[idx]);
}

// Test 9: Nested for-in loops
console.log('\nTest 9: Nested for-in loops');
const outer = { a: { x: 1, y: 2 }, b: { x: 3, y: 4 } };
for (let key1 in outer) {
  for (let key2 in outer[key1]) {
    console.log(key1 + '.' + key2 + ' = ' + outer[key1][key2]);
  }
}

// Test 10: In operator with nested properties
console.log('\nTest 10: In operator with nested object');
const nested = { outer: { inner: 'value' } };
console.log("'outer' in nested: " + ('outer' in nested));
console.log("'inner' in nested: " + ('inner' in nested));

// Test 11: For-in counting properties
console.log('\nTest 11: Counting properties with for-in');
const obj7 = { prop1: 'a', prop2: 'b', prop3: 'c', prop4: 'd' };
let propCount = 0;
for (let p in obj7) {
  propCount = propCount + 1;
}
console.log('Property count: ' + propCount);

// Test 12: For-in with empty object
console.log('\nTest 12: For-in with empty object');
const empty = {};
let ranOnce = false;
for (let k in empty) {
  ranOnce = true;
}
console.log('Loop ran: ' + ranOnce);

// Test 13: In operator with different types
console.log('\nTest 13: In operator checks');
const testObj = { num: 42, str: 'hello', bool: true };
console.log("'num' in testObj: " + ('num' in testObj));
console.log("'str' in testObj: " + ('str' in testObj));
console.log("'bool' in testObj: " + ('bool' in testObj));

// Test 14: For-in modifying external variable
console.log('\nTest 14: For-in with external variable');
const data = { a: 10, b: 20, c: 30 };
let total = 0;
for (let key in data) {
  total = total + data[key];
}
console.log('Total: ' + total);

// Test 15: For-in with conditional logic
console.log('\nTest 15: For-in with conditional');
const items = { item1: 5, item2: 15, item3: 25, item4: 35 };
let filtered = 0;
for (let name in items) {
  if (items[name] > 10) {
    filtered = filtered + items[name];
  }
}
console.log('Filtered sum (>10): ' + filtered);

console.log('\n=== All in operator tests completed ===');
