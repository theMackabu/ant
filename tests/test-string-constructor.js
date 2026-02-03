// ============================================
// TEST: String() uses inspect instead of toString
// ============================================
// Bug: String(obj) uses inspect-style output instead of calling toString()
// ============================================

console.log("=== String() CONSTRUCTOR TESTS ===\n");

// --- PRIMITIVES ---

console.log("1. Primitives:");
console.log("   String(42):        expected '42',        got '" + String(42) + "'");
console.log("   String('hi'):      expected 'hi',        got '" + String("hi") + "'");
console.log("   String(true):      expected 'true',      got '" + String(true) + "'");
console.log("   String(false):     expected 'false',     got '" + String(false) + "'");
console.log("   String(null):      expected 'null',      got '" + String(null) + "'");
console.log("   String(undefined): expected 'undefined', got '" + String(undefined) + "'");
console.log("   String(123n):      expected '123',       got '" + String(123n) + "'");

// --- ARRAYS ---

console.log("\n2. Array [1, 2, 3]:");
const arr1 = [1, 2, 3];
console.log("   expected:  '1,2,3'");
console.log("   String():  '" + String(arr1) + "'");
console.log("   toString:  '" + arr1.toString() + "'");

console.log("\n3. Nested array [[1,2], [3,4]]:");
const arr2 = [[1, 2], [3, 4]];
console.log("   expected:  '1,2,3,4'");
console.log("   String():  '" + String(arr2) + "'");
console.log("   toString:  '" + arr2.toString() + "'");

console.log("\n4. Empty array []:");
const arr3 = [];
console.log("   expected:  ''");
console.log("   String():  '" + String(arr3) + "'");

console.log("\n5. Array with objects:");
const arr4 = [{ a: 1 }, { b: 2 }];
console.log("   expected:  '[object Object],[object Object]'");
console.log("   String():  '" + String(arr4) + "'");

// --- FUNCTIONS ---

console.log("\n6. Arrow function:");
const fn1 = () => 1;
console.log("   expected:  '() => 1'");
console.log("   String():  '" + String(fn1) + "'");
console.log("   toString:  '" + fn1.toString() + "'");

console.log("\n7. Named function:");
function namedFn() { return 1; }
console.log("   expected:  'function namedFn() { return 1; }'");
console.log("   String():  '" + String(namedFn) + "'");
console.log("   toString:  '" + namedFn.toString() + "'");

console.log("\n8. Async function:");
async function asyncFn() { return 1; }
console.log("   expected:  'async function asyncFn() { return 1; }'");
console.log("   String():  '" + String(asyncFn) + "'");
console.log("   toString:  '" + asyncFn.toString() + "'");

// --- OBJECTS ---

console.log("\n9. Plain object {}:");
const obj1 = {};
console.log("   expected:  '[object Object]'");
console.log("   String():  '" + String(obj1) + "'");

console.log("\n10. Object with properties:");
const obj2 = { a: 1, b: 2 };
console.log("   expected:  '[object Object]'");
console.log("   String():  '" + String(obj2) + "'");

console.log("\n11. Object with custom toString:");
const obj3 = { toString: function() { return "custom-toString"; } };
console.log("   expected:  'custom-toString'");
console.log("   toString:  '" + obj3.toString() + "'");
console.log("   String():  '" + String(obj3) + "'");

console.log("\n12. Object with valueOf only:");
const obj4 = { valueOf: function() { return "custom-valueOf"; } };
console.log("   expected:  'custom-valueOf' or '[object Object]'");
console.log("   String():  '" + String(obj4) + "'");

console.log("\n13. Object with both toString and valueOf:");
const obj5 = {
  toString: function() { return "from-toString"; },
  valueOf: function() { return "from-valueOf"; }
};
console.log("   expected:  'from-toString' (toString has priority)");
console.log("   String():  '" + String(obj5) + "'");

// --- BUILT-IN OBJECTS ---

console.log("\n14. Date:");
const date = new Date(0);
console.log("   expected:  date string like 'Thu Jan 01 1970...'");
console.log("   String():  '" + String(date) + "'");
console.log("   toString:  '" + date.toString() + "'");

console.log("\n15. RegExp:");
const re = /test/gi;
console.log("   expected:  '/test/gi'");
console.log("   String():  '" + String(re) + "'");
console.log("   toString:  '" + re.toString() + "'");

console.log("\n16. Error:");
const err = new Error("oops");
console.log("   expected:  'Error: oops'");
console.log("   String():  '" + String(err) + "'");
console.log("   toString:  '" + err.toString() + "'");

console.log("\n17. Map:");
const map = new Map([["a", 1]]);
console.log("   expected:  '[object Map]'");
console.log("   String():  '" + String(map) + "'");

console.log("\n18. Set:");
const set = new Set([1, 2, 3]);
console.log("   expected:  '[object Set]'");
console.log("   String():  '" + String(set) + "'");

// --- SYMBOL ---

console.log("\n19. Symbol:");
const sym = Symbol("test");
console.log("   expected:  'Symbol(test)'");
console.log("   String():  '" + String(sym) + "'");
console.log("   toString:  '" + sym.toString() + "'");

// --- Symbol.toPrimitive ---

console.log("\n20. Object with Symbol.toPrimitive:");
const obj6 = {};
obj6[Symbol.toPrimitive] = function(hint) { return "toPrimitive-" + hint; };
console.log("   expected:  'toPrimitive-string'");
console.log("   String():  '" + String(obj6) + "'");

// --- CLASS INSTANCES ---

console.log("\n21. Class instance with toString:");
class MyClass {
  toString() { return "MyClass instance"; }
}
const inst = new MyClass();
console.log("   expected:  'MyClass instance'");
console.log("   toString:  '" + inst.toString() + "'");
console.log("   String():  '" + String(inst) + "'");

console.log("\n22. Class instance without toString:");
class PlainClass {}
const inst2 = new PlainClass();
console.log("   expected:  '[object Object]'");
console.log("   String():  '" + String(inst2) + "'");

console.log("\n=== DONE ===");
