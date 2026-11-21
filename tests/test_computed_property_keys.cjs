const assert = require('node:assert');

const symbol = Symbol('q');
const toSymbol = { [Symbol.toPrimitive]() { return symbol; } };
const toString = { [Symbol.toPrimitive](hint) { return 'str-' + hint; } };
const prefersToString = { valueOf() { return 1; }, toString() { return 'ts'; } };
const throwing = { toString() { throw new RangeError('bad key'); } };
const toObject = { [Symbol.toPrimitive]() { return {}; } };

for (let round = 0; round < 400; round++) {
  class Keys {
    static [toSymbol] = 2;
    [toSymbol]() { return 1; }
    get [toString]() { return 3; }
    [prefersToString] = 4;
    static [toString]() {}
    static [prefersToString] = 5;
  }
  assert.deepStrictEqual(Object.getOwnPropertySymbols(Keys), [symbol]);
  assert.strictEqual(Keys[symbol], 2);
  assert.strictEqual(Keys.ts, 5);
  assert.strictEqual(typeof Keys['str-string'], 'function');
  assert.deepStrictEqual(Object.getOwnPropertySymbols(Keys.prototype), [symbol]);
  assert.strictEqual(Keys.prototype[symbol].name, '[q]');
  assert.deepStrictEqual(Object.getOwnPropertyNames(Keys.prototype), ['constructor', 'str-string']);
  assert.deepStrictEqual(Object.keys(new Keys()), ['ts']);

  const literal = { [toSymbol]: 1, [toString]: 2, [prefersToString]: 3 };
  assert.deepStrictEqual(Reflect.ownKeys(literal), ['str-string', 'ts', symbol]);
  const accessors = { get [toSymbol]() { return 9; }, set [toString](v) {} };
  assert.strictEqual(accessors[symbol], 9);
  assert.strictEqual(typeof Object.getOwnPropertyDescriptor(accessors, 'str-string').set, 'function');
  const method = { [toSymbol]() {} };
  assert.strictEqual(typeof method[symbol], 'function');

  const { [toSymbol]: bySymbol, [toString]: byString, ...rest } = { [symbol]: 'S', 'str-string': 'Y', z: 1 };
  assert.deepStrictEqual([bySymbol, byString, Object.keys(rest), Object.getOwnPropertySymbols(rest)], ['S', 'Y', ['z'], []]);

  assert.throws(() => ({ [throwing]: 1 }), RangeError);
  assert.throws(() => ({ ...{}, [throwing]: 1 }), RangeError);
  assert.throws(() => ({ a: 1, [throwing]: 1, b: 2 }), RangeError);
  assert.throws(() => ({ [throwing]() {} }), RangeError);
  assert.throws(() => ({ get [throwing]() { return 1; } }), RangeError);
  assert.throws(() => { class C { [throwing]() {} } }, RangeError);
  assert.throws(() => { class C { static [throwing] = 1; } }, RangeError);
  assert.throws(() => { const { [throwing]: z } = {}; return z; }, RangeError);
  assert.throws(() => ({ [toObject]: 1 }), TypeError);
}

function show(v) {
  if (typeof v === 'function') return 'function ' + v.name;
  if (typeof v === 'symbol' || typeof v === 'bigint') return String(v);
  if (v === undefined) return 'undefined';
  try { return JSON.stringify(v) } catch (e) { return 'unserializable' }
}

function outcome(src) {
  let fn;
  try { fn = new Function(src) } catch (e) { return 'compile ' + e.constructor.name }
  try { return 'value ' + show(fn()) } catch (e) { return 'throw ' + (e && e.constructor ? e.constructor.name : String(e)) }
}

const cases = [
  ["var k = { toString() { throw new RangeError('k') } }; var log = []; try { class C { [k] = 1 } log.push('defined'); new C() } catch (e) { log.push(e.constructor.name) } return log", "value [\"RangeError\"]"],
  ["var k = { toString() { throw new RangeError('k') } }; var log = []; try { var C = class { [k] = 1 }; log.push('defined'); new C() } catch (e) { log.push(e.constructor.name) } return log", "value [\"RangeError\"]"],
];

const failed = [];
for (const [src, want] of cases) {
  const got = outcome(src);
  if (got !== want) failed.push(`${src} => ${got}, want ${want}`);
}
assert.deepStrictEqual(failed, []);

console.log('ok');
