const assert = require('node:assert');

function increment(value) { const old = value++; return [old, value]; }
function decrement(value) { const old = value--; return [old, value]; }
function* interpretedIncrement(value) { const old = value++; return [old, value]; }
function* interpretedDecrement(value) { const old = value--; return [old, value]; }

for (let i = 0; i < 500; i++) {
  assert.deepStrictEqual(increment(i), [i, i + 1]);
  assert.deepStrictEqual(decrement(i), [i, i - 1]);
}

const builtin = Math.abs;
const previous = Object.getOwnPropertyDescriptor(builtin, Symbol.toPrimitive);
const paths = [
  [increment, decrement],
  [value => interpretedIncrement(value).next().value,
    value => interpretedDecrement(value).next().value],
];
let calls = 0;
try {
  for (const [primitive, old, next, prior] of [
    [41n, 41n, 42n, 40n], ['41', 41, 42, 40], [41, 41, 42, 40],
  ]) {
    Object.defineProperty(builtin, Symbol.toPrimitive, {
      configurable: true,
      value(hint) {
        assert.strictEqual(this, builtin);
        assert.strictEqual(hint, 'number');
        calls++;
        return primitive;
      },
    });
    calls = 0;
    for (const [inc, dec] of paths) {
      assert.deepStrictEqual(inc(builtin), [old, next]);
      assert.deepStrictEqual(dec(builtin), [old, prior]);
    }
    assert.strictEqual(calls, 4, 'one conversion per update');
  }
  Object.defineProperty(builtin, Symbol.toPrimitive, {
    configurable: true,
    value() { calls++; return Symbol('cannot update'); },
  });
  calls = 0;
  for (const [inc, dec] of paths) {
    assert.throws(() => inc(builtin), TypeError);
    assert.throws(() => dec(builtin), TypeError);
  }
  assert.strictEqual(calls, 4);

  const failure = new Error('conversion failed');
  Object.defineProperty(builtin, Symbol.toPrimitive, {
    configurable: true,
    value() { throw failure; },
  });
  for (const [inc, dec] of paths) {
    assert.throws(() => inc(builtin), error => error === failure);
    assert.throws(() => dec(builtin), error => error === failure);
  }
} finally {
  if (previous) Object.defineProperty(builtin, Symbol.toPrimitive, previous);
  else delete builtin[Symbol.toPrimitive];
}
console.log('PASS builtin postfix conversion');
