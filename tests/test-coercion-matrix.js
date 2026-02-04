// ============================================
// TEST: String coercion comparison matrix
// ============================================
// Compares: concat, template, String(), toString()
// ============================================

console.log("=== STRING COERCION MATRIX ===\n");

function safe(fn) {
  try {
    const result = fn();
    if (result === undefined) return "undefined";
    if (result === null) return "null";
    return result;
  } catch (e) {
    return "ERROR:" + e.name;
  }
}

function test(name, value) {
  console.log(name + ":");
  console.log("  concat:   " + safe(function() { return "" + value; }));
  console.log("  template: " + safe(function() { return `${value}`; }));
  console.log("  String(): " + safe(function() { return String(value); }));
  if (value !== null && value !== undefined) {
    console.log("  toString: " + safe(function() { return value.toString(); }));
  }
  console.log("");
}

// Primitives
console.log("--- PRIMITIVES ---\n");
test("number 42", 42);
test("number 0", 0);
test("number -1", -1);
test("number NaN", NaN);
test("number Infinity", Infinity);
test("string 'hello'", "hello");
test("string ''", "");
test("boolean true", true);
test("boolean false", false);
test("null", null);
test("undefined", undefined);
test("BigInt 123n", 123n);

// Symbol
console.log("--- SYMBOL ---\n");
test("Symbol('test')", Symbol("test"));

// Arrays
console.log("--- ARRAYS ---\n");
test("[]", []);
test("[1, 2, 3]", [1, 2, 3]);
test("[[1,2], [3,4]]", [[1, 2], [3, 4]]);
test("[null, undefined]", [null, undefined]);
test("['a', 'b']", ["a", "b"]);

// Functions
console.log("--- FUNCTIONS ---\n");
test("() => 1", () => 1);
test("function() {}", function() {});
function namedFn() { return 1; }
test("named function", namedFn);
test("async () => 1", async () => 1);

// Objects
console.log("--- OBJECTS ---\n");
test("{}", {});
test("{ a: 1 }", { a: 1 });
test("{ a: 1, b: 2 }", { a: 1, b: 2 });

// Built-in objects
console.log("--- BUILT-IN OBJECTS ---\n");
test("new Date(0)", new Date(0));
test("new Date()", new Date());
test("/test/gi", /test/gi);
test("new Error('oops')", new Error("oops"));
test("new TypeError('bad')", new TypeError("bad"));
test("new Map([['a',1]])", new Map([["a", 1]]));
test("new Set([1,2,3])", new Set([1, 2, 3]));
test("new WeakMap()", new WeakMap());
test("new WeakSet()", new WeakSet());
test("new ArrayBuffer(8)", new ArrayBuffer(8));
test("new Uint8Array([1,2])", new Uint8Array([1, 2]));
test("Promise.resolve(1)", Promise.resolve(1));

// Custom toString
console.log("--- CUSTOM toString (CRASH TESTS) ---\n");

console.log("native toString (should work):");
test("{ toString: Object.prototype.toString }", { toString: Object.prototype.toString });

console.log("user toString (may crash):");
const userToString = { toString: function() { return "custom"; } };
console.log("  direct call works: " + userToString.toString());
test("{ toString: fn => 'custom' }", userToString);

console.log("arrow toString (may crash):");
const arrowToString = { toString: () => "arrow" };
console.log("  direct call works: " + arrowToString.toString());
test("{ toString: () => 'arrow' }", arrowToString);

// Custom valueOf
console.log("--- CUSTOM valueOf ---\n");
test("{ valueOf: fn => 42 }", { valueOf: function() { return 42; } });
test("{ valueOf: fn => 'val' }", { valueOf: function() { return "val"; } });

// Symbol.toPrimitive
console.log("--- Symbol.toPrimitive ---\n");
const withPrimitive = {};
withPrimitive[Symbol.toPrimitive] = function(hint) { return "hint:" + hint; };
test("{ [Symbol.toPrimitive]: fn }", withPrimitive);

// Class instances
console.log("--- CLASS INSTANCES ---\n");

class WithToString {
  toString() { return "class-toString"; }
}
console.log("class with toString method:");
const inst1 = new WithToString();
console.log("  direct call works: " + inst1.toString());
test("new WithToString()", inst1);

class WithValueOf {
  valueOf() { return "class-valueOf"; }
}
test("new WithValueOf()", new WithValueOf());

class PlainClass {}
test("new PlainClass()", new PlainClass());

console.log("=== SUMMARY ===\n");
console.log("Check for:");
console.log("1. Missing output after test name = SILENT CRASH");
console.log("2. '[Function: ...]' instead of function source = INSPECT BUG");
console.log("3. '{ ... }' or '[ ... ]' with spaces = INSPECT BUG");
console.log("4. Different results between concat and template = BUG");
console.log("");