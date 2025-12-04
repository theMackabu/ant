// Test console methods (log, error, warn, assert, trace)
console.log('=== Testing Console Methods ===');

// Test 1: console.log
console.log('Test 1: console.log');
console.log('  - Basic message');
console.log('  - Multiple', 'arguments', 'work');
console.log('  - Numbers:', 42, 3.14);
console.log('  - Objects:', { key: 'value' });

// Test 2: console.error
console.error('Test 2: console.error - This should appear in red');

// Test 3: console.warn
console.warn('Test 3: console.warn - This should appear in yellow');

// Test 4: console.assert with true condition
console.assert(true, 'This should not print');
console.assert(1 === 1, 'Math check');
console.log('Test 4: console.assert (true conditions) - passed');

// Test 5: console.assert with false condition
console.assert(false, 'Expected assertion failure');
console.assert(1 === 2, 'This assertion fails');
console.log('Test 5: console.assert (false conditions) - checked');

// Test 6: console.trace
console.trace('Test 6: console.trace with message');

// Test 7: console.trace with nested functions
function level3() {
  console.trace('Trace from level 3');
}

function level2() {
  level3();
}

function level1() {
  level2();
}

console.log('Starting nested function trace:');
level1();

// Test 8: Complex objects
const complexObj = {
  name: 'test',
  nested: { x: 1, y: 2 },
  array: [1, 2, 3],
  bool: true
};
console.log('Test 8: Complex object:', complexObj);

// Test 9: Empty console.trace
console.trace();

console.log('\n=== All console method tests completed ===');
