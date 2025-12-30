// Test Function constructor
// This tests whether the Function constructor can create a dynamic function
// from string arguments.
//
// The Function constructor syntax:
//   new Function(arg1, arg2, ..., argN, functionBody)
// All arguments are strings. The last argument is the function body,
// and all previous arguments are parameter names.

console.log('=== Testing Function Constructor ===\n');

// Test 1: Basic two-parameter function
console.log('Test 1: Basic addition function');
let sum = new Function('a', 'b', 'return a + b');
console.log('  Created function:', sum);
console.log('  typeof sum:', typeof sum);
console.log('  sum(1, 2):', sum(1, 2), '(expected: 3)');

// Test 2: No parameters
console.log('\nTest 2: Function with no parameters');
let hello = new Function('return "Hello World"');
console.log('  hello():', hello(), '(expected: "Hello World")');

// Test 3: Single parameter
console.log('\nTest 3: Single parameter function');
let square = new Function('x', 'return x * x');
console.log('  square(5):', square(5), '(expected: 25)');

// Test 4: Multiple statements in body
console.log('\nTest 4: Multiple statements in body');
let multi = new Function('x', 'let y = x * 2; return y + 1');
console.log('  multi(5):', multi(5), '(expected: 11)');

// Test 5: Access to global scope
console.log('\nTest 5: Access to window.global scope variables');
window.global.factor = 10;
let multiply = new Function('x', 'return x * factor');
console.log('  multiply(5):', multiply(5), '(expected: 50)');

// Test 6: Empty function body
console.log('\nTest 6: Empty function body');
let empty = new Function('');
console.log('  empty():', empty(), '(expected: undefined)');

// Test 7: Many parameters
console.log('\nTest 7: Function with three parameters');
let add3 = new Function('a', 'b', 'c', 'return a + b + c');
console.log('  add3(1, 2, 3):', add3(1, 2, 3), '(expected: 6)');

// Test 8: String concatenation
console.log('\nTest 8: String concatenation');
let greet = new Function('name', 'return "Hello, " + name + "!"');
console.log('  greet("World"):', greet('World'), '(expected: "Hello, World!")');

console.log('\n=== All Function Constructor Tests Complete ===');
