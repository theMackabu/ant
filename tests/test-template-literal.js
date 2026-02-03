// ============================================
// TEST: Template literals use inspect instead of toString
// ============================================
// Bug: `${obj}` uses inspect-style output instead of calling toString()
// ============================================

console.log("=== TEMPLATE LITERAL TESTS ===\n");

// --- PRIMITIVES ---

console.log("1. Primitives:");
console.log("   number:    expected '42',        got '" + `${42}` + "'");
console.log("   string:    expected 'hi',        got '" + `${"hi"}` + "'");
console.log("   boolean:   expected 'true',      got '" + `${true}` + "'");
console.log("   null:      expected 'null',      got '" + `${null}` + "'");
console.log("   undefined: expected 'undefined', got '" + `${undefined}` + "'");
console.log("   BigInt:    expected '123',       got '" + `${123n}` + "'");

// --- ARRAYS ---

console.log("\n2. Array [1, 2, 3]:");
const arr1 = [1, 2, 3];
console.log("   expected:  '1,2,3'");
console.log("   concat:    '" + ("" + arr1) + "'");
console.log("   template:  '" + `${arr1}` + "'");
console.log("   toString:  '" + arr1.toString() + "'");

console.log("\n3. Nested array [[1,2], [3,4]]:");
const arr2 = [[1, 2], [3, 4]];
console.log("   expected:  '1,2,3,4'");
console.log("   concat:    '" + ("" + arr2) + "'");
console.log("   template:  '" + `${arr2}` + "'");
console.log("   toString:  '" + arr2.toString() + "'");

console.log("\n4. Empty array []:");
const arr3 = [];
console.log("   expected:  ''");
console.log("   concat:    '" + ("" + arr3) + "'");
console.log("   template:  '" + `${arr3}` + "'");

// --- FUNCTIONS ---

console.log("\n5. Arrow function:");
const fn1 = () => 1;
console.log("   expected:  '() => 1'");
console.log("   concat:    '" + ("" + fn1) + "'");
console.log("   template:  '" + `${fn1}` + "'");
console.log("   toString:  '" + fn1.toString() + "'");

console.log("\n6. Named function:");
function namedFn() { return 1; }
console.log("   expected:  'function namedFn() { return 1; }'");
console.log("   concat:    '" + ("" + namedFn) + "'");
console.log("   template:  '" + `${namedFn}` + "'");

console.log("\n7. Function expression:");
const fn2 = function() { return 1; };
console.log("   expected:  'function() { return 1; }'");
console.log("   concat:    '" + ("" + fn2) + "'");
console.log("   template:  '" + `${fn2}` + "'");

// --- OBJECTS ---

console.log("\n8. Plain object {}:");
const obj1 = {};
console.log("   expected:  '[object Object]'");
console.log("   concat:    '" + ("" + obj1) + "'");
console.log("   template:  '" + `${obj1}` + "'");

console.log("\n9. Object with properties {a:1, b:2}:");
const obj2 = { a: 1, b: 2 };
console.log("   expected:  '[object Object]'");
console.log("   concat:    '" + ("" + obj2) + "'");
console.log("   template:  '" + `${obj2}` + "'");

console.log("\n10. Object with custom toString:");
const obj3 = { toString: function() { return "custom"; } };
console.log("   expected:  'custom'");
console.log("   toString:  '" + obj3.toString() + "'");
console.log("   template:  '" + `${obj3}` + "'");

// --- BUILT-IN OBJECTS ---

console.log("\n11. Date:");
const date = new Date(0);
console.log("   expected:  'Thu Jan 01 1970...' (locale date string)");
console.log("   concat:    '" + ("" + date) + "'");
console.log("   template:  '" + `${date}` + "'");

console.log("\n12. RegExp /test/gi:");
const re = /test/gi;
console.log("   expected:  '/test/gi'");
console.log("   concat:    '" + ("" + re) + "'");
console.log("   template:  '" + `${re}` + "'");

console.log("\n13. Error:");
const err = new Error("oops");
console.log("   expected:  'Error: oops'");
console.log("   concat:    '" + ("" + err) + "'");
console.log("   template:  '" + `${err}` + "'");

console.log("\n14. Map:");
const map = new Map([["a", 1]]);
console.log("   expected:  '[object Map]'");
console.log("   concat:    '" + ("" + map) + "'");
console.log("   template:  '" + `${map}` + "'");

console.log("\n15. Set:");
const set = new Set([1, 2, 3]);
console.log("   expected:  '[object Set]'");
console.log("   concat:    '" + ("" + set) + "'");
console.log("   template:  '" + `${set}` + "'");

// --- SYMBOL (should throw) ---

console.log("\n16. Symbol (should throw TypeError):");
const sym = Symbol("test");
console.log("   toString:  '" + sym.toString() + "'");
try {
  console.log("   template:  '" + `${sym}` + "'");
  console.log("   ERROR: should have thrown!");
} catch (e) {
  console.log("   template:  threw " + e.name + " (correct)");
}

// --- QUINE TEST ---

console.log("\n17. Quine test (self-referencing function):");
const $ = function(_) { return "$=" + $ + ";$()"; };
console.log("   expected:  '$=function(_) { return \"$=\" + $ + \";$()\"; };$()'");
console.log("   concat:    '" + $() + "'");
const $2 = function(_) { return `$=${$2};$()`; };
console.log("   template:  '" + $2() + "'");

console.log("\n=== DONE ===");
