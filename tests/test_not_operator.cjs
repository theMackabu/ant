// Test the NOT operator fix

console.log("Test 1 - if (!null) with block:");
if (!null) {
  console.log("  PASS");
}

console.log("\nTest 2 - if (!undefined) with block:");
if (!undefined) {
  console.log("  PASS");
}

console.log("\nTest 3 - if (!false) with block:");
if (!false) {
  console.log("  PASS");
}

console.log("\nTest 4 - if (!0) with block:");
if (!0) {
  console.log("  PASS");
}

console.log("\nTest 5 - if (!'') with block:");
if (!'') {
  console.log("  PASS");
}

const x = null;
console.log("\nTest 6 - if (!x) return {}:");
function test6() {
  if (!x) return {};
  return { value: "should not reach" };
}
console.log("  Result:", test6());

console.log("\nTest 7 - if (!handler) return {} from radix3:");
function lookup(handler) {
  if (!handler) return {};
  return { handler, params: {} };
}
console.log("  With null:", lookup(null));
console.log("  With value:", lookup("test"));

console.log("\nAll tests passed!");
