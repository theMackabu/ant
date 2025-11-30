// Test arrow function IIFE (Immediately Invoked Function Expression)

// Basic IIFE with no parameters
let result1 = (() => 43)();
Ant.println("Basic IIFE:", result1); // Should be 43

// IIFE with expression body
let result2 = (() => 10 + 5)();
Ant.println("Expression IIFE:", result2); // Should be 15

// IIFE with block body
let result3 = (() => { return 99; })();
Ant.println("Block IIFE:", result3); // Should be 99

// IIFE with parameters
let result4 = ((x) => x * 2)(21);
Ant.println("IIFE with param:", result4); // Should be 42

// IIFE with multiple parameters
let result5 = ((a, b) => a + b)(30, 12);
Ant.println("IIFE with multiple params:", result5); // Should be 42

// Nested IIFE
let result6 = (() => (() => 7)())();
Ant.println("Nested IIFE:", result6); // Should be 7

// IIFE returning object (like module pattern)
let counter = (() => {
  let count = 0;
  return {
    increment: () => { count = count + 1; return count; },
    get: () => count
  };
})();

Ant.println("Counter initial:", counter.get()); // Should be 0
Ant.println("Counter after increment:", counter.increment()); // Should be 1
Ant.println("Counter after increment:", counter.increment()); // Should be 2
Ant.println("Counter value:", counter.get()); // Should be 2

// Complex expression in IIFE
let result7 = ((x, y) => x * 2 + y * 3)(5, 10);
Ant.println("Complex expression:", result7); // Should be 40

Ant.println("\nAll arrow function IIFE tests completed!");
