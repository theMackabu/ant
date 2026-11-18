const assert = require('node:assert');

const key = 'ck';
const symbol = Symbol('sy');

for (let round = 0; round < 300; round++) {
  let object = { [key]: Math.min, plain: Math.max, [symbol]: Math.abs, [1]: Math.cos, ['a' + 'b']: Math.hypot };
  assert.strictEqual(object.ck, Math.min);
  assert.strictEqual(object.ck.name, 'min');
  assert.strictEqual(object.plain, Math.max);
  assert.strictEqual(object[symbol], Math.abs);
  assert.strictEqual(object[symbol].name, 'abs');
  assert.strictEqual(object[1].name, 'cos');
  assert.strictEqual(object.ab, Math.hypot);
  assert.strictEqual(Object.getOwnPropertyDescriptor(object, key).value, Math.min);

  object = {};
  object[key] = Math.sin;
  object.direct = Math.tan;
  object[symbol] = Math.exp;
  assert.deepStrictEqual([object.ck === Math.sin, object.ck.name, object.direct === Math.tan, object[symbol] === Math.exp], [true, 'sin', true, true]);
  assert.strictEqual(Reflect.set(object, 'reflected', Math.atan), true);
  assert.strictEqual(object.reflected, Math.atan);
  assert.strictEqual(object.reflected.name, 'atan');

  class Holder { static [key] = Math.ceil; [key] = Math.round; static direct = Math.floor; }
  assert.deepStrictEqual([Holder.ck === Math.ceil, Holder.ck.name, new Holder().ck === Math.round, new Holder().ck.name, Holder.direct === Math.floor], [true, 'ceil', true, 'round', true]);

  const defined = Object.defineProperty({}, key, { value: Math.sqrt });
  const assigned = Object.assign({}, { [key]: Math.log });
  const spread = { ...{ [key]: Math.pow } };
  const entries = Object.fromEntries([[key, Math.sign]]);
  for (const [holder, fn, name] of [[defined, Math.sqrt, 'sqrt'], [assigned, Math.log, 'log'], [spread, Math.pow, 'pow'], [entries, Math.sign, 'sign']]) {
    assert.strictEqual(holder.ck, fn);
    assert.strictEqual(holder.ck.name, name);
  }
  assert.strictEqual(new Map([[key, Math.trunc]]).get(key), Math.trunc);
  assert.strictEqual([Math.cbrt][0], Math.cbrt);
  assert.strictEqual(Math.min.name, 'min');
  assert.strictEqual(JSON.parse.name, 'parse');
}

console.log('ok');
