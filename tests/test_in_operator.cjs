// Test 'in' operator and for-in loops
Ant.println('=== In Operator and For-In Tests ===');

// Test 1: Basic 'in' operator with object
Ant.println('\nTest 1: Basic in operator');
const obj1 = { name: 'test', value: 42, flag: true };
Ant.println("'name' in obj1: " + ('name' in obj1));
Ant.println("'value' in obj1: " + ('value' in obj1));
Ant.println("'missing' in obj1: " + ('missing' in obj1));

// Test 2: 'in' operator with arrays
Ant.println('\nTest 2: In operator with arrays');
const arr = [10, 20, 30];
Ant.println("'0' in arr: " + ('0' in arr));
Ant.println("'1' in arr: " + ('1' in arr));
Ant.println("'5' in arr: " + ('5' in arr));
Ant.println("'length' in arr: " + ('length' in arr));

// Test 3: Basic for-in loop with object
Ant.println('\nTest 3: For-in loop with object');
const obj2 = { a: 1, b: 2, c: 3 };
let keys = '';
for (let key in obj2) {
  keys = keys + key + ',';
}
Ant.println('Keys: ' + keys);

// Test 4: For-in loop with const
Ant.println('\nTest 4: For-in with const');
const obj3 = { x: 10, y: 20 };
for (const prop in obj3) {
  Ant.println('Property: ' + prop + ' = ' + obj3[prop]);
}

// Test 5: For-in loop accumulating values
Ant.println('\nTest 5: For-in accumulating values');
const obj4 = { first: 5, second: 10, third: 15 };
let sum = 0;
for (let k in obj4) {
  sum = sum + obj4[k];
}
Ant.println('Sum of values: ' + sum);

// Test 6: For-in with break
Ant.println('\nTest 6: For-in with break');
const obj5 = { a: 1, b: 2, c: 3, d: 4 };
let count = 0;
for (let key in obj5) {
  count = count + 1;
  if (count === 2) {
    break;
  }
  Ant.println('Key: ' + key);
}
Ant.println('Stopped at count: ' + count);

// Test 7: For-in with continue
Ant.println('\nTest 7: For-in with continue');
const obj6 = { a: 1, b: 2, c: 3, d: 4 };
for (let key in obj6) {
  if (key === 'b' || key === 'd') {
    continue;
  }
  Ant.println('Processing: ' + key);
}

// Test 8: For-in with array
Ant.println('\nTest 8: For-in with array');
const arr2 = ['first', 'second', 'third'];
for (let idx in arr2) {
  Ant.println('Index ' + idx + ': ' + arr2[idx]);
}

// Test 9: Nested for-in loops
Ant.println('\nTest 9: Nested for-in loops');
const outer = { a: { x: 1, y: 2 }, b: { x: 3, y: 4 } };
for (let key1 in outer) {
  for (let key2 in outer[key1]) {
    Ant.println(key1 + '.' + key2 + ' = ' + outer[key1][key2]);
  }
}

// Test 10: In operator with nested properties
Ant.println('\nTest 10: In operator with nested object');
const nested = { outer: { inner: 'value' } };
Ant.println("'outer' in nested: " + ('outer' in nested));
Ant.println("'inner' in nested: " + ('inner' in nested));

// Test 11: For-in counting properties
Ant.println('\nTest 11: Counting properties with for-in');
const obj7 = { prop1: 'a', prop2: 'b', prop3: 'c', prop4: 'd' };
let propCount = 0;
for (let p in obj7) {
  propCount = propCount + 1;
}
Ant.println('Property count: ' + propCount);

// Test 12: For-in with empty object
Ant.println('\nTest 12: For-in with empty object');
const empty = {};
let ranOnce = false;
for (let k in empty) {
  ranOnce = true;
}
Ant.println('Loop ran: ' + ranOnce);

// Test 13: In operator with different types
Ant.println('\nTest 13: In operator checks');
const testObj = { num: 42, str: 'hello', bool: true };
Ant.println("'num' in testObj: " + ('num' in testObj));
Ant.println("'str' in testObj: " + ('str' in testObj));
Ant.println("'bool' in testObj: " + ('bool' in testObj));

// Test 14: For-in modifying external variable
Ant.println('\nTest 14: For-in with external variable');
const data = { a: 10, b: 20, c: 30 };
let total = 0;
for (let key in data) {
  total = total + data[key];
}
Ant.println('Total: ' + total);

// Test 15: For-in with conditional logic
Ant.println('\nTest 15: For-in with conditional');
const items = { item1: 5, item2: 15, item3: 25, item4: 35 };
let filtered = 0;
for (let name in items) {
  if (items[name] > 10) {
    filtered = filtered + items[name];
  }
}
Ant.println('Filtered sum (>10): ' + filtered);

Ant.println('\n=== All in operator tests completed ===');
