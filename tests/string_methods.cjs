// Test String methods functionality

console.log("=== String Methods Tests ===\n");

// Test 1: indexOf
console.log("Test 1: indexOf()");
let str1 = "hello world";
console.log("  '" + str1 + "'.indexOf('world'):", str1.indexOf("world"));
console.log("  '" + str1 + "'.indexOf('o'):", str1.indexOf("o"));
console.log("  '" + str1 + "'.indexOf('xyz'):", str1.indexOf("xyz"));
console.log("  '" + str1 + "'.indexOf(''):", str1.indexOf(""));

// Test 2: substring
console.log("\nTest 2: substring()");
let str2 = "JavaScript";
console.log("  '" + str2 + "'.substring(0, 4):", str2.substring(0, 4));
console.log("  '" + str2 + "'.substring(4):", str2.substring(4));
console.log("  '" + str2 + "'.substring(4, 10):", str2.substring(4, 10));
console.log("  '" + str2 + "'.substring(10, 4):", str2.substring(10, 4)); // Should swap

// Test 3: slice
console.log("\nTest 3: slice()");
let str3 = "The quick brown fox";
console.log("  '" + str3 + "'.slice(0, 3):", str3.slice(0, 3));
console.log("  '" + str3 + "'.slice(4, 9):", str3.slice(4, 9));
console.log("  '" + str3 + "'.slice(10):", str3.slice(10));
console.log("  '" + str3 + "'.slice(-3):", str3.slice(-3));
console.log("  '" + str3 + "'.slice(0, -4):", str3.slice(0, -4));
console.log("  '" + str3 + "'.slice(-9, -4):", str3.slice(-9, -4));

// Test 4: split
console.log("\nTest 4: split()");
let str4 = "apple,banana,cherry";
let parts = str4.split(",");
console.log("  '" + str4 + "'.split(','):");
for (let i = 0; i < parts.length; i++) {
  console.log("    [" + i + "]: " + parts[i]);
}

let str5 = "hello";
let chars = str5.split("");
console.log("  '" + str5 + "'.split(''):");
for (let i = 0; i < chars.length; i++) {
  console.log("    [" + i + "]: " + chars[i]);
}

// Test 5: includes
console.log("\nTest 5: includes()");
let str6 = "The quick brown fox jumps over the lazy dog";
console.log("  '" + str6 + "'");
console.log("    includes('quick'):", str6.includes("quick"));
console.log("    includes('cat'):", str6.includes("cat"));
console.log("    includes('fox'):", str6.includes("fox"));
console.log("    includes(''):", str6.includes(""));
console.log("    includes('QUICK'):", str6.includes("QUICK"));

// Test 6: startsWith
console.log("\nTest 6: startsWith()");
let str7 = "Hello, World!";
console.log("  '" + str7 + "'");
console.log("    startsWith('Hello'):", str7.startsWith("Hello"));
console.log("    startsWith('World'):", str7.startsWith("World"));
console.log("    startsWith('H'):", str7.startsWith("H"));
console.log("    startsWith(''):", str7.startsWith(""));

// Test 7: endsWith
console.log("\nTest 7: endsWith()");
let str8 = "index.html";
console.log("  '" + str8 + "'");
console.log("    endsWith('.html'):", str8.endsWith(".html"));
console.log("    endsWith('.js'):", str8.endsWith(".js"));
console.log("    endsWith('html'):", str8.endsWith("html"));
console.log("    endsWith(''):", str8.endsWith(""));

// Test 8: Combining methods
console.log("\nTest 8: Combining methods");
let path = "/api/users/123/profile";
console.log("  Path:", path);
let segments = path.split("/");
console.log("  Segments:");
for (let i = 0; i < segments.length; i++) {
  if (segments[i] !== "") {
    console.log("    " + segments[i]);
  }
}

// Test 9: URL parsing example
console.log("\nTest 9: URL parsing");
let url = "https://example.com/path/to/resource";
if (url.startsWith("https://")) {
  console.log("  URL uses HTTPS");
  let domain = url.slice(8);
  let domainEnd = domain.indexOf("/");
  if (domainEnd !== -1) {
    let hostname = domain.substring(0, domainEnd);
    console.log("  Hostname:", hostname);
    let pathname = domain.slice(domainEnd);
    console.log("  Pathname:", pathname);
  }
}

// Test 10: String validation
console.log("\nTest 10: String validation");
let email = "user@example.com";
console.log("  Email:", email);
console.log("    Contains '@':", email.includes("@"));
let atIndex = email.indexOf("@");
if (atIndex !== -1) {
  let username = email.substring(0, atIndex);
  let domain = email.substring(atIndex + 1);
  console.log("    Username:", username);
  console.log("    Domain:", domain);
  console.log("    Domain ends with '.com':", domain.endsWith(".com"));
}

// Test 11: Edge cases
console.log("\nTest 11: Edge cases");
let empty = "";
console.log("  Empty string:");
console.log("    length:", empty.length);
console.log("    indexOf('x'):", empty.indexOf("x"));
console.log("    slice(0, 5):", "'" + empty.slice(0, 5) + "'");
console.log("    includes(''):", empty.includes(""));
console.log("    startsWith(''):", empty.startsWith(""));
console.log("    endsWith(''):", empty.endsWith(""));

let single = "a";
console.log("  Single char 'a':");
console.log("    slice(-1):", single.slice(-1));
console.log("    substring(0, 1):", single.substring(0, 1));

// Test 12: Practical example - template processing
console.log("\nTest 12: Template processing");
let template = "Hello, {{name}}! You have {{count}} messages.";
console.log("  Template:", template);
if (template.includes("{{") && template.includes("}}")) {
  console.log("  Template contains placeholders");
  let start = template.indexOf("{{");
  let end = template.indexOf("}}");
  if (start !== -1 && end !== -1) {
    let placeholder = template.substring(start + 2, end);
    console.log("  First placeholder:", placeholder);
  }
}

console.log("\n=== All tests completed ===");
