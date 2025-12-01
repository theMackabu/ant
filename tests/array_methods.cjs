// Test Array methods functionality

console.log("=== Array Methods Tests ===\n");

// Test 1: push and pop (existing)
console.log("Test 1: push() and pop()");
let arr1 = [1, 2, 3];
console.log("  Initial:", arr1);
arr1.push(4);
console.log("  After push(4):", arr1);
arr1.push(5, 6);
console.log("  After push(5, 6):", arr1);
let popped = arr1.pop();
console.log("  Popped value:", popped);
console.log("  After pop():", arr1);

// Test 2: slice
console.log("\nTest 2: slice()");
let arr2 = [10, 20, 30, 40, 50];
console.log("  Original:", arr2);
console.log("  slice(1, 3):", arr2.slice(1, 3));
console.log("  slice(2):", arr2.slice(2));
console.log("  slice(-2):", arr2.slice(-2));
console.log("  slice(0, -2):", arr2.slice(0, -2));
console.log("  slice(-4, -1):", arr2.slice(-4, -1));
console.log("  Original unchanged:", arr2);

// Test 3: join
console.log("\nTest 3: join()");
let arr3 = ["apple", "banana", "cherry"];
console.log("  Array:", arr3);
console.log("  join(', '):", arr3.join(", "));
console.log("  join(' - '):", arr3.join(" - "));
console.log("  join(''):", arr3.join(""));
console.log("  join():", arr3.join());

let numbers = [1, 2, 3, 4, 5];
console.log("  Numbers:", numbers);
console.log("  join('-'):", numbers.join("-"));

// Test 4: includes
console.log("\nTest 4: includes()");
let arr4 = [1, 2, 3, 4, 5];
console.log("  Array:", arr4);
console.log("  includes(3):", arr4.includes(3));
console.log("  includes(6):", arr4.includes(6));
console.log("  includes(1):", arr4.includes(1));
console.log("  includes(5):", arr4.includes(5));
console.log("  includes(0):", arr4.includes(0));

// Test 5: includes with different types
console.log("\nTest 5: includes() with different types");
let mixed = [1, "hello", true, false, 42];
console.log("  Array:", mixed);
console.log("  includes(1):", mixed.includes(1));
console.log("  includes('hello'):", mixed.includes("hello"));
console.log("  includes('Hello'):", mixed.includes("Hello"));
console.log("  includes(true):", mixed.includes(true));
console.log("  includes(false):", mixed.includes(false));
console.log("  includes(42):", mixed.includes(42));
console.log("  includes(0):", mixed.includes(0));

// Test 6: Combining methods
console.log("\nTest 6: Combining array methods");
let data = [10, 20, 30, 40, 50, 60, 70];
console.log("  Original:", data);
let subset = data.slice(2, 5);
console.log("  slice(2, 5):", subset);
console.log("  Joined:", subset.join(" -> "));
console.log("  includes(40):", subset.includes(40));
console.log("  includes(10):", subset.includes(10));

// Test 7: Building CSV
console.log("\nTest 7: Building CSV");
let headers = ["Name", "Age", "City"];
let row1 = ["Alice", "25", "NYC"];
let row2 = ["Bob", "30", "LA"];
console.log("  " + headers.join(","));
console.log("  " + row1.join(","));
console.log("  " + row2.join(","));

// Test 8: Array manipulation
console.log("\nTest 8: Array manipulation");
let queue = [];
console.log("  Initial queue:", queue);
queue.push("task1");
queue.push("task2");
queue.push("task3");
console.log("  After adding tasks:", queue);
let first = queue.slice(0, 1);
console.log("  First task:", first);
let remaining = queue.slice(1);
console.log("  Remaining:", remaining);

// Test 9: Finding elements
console.log("\nTest 9: Finding elements");
let items = ["apple", "banana", "cherry", "date"];
console.log("  Items:", items);
if (items.includes("banana")) {
  console.log("  Found 'banana' in the list");
}
if (!items.includes("grape")) {
  console.log("  'grape' not found in the list");
}

// Test 10: Slice for copy
console.log("\nTest 10: Copying array with slice");
let original = [1, 2, 3, 4, 5];
let copy = original.slice();
console.log("  Original:", original);
console.log("  Copy:", copy);
copy.push(6);
console.log("  After push to copy:", copy);
console.log("  Original unchanged:", original);

// Test 11: Working with ranges
console.log("\nTest 11: Working with ranges");
let range = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];
console.log("  Full range:", range);
console.log("  First 3:", range.slice(0, 3));
console.log("  Last 3:", range.slice(-3));
console.log("  Middle:", range.slice(3, 7));

// Test 12: Edge cases
console.log("\nTest 12: Edge cases");
let empty = [];
console.log("  Empty array:", empty);
console.log("  slice():", empty.slice());
console.log("  join(','):", "'" + empty.join(",") + "'");
console.log("  includes(1):", empty.includes(1));
let popped_empty = empty.pop();
console.log("  pop() on empty:", popped_empty);

let single = [42];
console.log("  Single element [42]:");
console.log("  slice():", single.slice());
console.log("  join():", single.join(","));
console.log("  includes(42):", single.includes(42));
console.log("  includes(0):", single.includes(0));

// Test 13: Practical example - path segments
console.log("\nTest 13: Path processing");
let pathSegments = ["home", "user", "documents", "file.txt"];
console.log("  Segments:", pathSegments);
let path = "/" + pathSegments.join("/");
console.log("  Path:", path);
let filename = pathSegments.slice(-1);
console.log("  Filename:", filename);
let directory = pathSegments.slice(0, -1);
console.log("  Directory parts:", directory);

// Test 14: Data filtering (manual)
console.log("\nTest 14: Manual filtering");
let scores = [85, 92, 78, 95, 88, 73, 91];
console.log("  All scores:", scores);
let highScores = [];
for (let i = 0; i < scores.length; i++) {
  if (scores[i] >= 90) {
    highScores.push(scores[i]);
  }
}
console.log("  High scores (>= 90):", highScores);
console.log("  Joined:", highScores.join(", "));

// Test 15: Checking membership
console.log("\nTest 15: Membership checking");
let allowedUsers = ["admin", "user1", "user2", "guest"];
let username = "user1";
console.log("  Allowed users:", allowedUsers);
console.log("  Check '" + username + "':", allowedUsers.includes(username));
username = "hacker";
console.log("  Check '" + username + "':", allowedUsers.includes(username));

console.log("\n=== All tests completed ===");
