const assert = require('node:assert');

const IteratorPrototype = Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]()));
const dispose = IteratorPrototype[Symbol.dispose];
const d = Object.getOwnPropertyDescriptor(IteratorPrototype, Symbol.dispose);
assert.deepStrictEqual([typeof dispose, dispose.name, dispose.length, d.writable, d.enumerable, d.configurable], ['function', '[Symbol.dispose]', 0, true, false, true]);

const log = [];
function* gen() { try { yield 1; yield 2; } finally { log.push('closed'); } }
{
  using it = gen();
  log.push(it.next().value);
}
assert.deepStrictEqual(log, [1, 'closed'], 'using closes a generator at block exit');

{ using values = [1, 2].values(); assert.strictEqual(values.next().value, 1); }

assert.deepStrictEqual([dispose.call({}), dispose.call({ return() { return 5; } })], [undefined, undefined], 'a missing return is fine and the result is dropped');
assert.throws(() => dispose.call({ return: 1 }), TypeError);
assert.throws(() => dispose.call({ return() { throw new RangeError('x'); } }), RangeError);

console.log('iterator dispose OK');
