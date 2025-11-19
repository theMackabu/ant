// Test string indexing feature

Ant.println("=== String Indexing Test ===\n");

// Test 1: Basic string indexing
Ant.println("Test 1: Basic indexing");
let str = "hello";
Ant.println("  str = 'hello'");
Ant.println("  str[0]:", str[0]);
Ant.println("  str[1]:", str[1]);
Ant.println("  str[2]:", str[2]);
Ant.println("  str[3]:", str[3]);
Ant.println("  str[4]:", str[4]);

// Test 2: Last character access
Ant.println("\nTest 2: Last character");
let path = "hello/world";
Ant.println("  path = 'hello/world'");
Ant.println("  path.length:", path.length);
let lastIdx = path.length - 1;
Ant.println("  path[path.length - 1]:", path[lastIdx]);

// Test 3: Direct expression in brackets
Ant.println("\nTest 3: Expression in brackets");
Ant.println("  path[path.length - 1]:", path[path.length - 1]);
Ant.println("  path[5]:", path[5]);
Ant.println("  path[0]:", path[0]);

// Test 4: Out of bounds access
Ant.println("\nTest 4: Out of bounds");
Ant.println("  str[100] (should be undefined):", str[100]);
Ant.println("  str[-1] (should be undefined):", str[-1]);

// Test 5: Looping through string characters
Ant.println("\nTest 5: Loop through string");
let word = "test";
Ant.println("  word = 'test'");
for (let i = 0; i < word.length; i = i + 1) {
  Ant.println("    word[" + i + "]:", word[i]);
}

// Test 6: String comparison with indexing
Ant.println("\nTest 6: Character comparison");
let s1 = "abc";
let s2 = "xyz";
Ant.println("  s1 = 'abc', s2 = 'xyz'");
Ant.println("  s1[0] == s2[0]:", s1[0] == s2[0]);
Ant.println("  s1[0] != s2[0]:", s1[0] != s2[0]);

// Test 7: Building strings from characters
Ant.println("\nTest 7: String building");
let original = "hello";
let reversed = "";
for (let i = original.length - 1; i >= 0; i = i - 1) {
  reversed = reversed + original[i];
}
Ant.println("  original:", original);
Ant.println("  reversed:", reversed);

// Test 8: First and last character check
Ant.println("\nTest 8: First and last character");
let url = "/api/users/";
Ant.println("  url = '/api/users/'");
Ant.println("  First char (url[0]):", url[0]);
Ant.println("  Last char (url[url.length - 1]):", url[url.length - 1]);
Ant.println("  Starts with '/':", url[0] === "/");
Ant.println("  Ends with '/':", url[url.length - 1] === "/");

// Test 9: Middle character access
Ant.println("\nTest 9: Middle character");
let text = "abcdefgh";
let mid = text.length / 2;
Ant.println("  text = 'abcdefgh'");
Ant.println("  Middle index:", mid);
Ant.println("  text[mid]:", text[mid]);

// Test 10: Empty string
Ant.println("\nTest 10: Empty string");
let empty = "";
Ant.println("  empty.length:", empty.length);
Ant.println("  empty[0] (should be undefined):", empty[0]);

// Test 11: Single character string
Ant.println("\nTest 11: Single character");
let single = "x";
Ant.println("  single = 'x'");
Ant.println("  single[0]:", single[0]);
Ant.println("  single[1] (should be undefined):", single[1]);

// Test 12: Path manipulation example
Ant.println("\nTest 12: Path manipulation");
let filePath = "/home/user/file.txt";
Ant.println("  filePath = '/home/user/file.txt'");
let hasTrailingSlash = filePath[filePath.length - 1] === "/";
Ant.println("  Has trailing slash:", hasTrailingSlash);
let hasLeadingSlash = filePath[0] === "/";
Ant.println("  Has leading slash:", hasLeadingSlash);

Ant.println("\n=== All tests completed ===");
