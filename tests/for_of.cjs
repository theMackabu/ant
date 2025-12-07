// Test for-of loop functionality

console.log("=== For-Of Loop Tests ===\n");

// Test 1: Basic for-of with array
console.log("Test 1: Basic for-of with array");
const fruits = ["apple", "banana", "cherry"];
let collected = [];
for (const fruit of fruits) {
  collected.push(fruit);
}
console.log("  Collected:", collected);

// Test 2: for-of with Object.keys()
console.log("\nTest 2: for-of with Object.keys()");
const person = { name: "Alice", age: 30, city: "NYC" };
let keys = [];
for (const key of Object.keys(person)) {
  keys.push(key);
}
console.log("  Keys:", keys);

// Test 3: for-of with Object.keys() accessing values
console.log("\nTest 3: for-of accessing object values");
const scores = { math: 95, english: 88, science: 92 };
let total = 0;
for (const key of Object.keys(scores)) {
  console.log("  " + key + ":", scores[key]);
  total = total + scores[key];
}
console.log("  Total:", total);

// Test 4: for-of with let (mutable variable)
console.log("\nTest 4: for-of with let");
const numbers = [1, 2, 3, 4, 5];
let sum = 0;
for (let num of numbers) {
  num = num * 2;
  sum = sum + num;
}
console.log("  Sum of doubled:", sum);

// Test 5: for-of with string iteration
console.log("\nTest 5: for-of with string");
const str = "hello";
let chars = [];
for (const char of str) {
  chars.push(char);
}
console.log("  Characters:", chars);

// Test 6: Nested for-of loops
console.log("\nTest 6: Nested for-of loops");
const matrix = [[1, 2], [3, 4], [5, 6]];
let flatSum = 0;
for (const row of matrix) {
  for (const val of row) {
    flatSum = flatSum + val;
  }
}
console.log("  Matrix sum:", flatSum);

// Test 7: for-of with break
console.log("\nTest 7: for-of with break");
const items = [10, 20, 30, 40, 50];
let breakSum = 0;
for (const item of items) {
  if (item > 25) {
    console.log("  Breaking at:", item);
    break;
  }
  breakSum = breakSum + item;
}
console.log("  Sum before break:", breakSum);

// Test 8: for-of with continue
console.log("\nTest 8: for-of with continue");
const mixed = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
let evenSum = 0;
for (const n of mixed) {
  if (n % 2 === 1) {
    continue;
  }
  evenSum = evenSum + n;
}
console.log("  Sum of evens:", evenSum);

// Test 9: for-of in function with return
console.log("\nTest 9: for-of with return");
function findValue(arr, target) {
  for (const val of arr) {
    if (val === target) {
      return true;
    }
  }
  return false;
}
console.log("  Contains 30:", findValue([10, 20, 30, 40], 30));
console.log("  Contains 99:", findValue([10, 20, 30, 40], 99));

// Test 10: for-of with empty array
console.log("\nTest 10: for-of with empty array");
const empty = [];
let emptyCount = 0;
for (const x of empty) {
  emptyCount = emptyCount + 1;
}
console.log("  Iterations on empty:", emptyCount);

// Test 11: for-of building new array
console.log("\nTest 11: for-of building new array");
const source = [1, 2, 3, 4, 5];
const squared = [];
for (const n of source) {
  squared.push(n * n);
}
console.log("  Squared:", squared);

// Test 12: for-of with complex objects in array
console.log("\nTest 12: for-of with objects in array");
const users = [
  { id: 1, name: "Alice" },
  { id: 2, name: "Bob" },
  { id: 3, name: "Charlie" }
];
let names = [];
for (const user of users) {
  names.push(user.name);
}
console.log("  Names:", names);

// Test 13: for-of with Object.keys() building result object
console.log("\nTest 13: for-of building result object");
const original = { a: 1, b: 2, c: 3 };
const doubled = {};
for (const key of Object.keys(original)) {
  doubled[key] = original[key] * 2;
}
console.log("  Doubled object:", doubled);

// Test 14: for-of scope isolation
console.log("\nTest 14: for-of scope isolation");
const vals = [100, 200, 300];
const results = [];
for (const v of vals) {
  const computed = v / 10;
  results.push(computed);
}
console.log("  Results:", results);

// Test 15: Chained for-of operations
console.log("\nTest 15: Chained for-of");
const data = { x: 10, y: 20, z: 30 };
const dataKeys = Object.keys(data);
let product = 1;
for (const k of dataKeys) {
  product = product * data[k];
}
console.log("  Product of values:", product);

console.log("\n=== All for-of tests completed ===");
