// ============================================
// TEST: catch with destructuring not supported
// ============================================
// Bug: catch ({ prop }) and catch ([a, b]) fail with SyntaxError
// ============================================

console.log("=== CATCH DESTRUCTURING TESTS ===\n");

// --- BASELINE (should work) ---

console.log("1. catch with simple identifier:");
try {
  throw new Error("test error");
} catch (e) {
  console.log("   caught: " + e.message);
}
console.log("   PASSED\n");

console.log("2. catch without parameter (ES2019):");
try {
  throw new Error("ignored");
} catch {
  console.log("   caught without param");
}
console.log("   PASSED\n");

console.log("3. catch with manual destructuring (workaround):");
try {
  throw { message: "manual", code: 42 };
} catch (e) {
  const { message, code } = e;
  console.log("   message: " + message + ", code: " + code);
}
console.log("   PASSED\n");

console.log("4. catch with manual array destructuring (workaround):");
try {
  throw [1, 2, 3];
} catch (e) {
  const [a, b, c] = e;
  console.log("   a: " + a + ", b: " + b + ", c: " + c);
}
console.log("   PASSED\n");

// --- BUG TESTS (these should work per ES6 but fail in Ant) ---

console.log("5. catch with object destructuring:");
console.log("   expected: works per ES6 spec");
try {
  throw { message: "destructured", code: 99 };
} catch ({ message, code }) {
  console.log("   message: " + message + ", code: " + code);
}
console.log("   PASSED\n");

console.log("6. catch with array destructuring:");
console.log("   expected: works per ES6 spec");
try {
  throw [10, 20];
} catch ([a, b]) {
  console.log("   a: " + a + ", b: " + b);
}
console.log("   PASSED\n");

console.log("7. catch with nested object destructuring:");
try {
  throw { outer: { inner: "value" } };
} catch ({ outer: { inner } }) {
  console.log("   inner: " + inner);
}
console.log("   PASSED\n");

console.log("8. catch with default values:");
try {
  throw { a: 1 };
} catch ({ a, b = 99 }) {
  console.log("   a: " + a + ", b: " + b);
}
console.log("   PASSED\n");

console.log("9. catch with renaming:");
try {
  throw { longPropertyName: "short" };
} catch ({ longPropertyName: short }) {
  console.log("   short: " + short);
}
console.log("   PASSED\n");

console.log("10. catch with rest pattern:");
try {
  throw [1, 2, 3, 4, 5];
} catch ([first, ...rest]) {
  console.log("   first: " + first + ", rest: " + rest);
}
console.log("   PASSED\n");

console.log("=== ALL TESTS PASSED ===");
