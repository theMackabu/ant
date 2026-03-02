// ============================================
// TEST: Shorthand method syntax
// ============================================
// Tests all variations of method shorthand in objects and classes
// ============================================

console.log("=== SHORTHAND METHOD TESTS ===\n");

// --- SECTION 1: Basic method shorthand ---

console.log("SECTION 1: Basic method shorthand\n");

console.log("1. Regular method shorthand:");
const obj1 = { foo() { return "foo"; } };
console.log("   result: " + obj1.foo());
console.log("   PASSED\n");

console.log("2. Method with parameters:");
const obj2 = { add(a, b) { return a + b; } };
console.log("   result: " + obj2.add(2, 3));
console.log("   PASSED\n");

console.log("3. Method with rest parameters:");
const obj3 = { sum(...nums) { return nums.reduce(function(a, b) { return a + b; }, 0); } };
console.log("   result: " + obj3.sum(1, 2, 3, 4));
console.log("   PASSED\n");

console.log("4. Method with default parameters:");
const obj4 = { greet(name = "world") { return "hello " + name; } };
console.log("   result: " + obj4.greet());
console.log("   result: " + obj4.greet("ant"));
console.log("   PASSED\n");

console.log("5. Method with destructuring parameters:");
const obj5 = { getX({ x }) { return x; } };
console.log("   result: " + obj5.getX({ x: 42, y: 99 }));
console.log("   PASSED\n");

console.log("6. Multiple methods:");
const obj6 = {
  foo() { return "foo"; },
  bar() { return "bar"; },
  baz() { return "baz"; }
};
console.log("   result: " + obj6.foo() + ", " + obj6.bar() + ", " + obj6.baz());
console.log("   PASSED\n");

// --- SECTION 2: Getters and setters ---

console.log("SECTION 2: Getters and setters\n");

console.log("7. Getter:");
const obj7 = { get value() { return 42; } };
console.log("   result: " + obj7.value);
console.log("   PASSED\n");

console.log("8. Setter:");
const obj8 = { _v: 0, set value(x) { this._v = x; } };
obj8.value = 99;
console.log("   result: " + obj8._v);
console.log("   PASSED\n");

console.log("9. Getter and setter pair:");
const obj9 = {
  _v: 0,
  get value() { return this._v; },
  set value(x) { this._v = x * 2; }
};
obj9.value = 10;
console.log("   result: " + obj9.value);
console.log("   PASSED\n");

console.log("10. Computed getter:");
const prop = "dynamic";
const obj10 = { get [prop]() { return "computed-getter"; } };
console.log("   result: " + obj10.dynamic);
console.log("   PASSED\n");

// --- SECTION 3: Computed property names ---

console.log("SECTION 3: Computed property names\n");

console.log("11. Computed method name (string):");
const name11 = "myMethod";
const obj11 = { [name11]() { return "computed"; } };
console.log("   result: " + obj11.myMethod());
console.log("   PASSED\n");

console.log("12. Computed method name (expression):");
const obj12 = { ["foo" + "Bar"]() { return "fooBar"; } };
console.log("   result: " + obj12.fooBar());
console.log("   PASSED\n");

console.log("13. Computed method name (symbol):");
const sym = Symbol("mySymbol");
const obj13 = { [sym]() { return "symbol-method"; } };
console.log("   result: " + obj13[sym]());
console.log("   PASSED\n");

console.log("14. Computed method name (number):");
const obj14 = { [42]() { return "number-method"; } };
console.log("   result: " + obj14[42]());
console.log("   PASSED\n");

// --- SECTION 4: Generator methods ---

console.log("SECTION 4: Generator methods\n");

console.log("15. Generator method shorthand:");
console.log("   expected: works per ES6 spec");
const obj15 = { *gen() { yield 1; yield 2; yield 3; } };
let result15 = [];
for (const v of obj15.gen()) result15.push(v);
console.log("   result: " + result15.join(", "));
console.log("   PASSED\n");

console.log("16. Computed generator method:");
const obj16 = { *["myGen"]() { yield "a"; yield "b"; } };
let result16 = [];
for (const v of obj16.myGen()) result16.push(v);
console.log("   result: " + result16.join(", "));
console.log("   PASSED\n");

// --- SECTION 5: Async methods ---

console.log("SECTION 5: Async methods\n");

