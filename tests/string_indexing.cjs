// Test string indexing feature

console.log("=== String Indexing Test ===\n");

// Test 1: Basic string indexing
console.log("Test 1: Basic indexing");
let str = "hello";
console.log("  str = 'hello'");
console.log("  str[0]:", str[0]);
console.log("  str[1]:", str[1]);
console.log("  str[2]:", str[2]);
console.log("  str[3]:", str[3]);
console.log("  str[4]:", str[4]);

// Test 2: Last character access
console.log("\nTest 2: Last character");
let path = "hello/world";
console.log("  path = 'hello/world'");
console.log("  path.length:", path.length);
let lastIdx = path.length - 1;
console.log("  path[path.length - 1]:", path[lastIdx]);

// Test 3: Direct expression in brackets
console.log("\nTest 3: Expression in brackets");
console.log("  path[path.length - 1]:", path[path.length - 1]);
console.log("  path[5]:", path[5]);
console.log("  path[0]:", path[0]);

// Test 4: Out of bounds access
console.log("\nTest 4: Out of bounds");
console.log("  str[100] (should be undefined):", str[100]);
console.log("  str[-1] (should be undefined):", str[-1]);

// Test 5: Looping through string characters
console.log("\nTest 5: Loop through string");
let word = "test";
console.log("  word = 'test'");
for (let i = 0; i < word.length; i = i + 1) {
  console.log("    word[" + i + "]:", word[i]);
}

// Test 6: String comparison with indexing
console.log("\nTest 6: Character comparison");
let s1 = "abc";
let s2 = "xyz";
console.log("  s1 = 'abc', s2 = 'xyz'");
console.log("  s1[0] == s2[0]:", s1[0] == s2[0]);
console.log("  s1[0] != s2[0]:", s1[0] != s2[0]);

// Test 7: Building strings from characters
console.log("\nTest 7: String building");
let original = "hello";
let reversed = "";
for (let i = original.length - 1; i >= 0; i = i - 1) {
  reversed = reversed + original[i];
}
console.log("  original:", original);
console.log("  reversed:", reversed);

// Test 8: First and last character check
console.log("\nTest 8: First and last character");
let url = "/api/users/";
console.log("  url = '/api/users/'");
console.log("  First char (url[0]):", url[0]);
console.log("  Last char (url[url.length - 1]):", url[url.length - 1]);
console.log("  Starts with '/':", url[0] === "/");
console.log("  Ends with '/':", url[url.length - 1] === "/");

// Test 9: Middle character access
console.log("\nTest 9: Middle character");
let text = "abcdefgh";
let mid = text.length / 2;
console.log("  text = 'abcdefgh'");
console.log("  Middle index:", mid);
console.log("  text[mid]:", text[mid]);

// Test 10: Empty string
console.log("\nTest 10: Empty string");
let empty = "";
console.log("  empty.length:", empty.length);
console.log("  empty[0] (should be undefined):", empty[0]);

// Test 11: Single character string
console.log("\nTest 11: Single character");
let single = "x";
console.log("  single = 'x'");
console.log("  single[0]:", single[0]);
console.log("  single[1] (should be undefined):", single[1]);

// Test 12: Path manipulation example
console.log("\nTest 12: Path manipulation");
let filePath = "/home/user/file.txt";
console.log("  filePath = '/home/user/file.txt'");
let hasTrailingSlash = filePath[filePath.length - 1] === "/";
console.log("  Has trailing slash:", hasTrailingSlash);
let hasLeadingSlash = filePath[0] === "/";
console.log("  Has leading slash:", hasLeadingSlash);

console.log("\n=== All tests completed ===");
