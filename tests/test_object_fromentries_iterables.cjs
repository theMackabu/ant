const assert = require("node:assert");

assert.deepStrictEqual(
  Object.fromEntries(new Map([
    ["a", 1],
    ["b", 2],
  ])),
  { a: 1, b: 2 },
);

function* pairGenerator() {
  yield ["x", 10];
  yield ["y", 20];
}

assert.deepStrictEqual(
  Object.fromEntries(pairGenerator()),
  { x: 10, y: 20 },
);

const iterable = {
  [Symbol.iterator]: function* () {
    yield ["left", "right"];
    yield ["up", "down"];
  },
};

assert.deepStrictEqual(
  Object.fromEntries(iterable),
  { left: "right", up: "down" },
);

class RequestContextLike {
  constructor() {
    this.registry = new Map();
  }

  set(key, value) {
    this.registry.set(key, value);
  }

  entries() {
    return this.registry.entries();
  }
}

const ctx = new RequestContextLike();
ctx.set("harness", {
  state: { projectPath: "/tmp/project" },
  getState() {
    return this.state;
  },
});

const snapshot = Object.fromEntries(ctx.entries());
assert.strictEqual(snapshot.harness.getState().projectPath, "/tmp/project");

assert.throws(
  () => Object.fromEntries((function* () { yield 1; })()),
  /entry objects/,
);

console.log("Object.fromEntries consumes Map iterators, generators, and custom iterables");
