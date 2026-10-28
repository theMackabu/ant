const assert = require('node:assert');

const fns = Object.getOwnPropertyNames(Math).filter(name => typeof Math[name] === 'function' && name !== 'random');
const inputs = [
  ['bigint', () => 2n],
  ['symbol', () => Symbol('s')],
  ['valueOf bigint', () => ({ valueOf() { return 3n; } })],
  ['valueOf throws', () => ({ valueOf() { throw new RangeError('vo'); } })],
];

function outcome(fn, args) {
  try {
    const r = Math[fn](...args);
    return Object.is(r, -0) ? '-0' : String(r);
  } catch (e) {
    return e.constructor.name;
  }
}

const twoArg = new Set(['atan2', 'hypot', 'imul', 'max', 'min', 'pow']);
for (const fn of fns) {
  for (const [label, make] of inputs) {
    const first = outcome(fn, [make()]);
    assert.ok(first === 'TypeError' || first === 'RangeError', `${fn}(${label}) gave ${first}`);
    if (twoArg.has(fn)) {
      const second = outcome(fn, [1, make()]);
      assert.ok(second === 'TypeError' || second === 'RangeError', `${fn}(1, ${label}) gave ${second}`);
    }
  }
}

const order = [];
const tracked = tag => ({ valueOf() { order.push(tag); return tag === 'b' ? Infinity : 1; } });
assert.strictEqual(Math.hypot(tracked('a'), tracked('b'), tracked('c')), Infinity);
assert.deepStrictEqual(order, ['a', 'b', 'c']);
assert.throws(() => Math.hypot(Infinity, { valueOf() { throw new RangeError('late'); } }), RangeError);
assert.strictEqual(Math.hypot(NaN, Infinity), Infinity);
assert.ok(Number.isNaN(Math.hypot(NaN, 1)));

function hot(v) { return Math.sin(v) + Math.abs(v) + Math.max(v, 0) + Math.pow(v, 2); }
let acc = 0;
for (let i = 0; i < 50000; i++) acc += hot(i % 7);
assert.ok(Number.isFinite(acc));
for (const bad of [2n, { valueOf() { return 2n; } }]) assert.throws(() => hot(bad), TypeError);

console.log('PASS math bigint args');
