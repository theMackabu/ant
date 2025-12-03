// Comprehensive import.meta test

console.log("=== Testing all import.meta properties ===\n");

console.log("1. import.meta.url:", import.meta.url);
console.log("2. import.meta.filename:", import.meta.filename);
console.log("3. import.meta.dirname:", import.meta.dirname);
console.log("4. import.meta.main:", import.meta.main);
console.log("5. typeof import.meta.resolve:", typeof import.meta.resolve);

console.log("\n=== Testing import.meta.resolve() ===");
const resolved = import.meta.resolve("./example.js");
console.log("Resolved './example.js':", resolved);

console.log("\n=== Testing that import() still works ===");
async function testImport() {
  try {
    const mod = await import('./export-test.js');
    console.log("Dynamic import successful!");
    mod.hello('from import.meta test');
  } catch (e) {
    console.log("Dynamic import failed:", e);
  }
}

void testImport();
