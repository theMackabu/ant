const { isDeepStrictEqual } = require('node:util');

function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

assert(isDeepStrictEqual({ a: 1, b: [2, 3] }, { a: 1, b: [2, 3] }) === true, 'expected matching objects to be deeply strict equal');
assert(isDeepStrictEqual({ a: 1 }, { a: '1' }) === false, 'expected strict comparison for nested values');
assert(isDeepStrictEqual([1, { x: 2 }], [1, { x: 3 }]) === false, 'expected differing nested arrays to be unequal');

console.log('PASS');
