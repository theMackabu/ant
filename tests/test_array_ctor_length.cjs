const assert = require('node:assert');

function C() { return [1, 2, 3]; }
assert.deepStrictEqual(Array.of.call(C, 7, 8), [7, 8]);

const frozen = Object.freeze([1, 2, 3]);
assert.throws(() => Array.of.call(function () { return frozen; }, 7), TypeError);

const a = [1, 2, 3, 4];
a.constructor = { [Symbol.species]: function () { return [9, 9, 9, 9, 9]; } };
assert.deepStrictEqual(a.splice(0, 2), [1, 2]);
assert.deepStrictEqual([...a], [3, 4]);

assert.deepStrictEqual(Array.of(1, 2), [1, 2]);
assert.deepStrictEqual(Array.from([1, 2], (x) => x * 2), [2, 4]);

console.log('ok');
