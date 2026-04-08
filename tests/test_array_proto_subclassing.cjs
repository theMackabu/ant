function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

console.log("array prototype and subclassing regression");

assert(Array.isArray(Array.prototype), "Array.prototype should be an array exotic");
assert(Object.getPrototypeOf(Array.prototype) === Object.prototype, "Array.prototype should inherit from Object.prototype");

class C extends Array {}
const c = new C(1, 2, 3);

assert(c instanceof C, "Array subclass instances should preserve subclass identity");
assert(c instanceof Array, "Array subclass instances should still be arrays");
assert(c.concat(4) instanceof C, "concat should preserve Array subclass");
assert(c.map(x => x) instanceof C, "map should preserve Array subclass");
assert(c.filter(() => true) instanceof C, "filter should preserve Array subclass");
assert(c.slice(0) instanceof C, "slice should preserve Array subclass");
assert(C.from([1, 2]) instanceof C, "Array.from on subclass should preserve subclass");
assert(C.of(1, 2) instanceof C, "Array.of on subclass should preserve subclass");

console.log("PASS");
