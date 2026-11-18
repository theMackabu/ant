const assert = require('node:assert');

function C() { return [1, 2, 3]; }
assert.deepStrictEqual(Array.of.call(C, 7, 8), [7, 8]);
assert.deepStrictEqual(Array.from.call(C, { length: 2, 0: 7, 1: 8 }), [7, 8]);
assert.deepStrictEqual(Array.from.call(C, [7, 8]), [7, 8]);
assert.deepStrictEqual(Array.from.call(C, new Set([7, 8])), [7, 8]);
assert.deepStrictEqual(Array.from.call(C, 'ab'), ['a', 'b']);

const frozen = Object.freeze([1, 2, 3]);
assert.throws(() => Array.of.call(function () { return frozen; }, 7), TypeError);

const a = [1, 2, 3, 4];
a.constructor = { [Symbol.species]: function () { return [9, 9, 9, 9, 9]; } };
assert.deepStrictEqual(a.splice(0, 2), [1, 2]);
assert.deepStrictEqual([...a], [3, 4]);

assert.deepStrictEqual(Array.of(1, 2), [1, 2]);
assert.deepStrictEqual(Array.from([1, 2], (x) => x * 2), [2, 4]);

console.log('ok');
