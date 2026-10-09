const assert = require('node:assert');

// Array.prototype.fill does Set(O, k, value, true) for each index in order.
// An index the object cannot hold itself may still be accepted by an
// inherited setter or proxy, so nothing is decided before the writes.
const log = [];
const proxyProto = new Proxy({}, {
  has(target, key) { log.push(`has ${String(key)}`); return false; },
  set(target, key, value) { log.push(`set ${String(key)}=${value}`); return true; },
  get() { return undefined; },
});
const viaProxy = Object.create(proxyProto);
Object.defineProperty(viaProxy, 'length', { value: 2, writable: true });
Object.preventExtensions(viaProxy);
Array.prototype.fill.call(viaProxy, 7);
assert.deepStrictEqual(log, ['set 0=7', 'set 1=7']);

const frozenViaProxy = Object.create(new Proxy({}, { set() { return true; } }));
Object.defineProperty(frozenViaProxy, 'length', { value: 2 });
Object.freeze(frozenViaProxy);
Array.prototype.fill.call(frozenViaProxy, 1);

const setterLog = [];
const setterProto = {};
Object.defineProperty(setterProto, '0', { set(v) { setterLog.push(v); } });
const viaSetter = Object.create(setterProto);
Object.defineProperty(viaSetter, 'length', { value: 1 });
Object.preventExtensions(viaSetter);
Array.prototype.fill.call(viaSetter, 5);
assert.deepStrictEqual(setterLog, [5]);

// the first failing write throws, after the earlier indexes were written
const partial = { 0: 'a', length: 3 };
Object.preventExtensions(partial);
assert.throws(() => Array.prototype.fill.call(partial, 'z'), TypeError);
assert.deepStrictEqual({ ...partial }, { 0: 'z', length: 3 });

const holey = [1, , 3];
Object.preventExtensions(holey);
assert.throws(() => holey.fill(0), TypeError);
assert.deepStrictEqual(Object.keys(holey), ['0', '2']);
assert.strictEqual(holey[0], 0);

assert.throws(() => Array.prototype.fill.call(new String('abc'), 'x', 1),
  { name: 'TypeError', message: "Cannot assign to read only property '1' of object '[object String]'" });
assert.throws(() => Object.freeze([1, 2]).fill(0), TypeError);
assert.deepStrictEqual(Object.freeze([1, 2]).fill(0, 2), [1, 2]);
assert.deepStrictEqual(Object.seal([1, 2, 3]).fill(9), [9, 9, 9]);
assert.deepStrictEqual([1, , 3, 4].fill(8, -3, -1), [1, 8, 8, 4]);
assert.deepStrictEqual(Array.prototype.fill.call({ length: 2 }, 1), { 0: 1, 1: 1, length: 2 });

console.log('array fill set order: ok');
