// Test primitive wrapper objects with SLOT_PRIMITIVE

let passed = 0;
let failed = 0;

function test(name, condition) {
  if (condition) {
    console.log("✓", name);
    passed++;
  } else {
    console.log("✗", name);
    failed++;
  }
}

// String wrapper
let s = new String("hello");
test("String wrapper typeof", typeof s === "object");
test("String wrapper + concat", s + " world" === "hello world");
test("String wrapper length", s.length === 5);
test("String wrapper valueOf", s.valueOf() === "hello");

// Number wrapper
let n = new Number(42);
test("Number wrapper typeof", typeof n === "object");
test("Number wrapper + arithmetic", n + 8 === 50);
test("Number wrapper valueOf", n.valueOf() === 42);

// Boolean wrapper
let b = new Boolean(true);
test("Boolean wrapper typeof", typeof b === "object");
test("Boolean wrapper is truthy", !!b === true);
test("Boolean wrapper valueOf", b.valueOf() === true);

// Slot should be hidden from enumeration
let sKeys = Object.keys(s);
test("String keys excludes internal slot", !sKeys.includes("__primitive_value__"));

let nKeys = Object.keys(n);
test("Number keys excludes internal slot", !nKeys.includes("__primitive_value__"));

// Object() wrapping primitives
let wrapped = Object("test");
test("Object(string) is object", typeof wrapped === "object");
test("Object(string) valueOf", wrapped.valueOf() === "test");

console.log("\nResults:", passed, "passed,", failed, "failed");
