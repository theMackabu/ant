// Test Array methods functionality

Ant.println("=== Array Methods Tests ===\n");

// Test 1: push and pop (existing)
Ant.println("Test 1: push() and pop()");
let arr1 = [1, 2, 3];
Ant.println("  Initial:", arr1);
arr1.push(4);
Ant.println("  After push(4):", arr1);
arr1.push(5, 6);
Ant.println("  After push(5, 6):", arr1);
let popped = arr1.pop();
Ant.println("  Popped value:", popped);
Ant.println("  After pop():", arr1);

// Test 2: slice
Ant.println("\nTest 2: slice()");
let arr2 = [10, 20, 30, 40, 50];
Ant.println("  Original:", arr2);
Ant.println("  slice(1, 3):", arr2.slice(1, 3));
Ant.println("  slice(2):", arr2.slice(2));
Ant.println("  slice(-2):", arr2.slice(-2));
Ant.println("  slice(0, -2):", arr2.slice(0, -2));
Ant.println("  slice(-4, -1):", arr2.slice(-4, -1));
Ant.println("  Original unchanged:", arr2);

// Test 3: join
Ant.println("\nTest 3: join()");
let arr3 = ["apple", "banana", "cherry"];
Ant.println("  Array:", arr3);
Ant.println("  join(', '):", arr3.join(", "));
Ant.println("  join(' - '):", arr3.join(" - "));
Ant.println("  join(''):", arr3.join(""));
Ant.println("  join():", arr3.join());

let numbers = [1, 2, 3, 4, 5];
Ant.println("  Numbers:", numbers);
Ant.println("  join('-'):", numbers.join("-"));

// Test 4: includes
Ant.println("\nTest 4: includes()");
let arr4 = [1, 2, 3, 4, 5];
Ant.println("  Array:", arr4);
Ant.println("  includes(3):", arr4.includes(3));
Ant.println("  includes(6):", arr4.includes(6));
Ant.println("  includes(1):", arr4.includes(1));
Ant.println("  includes(5):", arr4.includes(5));
Ant.println("  includes(0):", arr4.includes(0));

// Test 5: includes with different types
Ant.println("\nTest 5: includes() with different types");
let mixed = [1, "hello", true, false, 42];
Ant.println("  Array:", mixed);
Ant.println("  includes(1):", mixed.includes(1));
Ant.println("  includes('hello'):", mixed.includes("hello"));
Ant.println("  includes('Hello'):", mixed.includes("Hello"));
Ant.println("  includes(true):", mixed.includes(true));
Ant.println("  includes(false):", mixed.includes(false));
Ant.println("  includes(42):", mixed.includes(42));
Ant.println("  includes(0):", mixed.includes(0));

// Test 6: Combining methods
Ant.println("\nTest 6: Combining array methods");
let data = [10, 20, 30, 40, 50, 60, 70];
Ant.println("  Original:", data);
let subset = data.slice(2, 5);
Ant.println("  slice(2, 5):", subset);
Ant.println("  Joined:", subset.join(" -> "));
Ant.println("  includes(40):", subset.includes(40));
Ant.println("  includes(10):", subset.includes(10));

// Test 7: Building CSV
Ant.println("\nTest 7: Building CSV");
let headers = ["Name", "Age", "City"];
let row1 = ["Alice", "25", "NYC"];
let row2 = ["Bob", "30", "LA"];
Ant.println("  " + headers.join(","));
Ant.println("  " + row1.join(","));
Ant.println("  " + row2.join(","));

// Test 8: Array manipulation
Ant.println("\nTest 8: Array manipulation");
let queue = [];
Ant.println("  Initial queue:", queue);
queue.push("task1");
queue.push("task2");
queue.push("task3");
Ant.println("  After adding tasks:", queue);
let first = queue.slice(0, 1);
Ant.println("  First task:", first);
let remaining = queue.slice(1);
Ant.println("  Remaining:", remaining);

// Test 9: Finding elements
Ant.println("\nTest 9: Finding elements");
let items = ["apple", "banana", "cherry", "date"];
Ant.println("  Items:", items);
if (items.includes("banana")) {
  Ant.println("  Found 'banana' in the list");
}
if (!items.includes("grape")) {
  Ant.println("  'grape' not found in the list");
}

// Test 10: Slice for copy
Ant.println("\nTest 10: Copying array with slice");
let original = [1, 2, 3, 4, 5];
let copy = original.slice();
Ant.println("  Original:", original);
Ant.println("  Copy:", copy);
copy.push(6);
Ant.println("  After push to copy:", copy);
Ant.println("  Original unchanged:", original);

// Test 11: Working with ranges
Ant.println("\nTest 11: Working with ranges");
let range = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];
Ant.println("  Full range:", range);
Ant.println("  First 3:", range.slice(0, 3));
Ant.println("  Last 3:", range.slice(-3));
Ant.println("  Middle:", range.slice(3, 7));

// Test 12: Edge cases
Ant.println("\nTest 12: Edge cases");
let empty = [];
Ant.println("  Empty array:", empty);
Ant.println("  slice():", empty.slice());
Ant.println("  join(','):", "'" + empty.join(",") + "'");
Ant.println("  includes(1):", empty.includes(1));
let popped_empty = empty.pop();
Ant.println("  pop() on empty:", popped_empty);

let single = [42];
Ant.println("  Single element [42]:");
Ant.println("  slice():", single.slice());
Ant.println("  join():", single.join(","));
Ant.println("  includes(42):", single.includes(42));
Ant.println("  includes(0):", single.includes(0));

// Test 13: Practical example - path segments
Ant.println("\nTest 13: Path processing");
let pathSegments = ["home", "user", "documents", "file.txt"];
Ant.println("  Segments:", pathSegments);
let path = "/" + pathSegments.join("/");
Ant.println("  Path:", path);
let filename = pathSegments.slice(-1);
Ant.println("  Filename:", filename);
let directory = pathSegments.slice(0, -1);
Ant.println("  Directory parts:", directory);

// Test 14: Data filtering (manual)
Ant.println("\nTest 14: Manual filtering");
let scores = [85, 92, 78, 95, 88, 73, 91];
Ant.println("  All scores:", scores);
let highScores = [];
for (let i = 0; i < scores.length; i++) {
  if (scores[i] >= 90) {
    highScores.push(scores[i]);
  }
}
Ant.println("  High scores (>= 90):", highScores);
Ant.println("  Joined:", highScores.join(", "));

// Test 15: Checking membership
Ant.println("\nTest 15: Membership checking");
let allowedUsers = ["admin", "user1", "user2", "guest"];
let username = "user1";
Ant.println("  Allowed users:", allowedUsers);
Ant.println("  Check '" + username + "':", allowedUsers.includes(username));
username = "hacker";
Ant.println("  Check '" + username + "':", allowedUsers.includes(username));

Ant.println("\n=== All tests completed ===");
