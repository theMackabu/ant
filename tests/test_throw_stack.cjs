// Test throw statement with stack traces
Ant.println('=== Throw with Stack Trace Test ===');

function level3() {
  Ant.println('In level3');
  throw "error from level3";
}

function level2() {
  Ant.println('In level2');
  level3();
}

function level1() {
  Ant.println('In level1');
  level2();
}

Ant.println('Starting test...');
level1();
Ant.println('This should not print');
