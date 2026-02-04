// ============================================
// TEST: Concatenation crashes with user-defined toString
// ============================================
// Bug: "" + obj crashes silently when obj has user-defined toString
// ============================================

console.log("=== CONCAT CRASH TESTS ===\n");

// --- BASELINE (should work) ---

console.log("1. No custom toString:");
const obj1 = {};
console.log("   result: " + ("" + obj1));

console.log("\n2. toString = native function:");
const obj2 = { toString: Object.prototype.toString };
console.log("   result: " + ("" + obj2));

console.log("\n3. toString = Array.prototype.toString:");
const obj3 = { toString: Array.prototype.toString };
console.log("   result: " + ("" + obj3));

console.log("\n4. toString = console.log (native):");
const obj4 = { toString: console.log };
console.log("   result: " + ("" + obj4));

// --- CRASH TESTS (these crash silently) ---

console.log("\n5. toString = user function:");
const obj5 = { toString: function() { return "custom"; } };
console.log("   expected: custom");
console.log("   direct call: " + obj5.toString());
const result5 = "" + obj5;
console.log("   concat: " + result5);
console.log("   (if missing, CRASH)");

console.log("\n6. toString = arrow function:");
const obj6 = { toString: () => "arrow" };
console.log("   expected: arrow");
console.log("   direct call: " + obj6.toString());
const result6 = "" + obj6;
console.log("   concat: " + result6);
console.log("   (if missing, CRASH)");

console.log("\n7. toString = user function returning number:");
const obj7 = { toString: function() { return 42; } };
console.log("   expected: 42");
console.log("   direct call: " + obj7.toString());
const result7 = "" + obj7;
console.log("   concat: " + result7);
console.log("   (if missing, CRASH)");

console.log("\n8. Inherited toString from prototype:");
function MyClass() {}
MyClass.prototype.toString = function() { return "inherited"; };
const obj8 = new MyClass();
console.log("   expected: inherited");
console.log("   direct call: " + obj8.toString());
const result8 = "" + obj8;
console.log("   concat: " + result8);
console.log("   (if missing, CRASH)");

console.log("\n9. toString as getter returning function:");
const obj9 = { get toString() { return function() { return "getter"; }; } };
console.log("   expected: getter");
const result9 = "" + obj9;
console.log("   concat: " + result9);

console.log("\n10. toString = non-function value:");
const obj10 = { toString: "not a function" };
console.log("   expected: TypeError");
try {
  const result10 = "" + obj10;
  console.log("   concat: " + result10);
  console.log("   ERROR: should have thrown TypeError!");
} catch (e) {
  console.log("   threw: " + e.name + " (correct)");
}

console.log("\n=== DONE ===");
