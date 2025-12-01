// Simple test for optional chaining

Ant.println("Test 1: Basic optional chaining");
const value = undefined;
const result = value?.thing;
Ant.println("value?.thing:", result);

Ant.println("\nTest 2: With object");
const obj = { nested: { deep: "value" } };
const result2 = obj?.nested?.deep;
Ant.println("obj?.nested?.deep:", result2);

Ant.println("\nTest 3: In if statement");
if (value?.thing) {
  Ant.println("FAIL");
} else {
  Ant.println("PASS: value?.thing is falsy");
}

Ant.println("\nAll tests done");
