// Test the NOT operator fix

Ant.println("Test 1 - if (!null) with block:");
if (!null) {
  Ant.println("  PASS");
}

Ant.println("\nTest 2 - if (!undefined) with block:");
if (!undefined) {
  Ant.println("  PASS");
}

Ant.println("\nTest 3 - if (!false) with block:");
if (!false) {
  Ant.println("  PASS");
}

Ant.println("\nTest 4 - if (!0) with block:");
if (!0) {
  Ant.println("  PASS");
}

Ant.println("\nTest 5 - if (!'') with block:");
if (!'') {
  Ant.println("  PASS");
}

const x = null;
Ant.println("\nTest 6 - if (!x) return {}:");
function test6() {
  if (!x) return {};
  return { value: "should not reach" };
}
Ant.println("  Result:", test6());

Ant.println("\nTest 7 - if (!handler) return {} from radix3:");
function lookup(handler) {
  if (!handler) return {};
  return { handler, params: {} };
}
Ant.println("  With null:", lookup(null));
Ant.println("  With value:", lookup("test"));

Ant.println("\nAll tests passed!");
