const assert = require('node:assert');

function setLength(a, v) { return (a.length = v); }

for (let i = 0; i < 3000; i++) {
  const a = [1, 2, 3, 4];
  assert.strictEqual(setLength(a, '3'), '3');
  assert.deepStrictEqual(a, [1, 2, 3]);

  setLength(a, -0);
  assert.strictEqual(a.length, 0);

  const b = [1, 2];
  setLength(b, '');
  assert.strictEqual(b.length, 0);

  const c = [];
  setLength(c, true);
  assert.strictEqual(c.length, 1);

  let calls = 0;
  const d = [1, 2, 3];
  setLength(d, { valueOf() { calls++; return 1; } });
  assert.strictEqual(d.length, 1);
  assert.strictEqual(calls, 2);

  assert.throws(() => setLength([], '1.5'), RangeError);
  assert.throws(() => setLength([], -1), RangeError);
  assert.throws(() => setLength([], 'abc'), RangeError);
  assert.throws(() => setLength([], { valueOf() { throw new SyntaxError('boom'); } }), SyntaxError);
  assert.throws(() => setLength([1], 0n), TypeError);
  assert.throws(() => setLength([1], { valueOf: () => 0n }), TypeError);
  assert.throws(() => setLength([1], Symbol()), TypeError);

  const frozen = Object.freeze([1]);
  assert.throws(() => { 'use strict'; frozen.length = '0'; }, TypeError);
  assert.strictEqual(frozen.length, 1);
}

console.log('ok');
