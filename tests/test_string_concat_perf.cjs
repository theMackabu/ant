// Performance test for string concatenation optimization

console.log("=== String Concatenation Performance Tests ===\n");

// Test 1: Simple binary concatenation
console.log("Test 1: Binary Concatenation");
let start = Date.now();
for (let i = 0; i < 1000; i++) {
  let result = "hello" + "world";
}
let elapsed = Date.now() - start;
console.log("  1000 binary concatenations: " + elapsed + "ms");

// Test 2: Chained concatenation (4-way)
console.log("\nTest 2: Chained Concatenation (4-way)");
start = Date.now();
for (let i = 0; i < 1000; i++) {
  let result = "a" + "b" + "c" + "d";
}
elapsed = Date.now() - start;
console.log("  1000 chained concatenations (4 parts): " + elapsed + "ms");

// Test 3: Long chained concatenation (10-way)
console.log("\nTest 3: Long Chained Concatenation (10-way)");
start = Date.now();
for (let i = 0; i < 500; i++) {
  let result = "1" + "2" + "3" + "4" + "5" + "6" + "7" + "8" + "9" + "10";
}
elapsed = Date.now() - start;
console.log("  500 chained concatenations (10 parts): " + elapsed + "ms");

// Test 4: Type coercion concatenation
console.log("\nTest 4: Type Coercion in Concatenation");
start = Date.now();
for (let i = 0; i < 1000; i++) {
  let result = "Value: " + i + " is " + (i % 2 == 0 ? "even" : "odd");
}
elapsed = Date.now() - start;
console.log("  1000 concatenations with type coercion: " + elapsed + "ms");

// Test 5: Building longer strings progressively
console.log("\nTest 5: Progressive String Building");
start = Date.now();
let message = "";
for (let i = 0; i < 100; i++) {
  message = message + "line " + i + ": some content\n";
}
elapsed = Date.now() - start;
console.log("  100 progressive concatenations: " + elapsed + "ms");
console.log("  Result length: " + message.length + " bytes");

// Test 6: Concatenation in loop with variables
console.log("\nTest 6: Variable Concatenation in Loop");
let prefix = "Item";
let suffix = "end";
start = Date.now();
for (let i = 0; i < 1000; i++) {
  let result = prefix + " " + i + " " + suffix;
}
elapsed = Date.now() - start;
console.log("  1000 variable concatenations: " + elapsed + "ms");

// Test 7: Large string concatenation
console.log("\nTest 7: Large String Concatenation");
let largeStr1 = "x".repeat(500);
let largeStr2 = "y".repeat(500);
start = Date.now();
for (let i = 0; i < 100; i++) {
  let result = largeStr1 + largeStr2;
}
elapsed = Date.now() - start;
console.log("  100 concatenations of 500-byte strings: " + elapsed + "ms");

// Test 8: Mixed type chaining
console.log("\nTest 8: Mixed Type Chaining");
start = Date.now();
for (let i = 0; i < 1000; i++) {
  let result = "Start" + 123 + true + 45.67 + "End";
}
elapsed = Date.now() - start;
console.log("  1000 mixed-type chained concatenations: " + elapsed + "ms");

// Test 9: Template-like concatenation
console.log("\nTest 9: Template-like String Building");
start = Date.now();
for (let i = 0; i < 100; i++) {
  let name = "User" + i;
  let age = 20 + i;
  let result = "Name: " + name + ", Age: " + age + ", Active: " + (i % 2 == 0);
}
elapsed = Date.now() - start;
console.log("  100 template-like concatenations: " + elapsed + "ms");

// Test 10: String concatenation with comparison
console.log("\nTest 10: Concatenation with String Comparison");
start = Date.now();
for (let i = 0; i < 1000; i++) {
  let str1 = "test" + i;
  let str2 = "test" + i;
  let isEqual = str1 == str2 ? "equal" : "not equal";
}
elapsed = Date.now() - start;
console.log("  1000 concatenations + comparisons: " + elapsed + "ms");

// Verification test - ensure correctness
console.log("\n=== Correctness Verification ===");
let verify1 = "a" + "b" + "c" + "d";
console.log("Chained concat result: '" + verify1 + "' (expected: 'abcd')");

let verify2 = "x" + 42 + "y";
console.log("Type coercion result: '" + verify2 + "' (expected: 'x42y')");

let verify3 = "" + 0 + "" + false + "" + true;
console.log("Boolean concat result: '" + verify3 + "' (expected: '0falsetrue')");

console.log("\n=== All performance tests completed ===");
