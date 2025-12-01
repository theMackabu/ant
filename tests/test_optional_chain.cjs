// Simple test for optional chaining

console.log("Test 1: Basic optional chaining");
const value = undefined;
const result = value?.thing;
console.log("value?.thing:", result);

console.log("\nTest 2: With object");
const obj = { nested: { deep: "value" } };
const result2 = obj?.nested?.deep;
console.log("obj?.nested?.deep:", result2);

console.log("\nTest 3: In if statement");
if (value?.thing) {
  console.log("FAIL");
} else {
  console.log("PASS: value?.thing is falsy");
}

console.log("\nAll tests done");
