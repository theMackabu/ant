// Test eval() function
// eval() executes JavaScript code represented as a string
// and returns the value of the last expression evaluated

console.log('=== Testing eval() Function ===\n');

// Test 1: Basic arithmetic
console.log('Test 1: Basic arithmetic');
console.log('  eval("1 + 2"):', eval('1 + 2'), '(expected: 3)');
console.log('  eval("10 * 5"):', eval('10 * 5'), '(expected: 50)');

// Test 2: Variable declaration and use
console.log('\nTest 2: Variable declaration and access');
eval('let x = 42');
console.log('  After eval("let x = 42"), x =', x, '(expected: 42)');

// Test 3: Function definition
console.log('\nTest 3: Function definition via eval');
eval('function add(a, b) { return a + b; }');
console.log('  add(3, 4):', add(3, 4), '(expected: 7)');

// Test 4: Access to outer scope variables
console.log('\nTest 4: Access to outer scope variables');
let outerVar = 100;
let result = eval('outerVar + 50');
console.log('  outerVar = 100, eval("outerVar + 50"):', result, '(expected: 150)');

// Test 5: String operations
console.log('\nTest 5: String operations');
console.log('  eval(\'"Hello" + " " + "World"\'):', eval('"Hello" + " " + "World"'), '(expected: "Hello World")');

// Test 6: Object literal
console.log('\nTest 6: Object literal');
let obj = eval('({a: 1, b: 2})');
console.log('  eval("({a: 1, b: 2})"):', obj);
console.log('  obj.a:', obj.a, '(expected: 1)');
console.log('  obj.b:', obj.b, '(expected: 2)');

// Test 7: Array literal
console.log('\nTest 7: Array literal');
let arr = eval('[1, 2, 3, 4, 5]');
console.log('  eval("[1, 2, 3, 4, 5]"):', arr);
console.log('  arr[0]:', arr[0], '(expected: 1)');
console.log('  arr.length:', arr.length, '(expected: 5)');

// Test 8: Non-string argument (should return as-is)
console.log('\nTest 8: Non-string argument');
console.log('  eval(42):', eval(42), '(expected: 42)');
console.log('  eval(true):', eval(true), '(expected: true)');

// Test 9: Empty eval
console.log('\nTest 9: Empty eval');
console.log('  eval():', eval(), '(expected: undefined)');

// Test 10: Complex expression
console.log('\nTest 10: Complex expression');
let complex = eval('let temp = 5; temp * temp + 10');
console.log('  eval("let temp = 5; temp * temp + 10"):', complex, '(expected: 35)');

// Test 11: Modifying existing variable
console.log('\nTest 11: Modifying existing variable');
let modVar = 10;
console.log('  Before: modVar =', modVar);
eval('modVar = modVar * 2');
console.log('  After eval("modVar = modVar * 2"): modVar =', modVar, '(expected: 20)');

// Test 12: Return value is last expression
console.log('\nTest 12: Return value is last expression');
let lastExpr = eval('1 + 1; 2 + 2; 3 + 3');
console.log('  eval("1 + 1; 2 + 2; 3 + 3"):', lastExpr, '(expected: 6)');

console.log('\n=== All eval() Tests Complete ===');
