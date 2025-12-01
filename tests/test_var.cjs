// Test var keyword (should work like let but show warnings)
Ant.println('=== Var Keyword Tests ===');

// Test 1: Basic var declaration
Ant.println('\nTest 1: Basic var declaration');
var x = 10;
Ant.println('var x = 10: ' + x);

// Test 2: Var reassignment
Ant.println('\nTest 2: Var reassignment');
var y = 5;
y = y + 10;
Ant.println('var y reassigned: ' + y);

// Test 3: Var in for loop
Ant.println('\nTest 3: Var in for loop');
var sum = 0;
for (var i = 0; i < 5; i = i + 1) {
  sum = sum + i;
}
Ant.println('Sum with var: ' + sum);

// Test 4: Var in for-in loop
Ant.println('\nTest 4: Var in for-in loop');
const obj = { a: 1, b: 2, c: 3 };
var keys = '';
for (var key in obj) {
  keys = keys + key + ',';
}
Ant.println('Keys with var: ' + keys);

// Test 5: Multiple var declarations
Ant.println('\nTest 5: Multiple var declarations');
var a = 1, b = 2, c = 3;
Ant.println('var a, b, c: ' + (a + b + c));

// Test 6: Var in while loop
Ant.println('\nTest 6: Var in while loop');
var count = 0;
while (count < 3) {
  count = count + 1;
}
Ant.println('While with var: ' + count);

// Test 7: Var with objects
Ant.println('\nTest 7: Var with objects');
var obj2 = { name: 'test', value: 42 };
Ant.println('var object: ' + obj2.name + ' = ' + obj2.value);

// Test 8: Var with arrays
Ant.println('\nTest 8: Var with arrays');
var arr = [10, 20, 30];
Ant.println('var array[1]: ' + arr[1]);

// Test 9: Var in nested scopes
Ant.println('\nTest 9: Var in nested scopes');
var outer = 'outer';
if (true) {
  var inner = 'inner';
  Ant.println('Inner var: ' + inner);
}
Ant.println('Outer var: ' + outer);

// Test 10: Var works like let
Ant.println('\nTest 10: Var behavior like let');
var testVar = 100;
if (testVar > 50) {
  var modified = testVar * 2;
  Ant.println('Modified: ' + modified);
}
Ant.println('Original: ' + testVar);

Ant.println('\n=== All var tests completed ===');
Ant.println('Note: You should see deprecation warnings above');
