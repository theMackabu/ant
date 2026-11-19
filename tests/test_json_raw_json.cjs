const assert = require('node:assert');

for (const text of ['1', '-0', '1e1000', '12345678901234567890', '0.1000', '"str"', '"\\u00e9\\n"', 'true', 'false', 'null']) {
  const raw = JSON.rawJSON(text);
  assert.strictEqual(raw.rawJSON, text);
  assert.strictEqual(JSON.stringify(raw), text);
  assert.strictEqual(JSON.stringify({ a: raw, b: [raw] }), `{"a":${text},"b":[${text}]}`);
}

for (const text of ['{}', '[]', '[1]', '{"a":1}', '', ' 1', '1 ', '\t1', '1\n', '1\r', 'NaN', 'Infinity', 'undefined', '01', '1.', '.5', '+1', "'a'", '"open', '1 2', '1,2', 'tru', '0x10', '1e', '-', '1/**/'])
  assert.throws(() => JSON.rawJSON(text), SyntaxError, JSON.stringify(text));

assert.throws(() => JSON.rawJSON(), SyntaxError);
assert.throws(() => JSON.rawJSON({}), SyntaxError);
assert.throws(() => JSON.rawJSON(Symbol('s')), TypeError);
assert.strictEqual(JSON.rawJSON(1.5).rawJSON, '1.5');
assert.strictEqual(JSON.rawJSON(null).rawJSON, 'null');
assert.strictEqual(JSON.rawJSON(10n).rawJSON, '10');
assert.strictEqual(JSON.rawJSON({ toString() { return '7'; } }).rawJSON, '7');

{
  const raw = JSON.rawJSON('1');
  assert.strictEqual(Object.getPrototypeOf(raw), null);
  assert.strictEqual(Object.isFrozen(raw), true);
  assert.deepStrictEqual(Reflect.ownKeys(raw), ['rawJSON']);
  assert.deepStrictEqual(Object.getOwnPropertyDescriptor(raw, 'rawJSON'), { value: '1', writable: false, enumerable: true, configurable: false });
  assert.throws(() => { 'use strict'; raw.rawJSON = '2'; }, TypeError);
}

assert.deepStrictEqual([JSON.rawJSON.name, JSON.rawJSON.length, JSON.isRawJSON.name, JSON.isRawJSON.length], ['rawJSON', 1, 'isRawJSON', 1]);
assert.strictEqual(JSON.isRawJSON(JSON.rawJSON('1')), true);
for (const value of [{ rawJSON: '1' }, Object.freeze(Object.assign(Object.create(null), { rawJSON: '1' })), undefined, null, '1', new Proxy(JSON.rawJSON('1'), {})])
  assert.strictEqual(JSON.isRawJSON(value), false);
assert.strictEqual(JSON.isRawJSON(), false);

assert.strictEqual(JSON.stringify({ id: JSON.rawJSON('9007199254740993'), price: JSON.rawJSON('1.10') }), '{"id":9007199254740993,"price":1.10}');
assert.strictEqual(JSON.stringify({ a: JSON.rawJSON('1'), b: [JSON.rawJSON('"x"')] }, null, 2), '{\n  "a": 1,\n  "b": [\n    "x"\n  ]\n}');
assert.strictEqual(JSON.stringify({ a: 1, b: 2 }, (key, value) => key === 'a' ? JSON.rawJSON('99') : value), '{"a":99,"b":2}');
assert.strictEqual(JSON.stringify({ a: JSON.rawJSON('1'), b: 2 }, ['a']), '{"a":1}');
assert.strictEqual(JSON.stringify({ a: { toJSON() { return JSON.rawJSON('7'); } } }), '{"a":7}');
assert.strictEqual(JSON.stringify({ ...JSON.rawJSON('1') }), '{"rawJSON":"1"}');

{
  const seen = [];
  JSON.stringify({ a: JSON.rawJSON('1') }, function (key, value) { if (key) seen.push(JSON.isRawJSON(value)); return value; });
  assert.deepStrictEqual(seen, [true]);
  const shared = JSON.rawJSON('5');
  assert.strictEqual(JSON.stringify([shared, shared, { shared }]), '[5,5,{"shared":5}]');
  const many = [];
  for (let i = 0; i < 5000; i++) many.push(JSON.rawJSON(String(i)));
  assert.deepStrictEqual(JSON.parse(JSON.stringify(many)).length, 5000);
}

console.log('ok');
