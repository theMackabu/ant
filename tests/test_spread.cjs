// Test spread operator in function calls

// Test 1: Basic spread into function
function sum(a, b, c) {
  return a + b + c;
}
let arr = [1, 2, 3];
console.log("Test 1 - Basic spread:");
console.log(sum(...arr)); // Should print 6

// Test 2: Spread with regular args before
function greet(prefix, a, b) {
  return prefix + ": " + a + " and " + b;
}
let names = ["Alice", "Bob"];
console.log("\nTest 2 - Spread with prefix arg:");
console.log(greet("Hello", ...names)); // Should print "Hello: Alice and Bob"

// Test 3: Spread with regular args after
function build(a, b, suffix) {
  return a + "-" + b + "-" + suffix;
}
let parts = ["x", "y"];
console.log("\nTest 3 - Spread with suffix arg:");
console.log(build(...parts, "z")); // Should print "x-y-z"

// Test 4: Multiple spreads
function concat(a, b, c, d) {
  return a + b + c + d;
}
let first = [1, 2];
let second = [3, 4];
console.log("\nTest 4 - Multiple spreads:");
console.log(concat(...first, ...second)); // Should print 10

// Test 5: Spread into rest parameter function
function collectAll(...items) {
  let total = 0;
  for (let i = 0; i < items.length; i++) {
    total = total + items[i];
  }
  return total;
}
let nums = [10, 20, 30];
console.log("\nTest 5 - Spread into rest param:");
console.log(collectAll(...nums)); // Should print 60

// Test 6: Spread empty array
function countArgs(...args) {
  return args.length;
}
let empty = [];
console.log("\nTest 6 - Spread empty array:");
console.log(countArgs(...empty)); // Should print 0

// Test 7: Spread with console.log (C function)
let values = ["a", "b", "c"];
console.log("\nTest 7 - Spread to console.log:");
console.log(...values); // Should print "a b c"
