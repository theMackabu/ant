// Simple test matching MDN example
const object = {};

Object.defineProperty(object, "foo", {
  value: 42,
  writable: false,
});

console.log("object.foo:", object.foo);
// Expected output: 42

// Test modifying existing property
const obj2 = { bar: 10 };
console.log("Before defineProperty, obj2.bar:", obj2.bar);

Object.defineProperty(obj2, "bar", {
  value: 20
});

console.log("After defineProperty, obj2.bar:", obj2.bar);
// Expected output: 20

// Test return value
const obj3 = {};
const result = Object.defineProperty(obj3, "test", {
  value: 123
});

console.log("Returned object is same as input:", result === obj3);
// Expected output: true

console.log("obj3.test:", obj3.test);
// Expected output: 123
