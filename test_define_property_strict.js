"use strict";

// Test Object.defineProperty() in strict mode

const object = {};

Object.defineProperty(object, "foo", {
  value: 42,
  writable: false,
});

console.log("object.foo =", object.foo);

// Try to modify in strict mode (should throw)
try {
  object.foo = 77;
  console.log("FAIL: Should have thrown an error");
} catch (e) {
  console.log("PASS: Correctly throws in strict mode");
  console.log("object.foo still =", object.foo);
}

// Test non-configurable property
const obj2 = {};
Object.defineProperty(obj2, "bar", {
  value: "hello",
  configurable: false
});

try {
  obj2.bar = "world";
  console.log("FAIL: Should have thrown an error for non-configurable");
} catch (e) {
  console.log("PASS: Correctly throws for non-configurable in strict mode");
  console.log("obj2.bar still =", obj2.bar);
}

// Test enumerable
const obj3 = {};
Object.defineProperty(obj3, "hidden", {
  value: "secret",
  enumerable: false
});

Object.defineProperty(obj3, "visible", {
  value: "public",
  enumerable: true
});

console.log("\nKeys of obj3:", Object.keys(obj3));
console.log("Expected: only 'visible' should be in keys");

console.log("\nAll strict mode tests passed!");
