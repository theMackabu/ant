// Comprehensive Strict Mode Tests
// Reference: https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Strict_mode

console.log("=== Strict Mode Comprehensive Tests ===\n");

// Test 1: Converting mistakes into errors
console.log("Test 1: Assigning to undeclared variables");
try {
  "use strict";
  eval("x = 3.14;");
  console.log("  FAIL: Should throw ReferenceError");
} catch (e) {
  console.log("  PASS: " + e);
}

// Test 2: Octal literals
console.log("\nTest 2: Legacy octal literals");
try {
  "use strict";
  eval("let num = 0123;");
  console.log("  FAIL: Should reject octal literals");
} catch (e) {
  console.log("  PASS: Octal literals rejected");
}

// Test 3: Reserved words as identifiers
console.log("\nTest 3: Future reserved words");
const reservedWords = ["implements", "interface", "package", "private", "protected", "public", "static"];
let passedReserved = 0;
for (let i = 0; i < reservedWords.length; i++) {
  try {
    eval('"use strict"; let ' + reservedWords[i] + ' = 1;');
    console.log("  FAIL: '" + reservedWords[i] + "' should be reserved");
  } catch (e) {
    passedReserved++;
  }
}
console.log("  PASS: " + passedReserved + " of " + reservedWords.length + " reserved words blocked");

// Test 4: eval and arguments restrictions
console.log("\nTest 4: eval and arguments restrictions");
try {
  "use strict";
  eval("let eval = 5;");
  console.log("  FAIL: Cannot assign to 'eval'");
} catch (e) {
  console.log("  PASS: Cannot use 'eval' as identifier");
}

try {
  "use strict";
  eval("let arguments = 10;");
  console.log("  FAIL: Cannot assign to 'arguments'");
} catch (e) {
  console.log("  PASS: Cannot use 'arguments' as identifier");
}

// Test 5: with statement
console.log("\nTest 5: with statement");
try {
  "use strict";
  eval("with ({}) {}");
  console.log("  FAIL: with statement should be disallowed");
} catch (e) {
  console.log("  PASS: with statement blocked");
}

// Test 6: Function parameter restrictions
console.log("\nTest 6: Function parameter restrictions");
try {
  "use strict";
  eval("function f(a, b, a) {}");
  console.log("  FAIL: Duplicate parameters should be disallowed");
} catch (e) {
  console.log("  PASS: Duplicate parameters blocked");
}

try {
  "use strict";
  eval("function f(eval) {}");
  console.log("  FAIL: 'eval' as parameter should be disallowed");
} catch (e) {
  console.log("  PASS: Cannot use 'eval' as parameter name");
}

// Test 7: Strict mode in different contexts
console.log("\nTest 7: Strict mode in different contexts");

// Script-level strict mode
let scriptStrictWorks = false;
try {
  eval('"use strict"; undeclared = 1;');
} catch (e) {
  scriptStrictWorks = true;
}
console.log("  " + (scriptStrictWorks ? "PASS" : "FAIL") + ": Script-level strict mode");

// Function-level strict mode
function testFunctionStrict() {
  "use strict";
  try {
    undeclared2 = 2;
    return false;
  } catch (e) {
    return true;
  }
}
console.log("  " + (testFunctionStrict() ? "PASS" : "FAIL") + ": Function-level strict mode");

console.log("\n=== All Tests Completed ===");
