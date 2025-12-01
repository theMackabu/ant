// Test var keyword (should work like let but show warnings)
console.log('=== Var Keyword Tests ===');

// Test 1: Basic var declaration
console.log('\nTest 1: Basic var declaration');
var x = 10;
console.log('var x = 10: ' + x);

// Test 2: Var reassignment
console.log('\nTest 2: Var reassignment');
var y = 5;
y = y + 10;
console.log('var y reassigned: ' + y);

// Test 3: Var in for loop
console.log('\nTest 3: Var in for loop');
var sum = 0;
for (var i = 0; i < 5; i = i + 1) {
  sum = sum + i;
}
console.log('Sum with var: ' + sum);

// Test 4: Var in for-in loop
console.log('\nTest 4: Var in for-in loop');
const obj = { a: 1, b: 2, c: 3 };
var keys = '';
for (var key in obj) {
  keys = keys + key + ',';
}
console.log('Keys with var: ' + keys);

// Test 5: Multiple var declarations
console.log('\nTest 5: Multiple var declarations');
var a = 1, b = 2, c = 3;
console.log('var a, b, c: ' + (a + b + c));

// Test 6: Var in while loop
console.log('\nTest 6: Var in while loop');
var count = 0;
while (count < 3) {
  count = count + 1;
}
console.log('While with var: ' + count);

// Test 7: Var with objects
console.log('\nTest 7: Var with objects');
var obj2 = { name: 'test', value: 42 };
console.log('var object: ' + obj2.name + ' = ' + obj2.value);

// Test 8: Var with arrays
console.log('\nTest 8: Var with arrays');
var arr = [10, 20, 30];
console.log('var array[1]: ' + arr[1]);

// Test 9: Var in nested scopes
console.log('\nTest 9: Var in nested scopes');
var outer = 'outer';
if (true) {
  var inner = 'inner';
  console.log('Inner var: ' + inner);
}
console.log('Outer var: ' + outer);

// Test 10: Var works like let
console.log('\nTest 10: Var behavior like let');
var testVar = 100;
if (testVar > 50) {
  var modified = testVar * 2;
  console.log('Modified: ' + modified);
}
console.log('Original: ' + testVar);

console.log('\n=== All var tests completed ===');
console.log('Note: You should see deprecation warnings above');
