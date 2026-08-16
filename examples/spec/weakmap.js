import { test, summary } from './helpers.js';

console.log('WeakMap Tests\n');

const wm = new WeakMap();
const key1 = {};
const key2 = {};

wm.set(key1, 'value1');
wm.set(key2, 42);

test('weakmap get', wm.get(key1), 'value1');
test('weakmap get number', wm.get(key2), 42);
test('weakmap has true', wm.has(key1), true);
test('weakmap has false', wm.has({}), false);

wm.delete(key1);
test('weakmap delete', wm.has(key1), false);

const key3 = { id: 1 };
wm.set(key3, 'test');
wm.set(key3, 'updated');
test('weakmap overwrite', wm.get(key3), 'updated');

const nativeFunction = Array.prototype.push;
wm.set(nativeFunction, 'native function');
test('weakmap native function key', wm.get(nativeFunction), 'native function');

const symbol = Symbol('weak key');
wm.set(symbol, 'symbol');
test('weakmap non-registered symbol key', wm.get(symbol), 'symbol');

const ws = new WeakSet([nativeFunction, symbol]);
test('weakset native function value', ws.has(nativeFunction), true);
test('weakset non-registered symbol value', ws.has(symbol), true);

for (const method of ['get', 'delete']) {
  let error;
  try {
    WeakMap.prototype[method].call({}, {});
  } catch (caught) {
    error = caught;
  }
  test(`weakmap ${method} invalid receiver throws TypeError`, error instanceof TypeError, true);
}

summary();
