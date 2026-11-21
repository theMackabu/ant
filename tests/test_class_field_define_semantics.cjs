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

class Static {
  static count = 1;
  static next = Static.count + 1;
  static self = this;
  static list = [this.count, this.next];
  static #hidden = 5;
  static reveal() { return Static.#hidden; }
  instance = Static.count;
}
assert.deepStrictEqual(Object.keys(Static), ['count', 'next', 'self', 'list']);
assert.strictEqual(Static.self, Static);
assert.deepStrictEqual(Static.list, [1, 2]);
assert.strictEqual(Static.reveal(), 5);
plain(Static, 'count', 1);
Static.count = 7;
assert.strictEqual(new Static().instance, 7);
class StaticChild extends Static { static own = super.reveal() + 1 }
assert.strictEqual(StaticChild.own, 6);
assert.strictEqual(StaticChild.count, 7);
assert.ok(!Object.prototype.hasOwnProperty.call(StaticChild, 'count'));

{
  const sym = Symbol('sym');
  let result;
  class Returns { constructor() { return result(); } }
  class SymbolField extends Returns { [sym] = 2 }
  function makeSymbolField() { return new SymbolField(); }

  result = () => ({});
  for (let i = 0; i < WARM; i++) plain(makeSymbolField(), sym, 2);

  result = () => Object.defineProperty({}, sym, { value: 1, writable: true, enumerable: true, configurable: false });
  assert.throws(makeSymbolField, TypeError);
  result = () => Object.defineProperty({}, sym, { get() { return 1; }, set(v) { throw new Error('setter ran'); }, configurable: true });
  plain(makeSymbolField(), sym, 2);
  result = () => Object.defineProperty({}, sym, { value: 1, writable: false, enumerable: false, configurable: true });
  plain(makeSymbolField(), sym, 2);
  result = () => Object.freeze({});
  assert.throws(makeSymbolField, TypeError);
  result = () => Object.preventExtensions({});
  assert.throws(makeSymbolField, TypeError);
  result = () => Object.seal({ [sym]: 1 });
  assert.throws(makeSymbolField, TypeError);

  const seen = [];
  const trap = { defineProperty(target, key, desc) { seen.push(key, Object.keys(desc).join()); return Reflect.defineProperty(target, key, desc); } };
  result = () => new Proxy({}, trap);
  plain(makeSymbolField(), sym, 2);
  assert.deepStrictEqual(seen, [sym, 'value,writable,enumerable,configurable']);
  result = () => new Proxy({}, { defineProperty() { return false; } });
  assert.throws(makeSymbolField, TypeError);
  result = () => ({});
  for (let i = 0; i < WARM; i++) plain(makeSymbolField(), sym, 2);
}

{
  class StaticRules { static name = 'renamed'; static length = 5 }
  plain(StaticRules, 'name', 'renamed');
  plain(StaticRules, 'length', 5);
}

console.log('ok');
