const assert = require('node:assert');

function keyed(log, name) {
  return { toString() { log.push('key ' + name); return name; } };
}

{
  const log = [];
  const o = { [keyed(log, 'a')]: (log.push('value a'), 1), [keyed(log, 'b')]: (log.push('value b'), 2) };
  assert.deepStrictEqual(log, ['key a', 'value a', 'key b', 'value b']);
  assert.deepStrictEqual(o, { a: 1, b: 2 });
}

{
  const seen = {};
  const key = { toString() { seen.ran = 'key first'; return 'k'; } };
  assert.strictEqual(({ [key]: seen.ran }).k, 'key first');
  let flag = 0;
  const bump = { [Symbol.toPrimitive]() { flag++; return 'p'; } };
  assert.deepStrictEqual({ [bump]: flag, [bump]: flag }, { p: 2 });
  assert.strictEqual(flag, 2);
}

{
  const log = [];
  const bad = { toString() { log.push('key'); throw new RangeError('key'); } };
  assert.throws(() => ({ [bad]: (log.push('value'), 1) }), RangeError);
  assert.deepStrictEqual(log, ['key']);
  const noPrimitive = { toString() { return {}; }, valueOf() { return {}; } };
  assert.throws(() => ({ [noPrimitive]: log.push('value') }), TypeError);
  assert.deepStrictEqual(log, ['key']);
  assert.throws(() => ({ [keyed(log, 'x')]: (() => { throw new EvalError('value'); })() }), EvalError);
  assert.deepStrictEqual(log, ['key', 'key x']);
}

{
  const log = [];
  const sym = Symbol('s');
  const symKey = { [Symbol.toPrimitive](hint) { log.push(hint); return sym; } };
  const o = { [symKey]: log.length, [1 + 1]: log.length, [null]: 1, [undefined]: 2, [1n]: 3, [-0]: 4, [1.5]: 5, [true]: 6 };
  assert.deepStrictEqual(log, ['string']);
  assert.strictEqual(o[sym], 1);
  assert.deepStrictEqual(Object.keys(o), ['0', '1', '2', 'null', 'undefined', '1.5', 'true']);
  assert.strictEqual(o[2], 1);
}

{
  const log = [];
  const o = {
    [keyed(log, 'm')]() { return 1; },
    get [keyed(log, 'g')]() { return 2; },
    set [keyed(log, 'g')](v) {},
    [keyed(log, 'f')]: function () {},
    [keyed(log, 'arrow')]: () => {},
    [keyed(log, 'cls')]: class {},
    [keyed(log, 'n')]: 1,
    ['lit']: (log.push('value lit'), 3),
    [`t${1}`]: (log.push('value t1'), 4),
  };
  assert.deepStrictEqual(log, ['key m', 'key g', 'key g', 'key f', 'key arrow', 'key cls', 'key n', 'value lit', 'value t1']);
  assert.deepStrictEqual([o.m(), o.g, o.m.name, o.f.name, o.arrow.name, o.cls.name, o.n, o.lit, o.t1], [1, 2, 'm', 'f', 'arrow', 'cls', 1, 3, 4]);
  assert.strictEqual(Object.getOwnPropertyDescriptor(o, 'g').get.name, 'get g');
}

{
  let total = 0;
  for (let i = 0; i < 30000; i++) {
    let order = '';
    const key = { toString() { order += 'k'; return 'p' + (i & 3); } };
    const o = { [key]: (order += 'v', i), [i & 7]: i, ['s' + (i & 1)]: order };
    if (order !== 'kv' || o['p' + (i & 3)] !== i || o[i & 7] !== i || o['s' + (i & 1)] !== 'kv') throw new Error('bad at ' + i);
    total += Object.keys(o).length;
  }
  assert.strictEqual(total, 90000);
}

console.log('ok');
