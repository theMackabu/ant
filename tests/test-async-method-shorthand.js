// ============================================
// TEST: async method shorthand not supported
// ============================================
// Bug: { async foo() {} } fails with SyntaxError
// ============================================

console.log("=== ASYNC METHOD SHORTHAND TESTS ===\n");

// --- BASELINE: Regular async (should work) ---

console.log("1. async function declaration:");
async function asyncFn1() { return "async-decl"; }
asyncFn1().then(function(v) { console.log("   result: " + v); });
console.log("   PASSED\n");

console.log("2. async function expression:");
const asyncFn2 = async function() { return "async-expr"; };
asyncFn2().then(function(v) { console.log("   result: " + v); });
console.log("   PASSED\n");

console.log("3. async arrow function:");
const asyncFn3 = async () => "async-arrow";
asyncFn3().then(function(v) { console.log("   result: " + v); });
console.log("   PASSED\n");

console.log("4. async arrow with body:");
const asyncFn4 = async () => { return "async-arrow-body"; };
asyncFn4().then(function(v) { console.log("   result: " + v); });
console.log("   PASSED\n");

// --- BASELINE: Method syntax (should work) ---

console.log("5. regular method shorthand:");
const obj5 = { foo() { return "method"; } };
console.log("   result: " + obj5.foo());
console.log("   PASSED\n");

console.log("6. method as property (async):");
const obj6 = { foo: async function() { return "async-prop"; } };
obj6.foo().then(function(v) { console.log("   result: " + v); });
console.log("   PASSED\n");

console.log("7. method as arrow property (async):");
const obj7 = { foo: async () => "async-arrow-prop" };
obj7.foo().then(function(v) { console.log("   result: " + v); });
console.log("   PASSED\n");

// --- BUG TESTS (these should work per ES2017 but fail in Ant) ---

console.log("8. async method shorthand:");
console.log("   expected: works per ES2017 spec");
const obj8 = { async foo() { return "async-shorthand"; } };
obj8.foo().then(function(v) { console.log("   result: " + v); });
console.log("   PASSED\n");

console.log("9. async method shorthand with params:");
const obj9 = { async add(a, b) { return a + b; } };
obj9.add(2, 3).then(function(v) { console.log("   result: " + v); });
console.log("   PASSED\n");

console.log("10. async method shorthand with await:");
const obj10 = {
  async fetch() {
    const result = await Promise.resolve("awaited");
    return result;
  }
};
obj10.fetch().then(function(v) { console.log("   result: " + v); });
console.log("   PASSED\n");

console.log("11. multiple async methods:");
const obj11 = {
  async foo() { return "foo"; },
  async bar() { return "bar"; },
  async baz() { return "baz"; }
};
Promise.all([obj11.foo(), obj11.bar(), obj11.baz()]).then(function(v) {
  console.log("   result: " + v.join(", "));
});
console.log("   PASSED\n");

console.log("12. mixed async and sync methods:");
const obj12 = {
  sync() { return "sync"; },
  async async1() { return "async"; },
  regular: function() { return "regular"; },
  async async2() { return "async2"; }
};
console.log("   sync: " + obj12.sync());
console.log("   regular: " + obj12.regular());
obj12.async1().then(function(v) { console.log("   async1: " + v); });
obj12.async2().then(function(v) { console.log("   async2: " + v); });
console.log("   PASSED\n");

console.log("13. async method in class:");
class MyClass {
  async fetch() { return "class-async"; }
}
const inst = new MyClass();
inst.fetch().then(function(v) { console.log("   result: " + v); });
console.log("   PASSED\n");

console.log("14. async static method in class:");
class MyClass2 {
  static async fetch() { return "static-async"; }
}
MyClass2.fetch().then(function(v) { console.log("   result: " + v); });
console.log("   PASSED\n");

console.log("15. computed async method name:");
const methodName = "dynamicAsync";
const obj15 = {
  async [methodName]() { return "computed-async"; }
};
obj15.dynamicAsync().then(function(v) { console.log("   result: " + v); });
console.log("   PASSED\n");

console.log("=== ALL TESTS PASSED ===");