console.log("17. Async method shorthand:");
console.log("   expected: works per ES2017 spec");
const obj17 = { async fetch() { return "async-result"; } };
obj17.fetch().then(function(v) { console.log("   result: " + v); });
console.log("   PASSED\n");

console.log("18. Async method with await:");
const obj18 = {
  async getData() {
    const a = await Promise.resolve(1);
    const b = await Promise.resolve(2);
    return a + b;
  }
};
obj18.getData().then(function(v) { console.log("   result: " + v); });
console.log("   PASSED\n");

console.log("19. Computed async method:");
const obj19 = { async ["asyncMethod"]() { return "computed-async"; } };
obj19.asyncMethod().then(function(v) { console.log("   result: " + v); });
console.log("   PASSED\n");

console.log("20. Async generator method:");
console.log("   expected: works per ES2018 spec");
const obj20 = {
  async *asyncGen() {
    yield await Promise.resolve(1);
    yield await Promise.resolve(2);
  }
};
(async function() {
  let result = [];
  for await (const v of obj20.asyncGen()) result.push(v);
  console.log("   result: " + result.join(", "));
})();
console.log("   PASSED\n");

// --- SECTION 6: Mixed with properties ---

console.log("SECTION 6: Mixed methods and properties\n");

console.log("21. Methods with regular properties:");
const obj21 = {
  x: 1,
  y: 2,
  getSum() { return this.x + this.y; },
  z: 3
};
console.log("   result: " + obj21.getSum() + ", z=" + obj21.z);
console.log("   PASSED\n");

console.log("22. Methods with shorthand properties:");
const a = 1, b = 2;
const obj22 = {
  a,
  b,
  sum() { return this.a + this.b; }
};
console.log("   result: " + obj22.sum());
console.log("   PASSED\n");

console.log("23. Methods with spread:");
const base = { foo() { return "base"; } };
const obj23 = { ...base, bar() { return "extended"; } };
console.log("   result: " + obj23.foo() + ", " + obj23.bar());
console.log("   PASSED\n");

// --- SECTION 7: Class methods ---

console.log("SECTION 7: Class methods\n");

console.log("24. Class method:");
class Class24 {
  foo() { return "class-method"; }
}
console.log("   result: " + new Class24().foo());
console.log("   PASSED\n");

console.log("25. Class static method:");
class Class25 {
  static foo() { return "static-method"; }
}
console.log("   result: " + Class25.foo());
console.log("   PASSED\n");

console.log("26. Class getter/setter:");
class Class26 {
  constructor() { this._v = 0; }
  get value() { return this._v; }
  set value(x) { this._v = x; }
}
const inst26 = new Class26();
inst26.value = 42;
console.log("   result: " + inst26.value);
console.log("   PASSED\n");

console.log("27. Class async method:");
class Class27 {
  async fetch() { return "class-async"; }
}
new Class27().fetch().then(function(v) { console.log("   result: " + v); });
console.log("   PASSED\n");

console.log("28. Class static async method:");
class Class28 {
  static async fetch() { return "static-async"; }
}
Class28.fetch().then(function(v) { console.log("   result: " + v); });
console.log("   PASSED\n");

console.log("29. Class generator method:");
class Class29 {
  *gen() { yield 1; yield 2; }
}
let result29 = [];
for (const v of new Class29().gen()) result29.push(v);
console.log("   result: " + result29.join(", "));
console.log("   PASSED\n");

console.log("30. Class computed method:");
const methodName30 = "dynamic";
class Class30 {
  [methodName30]() { return "computed-class-method"; }
}
console.log("   result: " + new Class30().dynamic());
console.log("   PASSED\n");

// --- SECTION 8: this binding ---

console.log("SECTION 8: this binding\n");

console.log("31. Method this binding:");
const obj31 = {
  name: "obj31",
  getName() { return this.name; }
};
console.log("   result: " + obj31.getName());
console.log("   PASSED\n");

console.log("32. Method this with call:");
const obj32 = {
  getName() { return this.name; }
};
console.log("   result: " + obj32.getName.call({ name: "called" }));
console.log("   PASSED\n");

console.log("33. Nested method this:");
const obj33 = {
  name: "outer",
  inner: {
    name: "inner",
    getName() { return this.name; }
  }
};
console.log("   result: " + obj33.inner.getName());
console.log("   PASSED\n");

console.log("\n=== ALL TESTS COMPLETE ===");
console.log("\nIf any test shows 'SyntaxError' before 'PASSED', that feature is broken.");
