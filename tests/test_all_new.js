function A() { this.foo = 1; this.bar = 42; }
A.prototype.method = function() { return "method"; };

console.log("Test 1 - new A().foo:", new A().foo);
console.log("Test 2 - new A().bar:", new A().bar);
console.log("Test 3 - new A().method():", new A().method());

var x = new A().foo;
console.log("Test 4 - assignment:", x);

if (new A().foo) console.log("Test 5 - if condition: PASS");
if (new A().bar === 42) console.log("Test 6 - comparison: PASS");

function B() { this.nested = { deep: "value" }; }
console.log("Test 7 - chained:", new B().nested.deep);
console.log("Test 8 - bracket:", new A()["foo"]);

function C() { this.x = 99; }
console.log("Test 9 - no parens:", new C);
console.log("Test 10 - (new C):", (new C));
console.log("Test 11 - (new C).x:", (new C).x);

// Test new with member access on constructor
var ns = { Ctor: function() { this.v = "ns"; } };
console.log("Test 12 - new ns.Ctor():", new ns.Ctor());
console.log("Test 13 - new ns.Ctor().v:", new ns.Ctor().v);
