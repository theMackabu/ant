// Test: import.meta support
// Tests import.meta.url, import.meta.dirname, and import.meta.resolve()

console.log("=== Testing import.meta ===\n");

// Test 1: import.meta.url
console.log("Test 1: import.meta.url");
console.log("import.meta.url:", import.meta.url);
console.log("Type:", typeof import.meta.url);
console.log("Starts with 'file://':", import.meta.url.startsWith("file://"));
console.log("✓ import.meta.url is accessible\n");

// Test 2: import.meta.dirname
console.log("Test 2: import.meta.dirname");
console.log("import.meta.dirname:", import.meta.dirname);
console.log("Type:", typeof import.meta.dirname);
console.log("✓ import.meta.dirname is accessible\n");

// Test 3: import.meta.resolve()
console.log("Test 3: import.meta.resolve()");
try {
  const resolved = import.meta.resolve("./example.js");
  console.log("Resolved './example.js':", resolved);
  console.log("Type:", typeof resolved);
  console.log("Starts with 'file://':", resolved.startsWith("file://"));
  console.log("✓ import.meta.resolve() works\n");
} catch (e) {
  console.log("✗ import.meta.resolve() failed:", e, "\n");
}

console.log("=== All import.meta tests complete ===");
