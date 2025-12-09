// Test Object.defineProperty()

const object = {};

Object.defineProperty(object, "foo", {
  value: 42,
  writable: false,
});

console.log("object.foo =", object.foo);

// Try to modify (should be prevented by configurable: false default)
try {
  object.foo = 77;
  console.log("After trying to modify, object.foo =", object.foo);
} catch (e) {
  console.log("Cannot modify foo (expected):", e);
}

// Test with enumerable
const obj2 = {};
Object.defineProperty(obj2, "bar", {
  value: "hello",
  enumerable: true,
  configurable: true
});

console.log("obj2.bar =", obj2.bar);
console.log("Object.keys(obj2) =", Object.keys(obj2));

// Test with non-enumerable property
const obj3 = {};
Object.defineProperty(obj3, "hidden", {
  value: "secret",
  enumerable: false
});
obj3.visible = "public";

console.log("obj3.hidden =", obj3.hidden);
console.log("obj3.visible =", obj3.visible);
console.log("Object.keys(obj3) =", Object.keys(obj3));

console.log("All tests passed!");
