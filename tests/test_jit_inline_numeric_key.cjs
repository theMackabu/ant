const assert = require('node:assert');

function readKey(array, index) { return array[index]; }
function callReadKey(array, index) { return readKey(array, index * 1).value; }

const array = [{ value: 'zero' }, { value: 'one' }];
for (let i = 0; i < 2000; i++)
  assert.strictEqual(callReadKey(array, 1), 'one');

assert.strictEqual(callReadKey(array, -0), 'zero');
for (const key of [1.5, -1, NaN, Infinity, -Infinity, 4294967295]) {
  array[key] = { value: String(key) };
  assert.strictEqual(callReadKey(array, key), String(key));
}
assert.strictEqual(array.length, 2);
assert.strictEqual(callReadKey(array, '1'), 'one');

let calls = 0;
const proto = Object.create(Array.prototype);
Object.defineProperty(proto, '0', { get() { calls++; return { value: 'inherited' }; } });
const holey = [, array[1]];
Object.setPrototypeOf(holey, proto);
assert.strictEqual(callReadKey(holey, 0), 'inherited');
assert.strictEqual(calls, 1);

const proxy = new Proxy(array, { get(target, key) {
  calls++;
  assert.strictEqual(key, '1');
  return target[key];
} });
assert.strictEqual(callReadKey(proxy, 1), 'one');
assert.strictEqual(calls, 2);
console.log('PASS inline numeric keys and fallback semantics');
