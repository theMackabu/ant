// Test String methods functionality

Ant.println("=== String Methods Tests ===\n");

// Test 1: indexOf
Ant.println("Test 1: indexOf()");
let str1 = "hello world";
Ant.println("  '" + str1 + "'.indexOf('world'):", str1.indexOf("world"));
Ant.println("  '" + str1 + "'.indexOf('o'):", str1.indexOf("o"));
Ant.println("  '" + str1 + "'.indexOf('xyz'):", str1.indexOf("xyz"));
Ant.println("  '" + str1 + "'.indexOf(''):", str1.indexOf(""));

// Test 2: substring
Ant.println("\nTest 2: substring()");
let str2 = "JavaScript";
Ant.println("  '" + str2 + "'.substring(0, 4):", str2.substring(0, 4));
Ant.println("  '" + str2 + "'.substring(4):", str2.substring(4));
Ant.println("  '" + str2 + "'.substring(4, 10):", str2.substring(4, 10));
Ant.println("  '" + str2 + "'.substring(10, 4):", str2.substring(10, 4)); // Should swap

// Test 3: slice
Ant.println("\nTest 3: slice()");
let str3 = "The quick brown fox";
Ant.println("  '" + str3 + "'.slice(0, 3):", str3.slice(0, 3));
Ant.println("  '" + str3 + "'.slice(4, 9):", str3.slice(4, 9));
Ant.println("  '" + str3 + "'.slice(10):", str3.slice(10));
Ant.println("  '" + str3 + "'.slice(-3):", str3.slice(-3));
Ant.println("  '" + str3 + "'.slice(0, -4):", str3.slice(0, -4));
Ant.println("  '" + str3 + "'.slice(-9, -4):", str3.slice(-9, -4));

// Test 4: split
Ant.println("\nTest 4: split()");
let str4 = "apple,banana,cherry";
let parts = str4.split(",");
Ant.println("  '" + str4 + "'.split(','):");
for (let i = 0; i < parts.length; i++) {
  Ant.println("    [" + i + "]: " + parts[i]);
}

let str5 = "hello";
let chars = str5.split("");
Ant.println("  '" + str5 + "'.split(''):");
for (let i = 0; i < chars.length; i++) {
  Ant.println("    [" + i + "]: " + chars[i]);
}

// Test 5: includes
Ant.println("\nTest 5: includes()");
let str6 = "The quick brown fox jumps over the lazy dog";
Ant.println("  '" + str6 + "'");
Ant.println("    includes('quick'):", str6.includes("quick"));
Ant.println("    includes('cat'):", str6.includes("cat"));
Ant.println("    includes('fox'):", str6.includes("fox"));
Ant.println("    includes(''):", str6.includes(""));
Ant.println("    includes('QUICK'):", str6.includes("QUICK"));

// Test 6: startsWith
Ant.println("\nTest 6: startsWith()");
let str7 = "Hello, World!";
Ant.println("  '" + str7 + "'");
Ant.println("    startsWith('Hello'):", str7.startsWith("Hello"));
Ant.println("    startsWith('World'):", str7.startsWith("World"));
Ant.println("    startsWith('H'):", str7.startsWith("H"));
Ant.println("    startsWith(''):", str7.startsWith(""));

// Test 7: endsWith
Ant.println("\nTest 7: endsWith()");
let str8 = "index.html";
Ant.println("  '" + str8 + "'");
Ant.println("    endsWith('.html'):", str8.endsWith(".html"));
Ant.println("    endsWith('.js'):", str8.endsWith(".js"));
Ant.println("    endsWith('html'):", str8.endsWith("html"));
Ant.println("    endsWith(''):", str8.endsWith(""));

// Test 8: Combining methods
Ant.println("\nTest 8: Combining methods");
let path = "/api/users/123/profile";
Ant.println("  Path:", path);
let segments = path.split("/");
Ant.println("  Segments:");
for (let i = 0; i < segments.length; i++) {
  if (segments[i] !== "") {
    Ant.println("    " + segments[i]);
  }
}

// Test 9: URL parsing example
Ant.println("\nTest 9: URL parsing");
let url = "https://example.com/path/to/resource";
if (url.startsWith("https://")) {
  Ant.println("  URL uses HTTPS");
  let domain = url.slice(8);
  let domainEnd = domain.indexOf("/");
  if (domainEnd !== -1) {
    let hostname = domain.substring(0, domainEnd);
    Ant.println("  Hostname:", hostname);
    let pathname = domain.slice(domainEnd);
    Ant.println("  Pathname:", pathname);
  }
}

// Test 10: String validation
Ant.println("\nTest 10: String validation");
let email = "user@example.com";
Ant.println("  Email:", email);
Ant.println("    Contains '@':", email.includes("@"));
let atIndex = email.indexOf("@");
if (atIndex !== -1) {
  let username = email.substring(0, atIndex);
  let domain = email.substring(atIndex + 1);
  Ant.println("    Username:", username);
  Ant.println("    Domain:", domain);
  Ant.println("    Domain ends with '.com':", domain.endsWith(".com"));
}

// Test 11: Edge cases
Ant.println("\nTest 11: Edge cases");
let empty = "";
Ant.println("  Empty string:");
Ant.println("    length:", empty.length);
Ant.println("    indexOf('x'):", empty.indexOf("x"));
Ant.println("    slice(0, 5):", "'" + empty.slice(0, 5) + "'");
Ant.println("    includes(''):", empty.includes(""));
Ant.println("    startsWith(''):", empty.startsWith(""));
Ant.println("    endsWith(''):", empty.endsWith(""));

let single = "a";
Ant.println("  Single char 'a':");
Ant.println("    slice(-1):", single.slice(-1));
Ant.println("    substring(0, 1):", single.substring(0, 1));

// Test 12: Practical example - template processing
Ant.println("\nTest 12: Template processing");
let template = "Hello, {{name}}! You have {{count}} messages.";
Ant.println("  Template:", template);
if (template.includes("{{") && template.includes("}}")) {
  Ant.println("  Template contains placeholders");
  let start = template.indexOf("{{");
  let end = template.indexOf("}}");
  if (start !== -1 && end !== -1) {
    let placeholder = template.substring(start + 2, end);
    Ant.println("  First placeholder:", placeholder);
  }
}

Ant.println("\n=== All tests completed ===");
