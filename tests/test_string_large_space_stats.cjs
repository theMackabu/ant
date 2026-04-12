const assert = require("assert");

const before = Ant.stats().pools.string;

assert.equal(typeof before.used, "number");
assert.equal(typeof before.capacity, "number");
assert.equal(typeof before.blocks, "number");
assert.equal(typeof before.pooled.used, "number");
assert.equal(typeof before.largeLive.capacity, "number");
assert.equal(typeof before.largeReusable.capacity, "number");
assert.equal(typeof before.largeQuarantine.capacity, "number");

const large = "x".repeat(256 * 1024);
assert.equal(large.length, 256 * 1024);

const after = Ant.stats().pools.string;

assert.ok(after.capacity >= before.capacity);
assert.ok(after.largeLive.capacity >= before.largeLive.capacity);
assert.ok(after.largeLive.blocks >= before.largeLive.blocks);

const rope = large + large;
assert.equal(rope.length, large.length * 2);

const afterRope = Ant.stats().pools.string;
assert.equal(typeof afterRope.largeReusable.blocks, "number");
assert.equal(typeof afterRope.largeQuarantine.blocks, "number");
assert.ok(afterRope.capacity >= after.capacity);

console.log("ok");
