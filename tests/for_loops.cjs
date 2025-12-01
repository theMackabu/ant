// Test for loop functionality

console.log("=== For Loop Tests ===\n");

// Test 1: Basic for loop
console.log("Test 1: Basic for loop");
let sum1 = 0;
for (let i = 0; i < 5; i = i + 1) {
  sum1 = sum1 + i;
}
console.log("  Sum of 0-4:", sum1);

// Test 2: For loop with const inside
console.log("\nTest 2: For loop with const declaration");
let sum2 = 0;
for (let i = 0; i < 3; i = i + 1) {
  const doubled = i * 2;
  sum2 = sum2 + doubled;
  console.log("  i:", i, "doubled:", doubled);
}
console.log("  Total sum:", sum2);

// Test 3: Nested for loops
console.log("\nTest 3: Nested for loops");
let product = 1;
for (let i = 1; i <= 3; i = i + 1) {
  for (let j = 1; j <= 2; j = j + 1) {
    product = product * (i + j);
    console.log("  i:", i, "j:", j, "product:", product);
  }
}
console.log("  Final product:", product);

// Test 4: Return from for loop in function
console.log("\nTest 4: Return from for loop");
function findFirst(arr, target) {
  for (let i = 0; i < arr.length; i = i + 1) {
    if (arr[i] === target) {
      return i;
    }
  }
  return -1;
}

const numbers = [10, 20, 30, 40, 50];
console.log("  Array:", numbers);
console.log("  Index of 30:", findFirst(numbers, 30));
console.log("  Index of 99:", findFirst(numbers, 99));

// Test 5: Break in for loop
console.log("\nTest 5: Break in for loop");
let breakSum = 0;
for (let i = 0; i < 10; i = i + 1) {
  if (i === 5) {
    console.log("  Breaking at i =", i);
    break;
  }
  breakSum = breakSum + i;
}
console.log("  Sum before break:", breakSum);

// Test 6: Continue in for loop
console.log("\nTest 6: Continue in for loop");
let evenSum = 0;
for (let i = 0; i < 10; i = i + 1) {
  if (i % 2 === 1) {
    continue;
  }
  evenSum = evenSum + i;
}
console.log("  Sum of even numbers 0-9:", evenSum);

// Test 7: For loop with empty initialization
console.log("\nTest 7: For loop with external initialization");
let k = 0;
let count = 0;
for (; k < 5; k = k + 1) {
  count = count + 1;
}
console.log("  Count:", count, "k:", k);

// Test 8: For loop with empty condition (infinite loop with break)
console.log("\nTest 8: For loop with break condition");
let limit = 0;
for (let i = 0; ; i = i + 1) {
  limit = i;
  if (i >= 3) {
    break;
  }
}
console.log("  Stopped at:", limit);

// Test 9: For loop with empty increment
console.log("\nTest 9: For loop with manual increment");
let manual = 0;
for (let i = 0; i < 5; ) {
  manual = manual + i;
  i = i + 2;
}
console.log("  Manual increment sum:", manual);

// Test 10: For loop with multiple variables
console.log("\nTest 10: For loop counting down");
let countdown = 0;
for (let i = 5; i > 0; i = i - 1) {
  countdown = countdown + i;
}
console.log("  Countdown sum:", countdown);

// Test 11: For loop modifying array
console.log("\nTest 11: For loop modifying array");
const arr = [1, 2, 3, 4, 5];
for (let i = 0; i < arr.length; i = i + 1) {
  arr[i] = arr[i] * 2;
}
console.log("  Doubled array:", arr);

// Test 12: For loop with object properties
console.log("\nTest 12: For loop with object");
const obj = { a: 1, b: 2, c: 3 };
const keys = Object.keys(obj);
let objSum = 0;
for (let i = 0; i < keys.length; i = i + 1) {
  const key = keys[i];
  objSum = objSum + obj[key];
  console.log("  key:", key, "value:", obj[key]);
}
console.log("  Object sum:", objSum);

// Test 13: For loop with string iteration
console.log("\nTest 13: For loop iterating string");
const str = "hello";
let chars = "";
for (let i = 0; i < str.length; i = i + 1) {
  chars = chars + str[i] + " ";
}
console.log("  Characters:", chars);

// Test 14: For loop with early return in nested function
console.log("\nTest 14: Early return in nested loops");
function findPair(arr, target) {
  for (let i = 0; i < arr.length; i = i + 1) {
    for (let j = i + 1; j < arr.length; j = j + 1) {
      if (arr[i] + arr[j] === target) {
        return { i: i, j: j, sum: target };
      }
    }
  }
  return null;
}

const testArr = [1, 2, 3, 4, 5];
const result = findPair(testArr, 7);
if (result) {
  console.log("  Found pair at indices", result.i, "and", result.j);
  console.log("  Values:", testArr[result.i], "+", testArr[result.j], "=", result.sum);
}

// Test 15: For loop scope isolation
console.log("\nTest 15: For loop scope isolation");
const results = [];
for (let i = 0; i < 3; i = i + 1) {
  const value = i * 10;
  results.push(value);
}
console.log("  Results array:", results);

// Test 16: Complex condition in for loop
console.log("\nTest 16: Complex condition");
let complexSum = 0;
for (let i = 0; i < 20 && complexSum < 50; i = i + 1) {
  complexSum = complexSum + i;
}
console.log("  Sum stopped at:", complexSum);

// Test 17: For loop with function calls
console.log("\nTest 17: For loop with function calls");
function square(n) {
  return n * n;
}

let squareSum = 0;
for (let i = 1; i <= 4; i = i + 1) {
  squareSum = squareSum + square(i);
}
console.log("  Sum of squares 1-4:", squareSum);

// Test 18: Return object from loop
console.log("\nTest 18: Return object from loop");
function findObject(arr, id) {
  for (let i = 0; i < arr.length; i = i + 1) {
    if (arr[i].id === id) {
      return arr[i];
    }
  }
  return null;
}

const items = [
  { id: 1, name: "first" },
  { id: 2, name: "second" },
  { id: 3, name: "third" }
];

const found = findObject(items, 2);
console.log("  Found item:", found ? found.name : "null");

// Test 19: For loop with undefined check
console.log("\nTest 19: For loop with undefined check");
const sparse = [1, 2, undefined, 4, 5];
let definedCount = 0;
for (let i = 0; i < sparse.length; i = i + 1) {
  if (sparse[i] !== undefined) {
    definedCount = definedCount + 1;
  }
}
console.log("  Defined elements:", definedCount);

// Test 20: Performance test
console.log("\nTest 20: Large loop");
let largeSum = 0;
for (let i = 0; i < 1000; i = i + 1) {
  largeSum = largeSum + i;
}
console.log("  Sum of 0-999:", largeSum);

console.log("\n=== All tests completed ===");
