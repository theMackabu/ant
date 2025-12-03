// Test strict mode features
// Based on MDN: https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Strict_mode

// Test 1: Strict mode enabled at script level
"use strict";

// Test 2: Assignment to undeclared variables should fail
try {
  mistypeVariable = 17;
  console.log("FAIL: Should have thrown ReferenceError for undeclared variable");
} catch (e) {
  console.log("PASS: Caught error for undeclared variable assignment");
}

// Test 3: Using eval as variable name should fail
try {
  eval("let eval = 5;");
  console.log("FAIL: Should have thrown error for using 'eval' as variable name");
} catch (e) {
  console.log("PASS: Cannot use 'eval' as variable name in strict mode");
}

// Test 4: Using arguments as variable name should fail
try {
  eval("let arguments = 10;");
  console.log("FAIL: Should have thrown error for using 'arguments' as variable name");
} catch (e) {
  console.log("PASS: Cannot use 'arguments' as variable name in strict mode");
}

// Test 5: Duplicate parameter names should fail
try {
  eval("function sum(a, a, c) { return a + a + c; }");
  console.log("FAIL: Should have thrown error for duplicate parameter names");
} catch (e) {
  console.log("PASS: Duplicate parameter names not allowed in strict mode");
}

// Test 6: Octal literals should fail
try {
  eval("let x = 0644;");
  console.log("FAIL: Should have thrown error for octal literal");
} catch (e) {
  console.log("PASS: Octal literals not allowed in strict mode");
}

// Test 7: Reserved words should not be usable as identifiers
try {
  eval("let implements = 5;");
  console.log("FAIL: Should have thrown error for using reserved word 'implements'");
} catch (e) {
  console.log("PASS: Reserved word 'implements' cannot be used in strict mode");
}

// Test 8: with statement should fail
try {
  eval("with (Math) { x = cos(2); }");
  console.log("FAIL: Should have thrown error for 'with' statement");
} catch (e) {
  console.log("PASS: 'with' statement not allowed in strict mode");
}

console.log("\nAll strict mode tests completed!");
