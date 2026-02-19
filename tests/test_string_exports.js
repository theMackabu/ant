// Test: String literal export/import names (ES2022)
// Tests export { x as "string" }, import { "string" as x }, export * as "string"

console.log("=== Testing string literal export/import names ===\n");

let passed = 0;
let failed = 0;

function assert(condition, msg) {
  if (condition) {
    console.log("✓", msg);
    passed++;
  } else {
    console.log("✗", msg);
    failed++;
  }
}

// Test 1: import { "string" as local } from module
async function main() {
  // Basic string-named exports
  const mod = await import('./string-export-module.js');

  assert(mod["matrix"] === "jquery", 'export { jq as "matrix" }');
  assert(mod["foo-bar"] === 42, 'export { foo as "foo-bar" }');
  assert(mod["unicode A"] === "hello", 'export { bar as "unicode \\u0041" }');
  assert(mod["default"] === true, 'export { baz as "default" }');

  // Test 2: export * as "string" from re-export
  const remod = await import('./string-reexport-module.js');

  assert(typeof remod["nested"] === "object", 'export * as "nested" produces object');
  assert(remod["nested"]["matrix"] === "jquery", 're-exported "matrix" accessible via "nested"');
  assert(remod["nested"]["foo-bar"] === 42, 're-exported "foo-bar" accessible via "nested"');

  // Test 3: string as local name in export (valid only in re-exports)
  const remod2 = await import('./string-local-reexport-module.js');

  assert(remod2["renamed"] === "jquery", 'export { "matrix" as "renamed" } from re-export');

  console.log(`\n=== Results: ${passed} passed, ${failed} failed ===`);
  if (failed > 0) process.exit(1);
}

void main();
