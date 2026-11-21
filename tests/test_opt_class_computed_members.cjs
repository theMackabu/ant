const assert = require('node:assert');

const WARM = 3000;

function own(object, key) { return Object.getOwnPropertyDescriptor(object, key); }
function plain(object, key, value) {
  const found = own(object, key);
  assert.ok(found, String(key));
  assert.strictEqual(found.value, value);
  assert.strictEqual(found.writable, true);
  assert.strictEqual(found.enumerable, true);
  assert.strictEqual(found.configurable, true);
}

let counter = 0;
function key(name) { counter++; return name; }
const symbol = Symbol('field');
class Computed {
  [key('first')] = 1;
  [key('se' + 'cond')] = 2;
  [symbol] = 3;
  [4] = 'four';
  5 = 'five';
  0.5 = 'half';
  ['constructor'] = 'own-constructor';
  ['__proto__'] = 'own-proto';
  static [key('fixed')] = 'static';
}
assert.strictEqual(counter, 3);
for (let i = 0; i < WARM; i++) {
  const made = new Computed();
  assert.strictEqual(made.first + made.second + made[symbol], 6);
}
assert.strictEqual(counter, 3);
{
  const made = new Computed();
  assert.deepStrictEqual(Object.keys(made), ['4', '5', 'first', 'second', '0.5', 'constructor', '__proto__']);
  assert.deepStrictEqual(Object.getOwnPropertySymbols(made), [symbol]);
  plain(made, symbol, 3);
  plain(made, '4', 'four');
  plain(made, 'constructor', 'own-constructor');
  plain(made, '__proto__', 'own-proto');
  assert.strictEqual(Object.getPrototypeOf(made), Computed.prototype);
  assert.strictEqual(Computed.fixed, 'static');
}

console.log('ok');
