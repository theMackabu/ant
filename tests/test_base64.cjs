// Test btoa() and atob() Base64 encoding/decoding

console.log("=== Base64 Tests ===\n");

// Test 1: Basic btoa encoding
console.log("Test 1: Basic btoa encoding");
let encoded1 = btoa("Hello, World!");
console.log("  Input: Hello, World!");
console.log("  Encoded:", encoded1);
console.log("  Expected: SGVsbG8sIFdvcmxkIQ==");
console.log("  Pass:", encoded1 === "SGVsbG8sIFdvcmxkIQ==");

// Test 2: Basic atob decoding
console.log("\nTest 2: Basic atob decoding");
let decoded2 = atob("SGVsbG8sIFdvcmxkIQ==");
console.log("  Input: SGVsbG8sIFdvcmxkIQ==");
console.log("  Decoded:", decoded2);
console.log("  Expected: Hello, World!");
console.log("  Pass:", decoded2 === "Hello, World!");

// Test 3: Round-trip test
console.log("\nTest 3: Round-trip test");
let original3 = "The quick brown fox jumps over the lazy dog";
let encoded3 = btoa(original3);
let decoded3 = atob(encoded3);
console.log("  Original:", original3);
console.log("  Encoded:", encoded3);
console.log("  Decoded:", decoded3);
console.log("  Pass:", original3 === decoded3);

// Test 4: Empty string
console.log("\nTest 4: Empty string");
let encoded4 = btoa("");
let decoded4 = atob("");
console.log("  btoa(''):", encoded4);
console.log("  atob(''):", decoded4);
console.log("  Pass:", encoded4 === "" && decoded4 === "");

// Test 5: Single character
console.log("\nTest 5: Single character");
let encoded5 = btoa("A");
console.log("  btoa('A'):", encoded5);
console.log("  Expected: QQ==");
console.log("  Pass:", encoded5 === "QQ==");

// Test 6: Two characters
console.log("\nTest 6: Two characters");
let encoded6 = btoa("AB");
console.log("  btoa('AB'):", encoded6);
console.log("  Expected: QUI=");
console.log("  Pass:", encoded6 === "QUI=");

// Test 7: Three characters (no padding)
console.log("\nTest 7: Three characters");
let encoded7 = btoa("ABC");
console.log("  btoa('ABC'):", encoded7);
console.log("  Expected: QUJD");
console.log("  Pass:", encoded7 === "QUJD");

// Test 8: Numbers in string
console.log("\nTest 8: Numbers in string");
let encoded8 = btoa("12345");
console.log("  btoa('12345'):", encoded8);
let decoded8 = atob(encoded8);
console.log("  atob result:", decoded8);
console.log("  Pass:", decoded8 === "12345");

// Test 9: Special characters
console.log("\nTest 9: Special characters");
let special = "!@#$%^&*()";
let encoded9 = btoa(special);
let decoded9 = atob(encoded9);
console.log("  Original:", special);
console.log("  Encoded:", encoded9);
console.log("  Decoded:", decoded9);
console.log("  Pass:", special === decoded9);

// Test 10: JSON data
console.log("\nTest 10: JSON data");
let jsonData = '{"name":"test","value":123}';
let encoded10 = btoa(jsonData);
let decoded10 = atob(encoded10);
console.log("  JSON:", jsonData);
console.log("  Encoded:", encoded10);
console.log("  Decoded:", decoded10);
console.log("  Pass:", jsonData === decoded10);

console.log("\n=== All Base64 tests completed ===");
