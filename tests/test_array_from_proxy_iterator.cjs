'use strict';
const assert = require('node:assert');

for (const handler of [() => ({}), () => ({ get: Reflect.get })]) {
  const keyed = [7, 8, 9];
  keyed[Symbol.iterator] = Array.prototype.keys;
  assert.deepStrictEqual(Array.from(new Proxy(keyed, handler())), [0, 1, 2]);

  const paired = [7, 8];
  paired[Symbol.iterator] = Array.prototype.entries;
  assert.deepStrictEqual(Array.from(new Proxy(paired, handler())), [[0, 7], [1, 8]]);

  const custom = [7, 8, 9];
  custom[Symbol.iterator] = function* () { yield 'x'; };
  assert.deepStrictEqual(Array.from(new Proxy(custom, handler())), ['x']);

  const other = [7, 8, 9];
  other[Symbol.iterator] = String.prototype[Symbol.iterator].bind('ab');
  assert.deepStrictEqual(Array.from(new Proxy(other, handler())), ['a', 'b']);

  assert.deepStrictEqual(Array.from(new Proxy([7, 8, 9], handler())), [7, 8, 9]);
  assert.deepStrictEqual(Array.from(new Proxy([7, 8, 9], handler()), (v, i) => v + i), [7, 9, 11]);
}

{
  const supplied = (method) => new Proxy([7, 8], { get(t, k, r) { return k === Symbol.iterator ? method : Reflect.get(t, k, r); } });
  assert.deepStrictEqual(Array.from(supplied(Array.prototype.keys)), [0, 1]);
  assert.deepStrictEqual(Array.from(supplied(Array.prototype.entries)), [[0, 7], [1, 8]]);
  assert.deepStrictEqual(Array.from(supplied(Array.prototype.values)), [7, 8]);
  assert.deepStrictEqual(Array.from(supplied(undefined)), [7, 8]);
}

{
  const log = [];
  const proxy = new Proxy([1, 2], { get(t, k, r) { log.push(typeof k === 'symbol' ? 'sym' : k); return Reflect.get(t, k, r); } });
  assert.deepStrictEqual(Array.from(proxy), [1, 2]);
  assert.deepStrictEqual(log, ['sym', 'length', '0', 'length', '1', 'length']);
}

{
  const proto = Object.getPrototypeOf([][Symbol.iterator]());
  const next = proto.next;
  let calls = 0;
  proto.next = function () { calls++; return next.call(this); };
  try {
    assert.deepStrictEqual(Array.from(new Proxy([1, 2], {})), [1, 2]);
    assert.strictEqual(calls, 3);
  } finally {
    proto.next = next;
  }
}

console.log('ok');
