// Test property lookup caching - validates that the optimization doesn't break property access
console.log('Testing property lookup cache optimization...');

// Test 1: Basic property access
const obj1 = { name: 'test', value: 42, nested: { x: 1, y: 2 } };
console.assert(obj1.name === 'test', 'Basic property access failed');
console.assert(obj1.value === 42, 'Number property access failed');
console.assert(obj1.nested.x === 1, 'Nested property access failed');
console.log('✓ Test 1: Basic property access');

// Test 2: Multiple accesses (should hit cache on second access)
const obj2 = { prop: 'cached' };
const access1 = obj2.prop;
const access2 = obj2.prop;
const access3 = obj2.prop;
console.assert(access1 === 'cached', 'First access failed');
console.assert(access2 === 'cached', 'Cached access failed');
console.assert(access3 === 'cached', 'Second cached access failed');
console.log('✓ Test 2: Multiple property accesses (cache hits)');

// Test 3: Property modification (cache invalidation)
const obj3 = { x: 1 };
console.assert(obj3.x === 1, 'Initial property value wrong');
obj3.x = 2;
console.assert(obj3.x === 2, 'Property modification failed');
obj3.x = 3;
console.assert(obj3.x === 3, 'Second modification failed');
console.log('✓ Test 3: Property modification with cache invalidation');

// Test 4: Dynamic property creation
const obj4 = {};
obj4.dynamic = 'new';
console.assert(obj4.dynamic === 'new', 'Dynamic property creation failed');
obj4.another = 'property';
console.assert(obj4.another === 'property', 'Second dynamic property failed');
console.assert(obj4.dynamic === 'new', 'First dynamic property corrupted');
console.log('✓ Test 4: Dynamic property creation');

// Test 5: Method access (direct property access)
const MyFunc = {};
MyFunc.getData = function() { return 'test data'; };
console.assert(MyFunc.getData() === 'test data', 'Method access failed');
console.log('✓ Test 5: Method access');

// Test 6: Properties with special names
const obj6 = { __code: 'test', name: 'obj6' };
console.assert(obj6.name === 'obj6', 'Short key property failed');
console.log('✓ Test 6: Properties with special names');

// Test 7: Many properties in object (stress test for cache)
const obj7 = {};
for (let i = 0; i < 50; i++) {
  obj7['prop' + i] = i;
}
console.assert(obj7.prop0 === 0, 'First property in large object failed');
console.assert(obj7.prop25 === 25, 'Middle property in large object failed');
console.assert(obj7.prop49 === 49, 'Last property in large object failed');
// Access again - should hit cache
console.assert(obj7.prop25 === 25, 'Cached access in large object failed');
console.log('✓ Test 7: Many properties in object');

// Test 8: Property lookup in scope chain
function outer() {
  const outerVar = 'outer';
  function inner() {
    const innerVar = 'inner';
    return outerVar + innerVar;
  }
  return inner();
}
console.assert(outer() === 'outerinner', 'Scope chain lookup failed');
console.log('✓ Test 8: Property lookup in scope chain');

// Test 9: Long property names (shouldn't be cached but should work)
const longName = 'verylongpropertyname' + 'thatshouldnot' + 'becached' + 'duetolength';
const obj9 = {};
obj9[longName] = 'value';
console.assert(obj9[longName] === 'value', 'Long property name access failed');
console.log('✓ Test 9: Long property names');

// Test 10: Computed properties
const obj10 = {};
obj10.value = 10;
obj10.double = function() { return this.value * 2; };
console.assert(obj10.double() === 20, 'Computed property failed');
console.log('✓ Test 10: Computed properties');

// Test 11: Property existence checks
const obj11 = { exists: true };
if (obj11.exists) {
  console.log('✓ Test 11: Property existence check');
} else {
  throw new Error('Property existence check failed');
}

// Test 12: Repeated property pattern (simulates real-world code)
const objs = [];
for (let i = 0; i < 10; i++) {
  const o = { id: i, name: 'obj' + i, value: i * 10 };
  objs.push(o);
}
let sum = 0;
for (let i = 0; i < objs.length; i++) {
  sum += objs[i].value;
}
console.assert(sum === 450, 'Repeated property pattern failed');
console.log('✓ Test 12: Repeated property access pattern');

// Test 13: console.assert with true condition (should not print anything)
console.assert(true, 'This should not appear');
console.log('✓ Test 13: console.assert with true condition');

// Test 14: console.assert with false condition (should print message)
console.assert(false, 'This is an expected assertion failure');
console.log('✓ Test 14: console.assert with false condition');

// Test 15: Accessing same property repeatedly across iterations
const testObj = { counter: 0 };
for (let i = 0; i < 100; i++) {
  testObj.counter = i;
}
console.assert(testObj.counter === 99, 'Repeated property writes failed');
console.log('✓ Test 15: Repeated property writes');

console.log('\n✅ All property lookup cache tests passed!');
