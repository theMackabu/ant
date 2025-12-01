// Test throw statement with stack traces
console.log('=== Throw with Stack Trace Test ===');

function level3() {
  console.log('In level3');
  throw "error from level3";
}

function level2() {
  console.log('In level2');
  level3();
}

function level1() {
  console.log('In level1');
  level2();
}

console.log('Starting test...');
level1();
console.log('This should not print');
