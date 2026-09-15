const assert = require('node:assert');

function arithmetic(n, i, j, carry) {
  while (--n >= 0) {
    i++;
    j += 2;
    carry = (carry + i + j) | 0;
  }
  return [n, i, j, carry];
}

function missing(value) {
  'use strict';
  value = 0;
  for (let i = 0; i < 50000; i++) value++;
  return [value, arguments.length, arguments[0]];
}

function captured(a, b, count) {
  const read = () => a;
  const change = () => { a += 7; };
  while (count-- > 0) {
    a++;
    change();
    b += read();
  }
  return [read(), b, count];
}

function bailout(value, count, extra) {
  for (let i = 0; i < count; i++) {
    value++;
    if (i === count - 1) value += extra;
  }
  return value;
}

function keepChain(value, count) {
  for (let i = 0; i < count; i++) value = { previous: value, index: i };
  return value;
}

function selfTail(count, value) {
  for (let i = 0; i < 1; i++) value += count;
  count--;
  if (count <= 0) return value;
  return selfTail(count, value);
}

function increment(value, count) {
  for (let i = 0; i < count; i++) value++;
  return value;
}

function callIncrement(value, count) {
  return increment(value, count);
}

function changeType(value, count, replacement) {
  for (let i = 0; i < count; i++) {
    value++;
    if (i === 5) value = replacement;
  }
  return value;
}

function withThis(value) {
  for (let i = 0; i < 10; i++) value++;
  return [this, value];
}

function callWithThis(value) {
  return withThis(value);
}

function boxedThis(value) {
  for (let i = 0; i < 10; i++) value++;
  return [typeof this, this.valueOf(), value];
}

function strictThis(value) {
  'use strict';
  for (let i = 0; i < 10; i++) value++;
  return [this, value];
}

function recover(value, result, count) {
  try {
    for (let i = 0; i < count; i++) {
      value++;
      if (i === 8) throw 7;
    }
  } catch (e) {
    result += value + e;
  }
  return [value, result];
}

function append(value, count) {
  while (count-- > 0) value += 'x';
  return value;
}

function wide(a, b, c, d, e, f, g, h, last) {
  a = last = 0;
  for (let i = 0; i < 2000; i++) {
    a += i;
    last += 2;
  }
  return a + last;
}

for (let i = 0; i < 300; i++) {
  assert.deepStrictEqual(arithmetic(20, 0, 0, 0), [-1, 20, 40, 630]);
  assert.deepStrictEqual(captured(0, 0, 10), [80, 440, -1]);
  assert.strictEqual(bailout(0, 20, 0), 20);
  assert.deepStrictEqual(recover(0, 0, 20), [9, 16]);
  assert.strictEqual(append('', 12), 'xxxxxxxxxxxx');
  assert.strictEqual(callIncrement(0, 20), 20);
  assert.strictEqual(changeType(0, 20, 0), 14);
  assert.ok(callWithThis(0)[0] === globalThis);
  assert.deepStrictEqual(boxedThis.call(7, 0), ['object', 7, 10]);
  assert.deepStrictEqual(strictThis.call(7, 0), [7, 10]);
}
assert.deepStrictEqual(arithmetic(0, 2, 3, 4), [-1, 2, 3, 4]);
assert.deepStrictEqual(arithmetic(10000, 0, 0, 0), [-1, 10000, 20000, 150015000]);
assert.deepStrictEqual(missing(), [50000, 0, undefined]);
assert.deepStrictEqual(missing(91), [50000, 1, 91]);
assert.strictEqual(wide(), 2003000);
assert.strictEqual(selfTail(2000, 0), 2001000);
assert.ok(Object.is(bailout(-0, 0, 0), -0));
assert.strictEqual(bailout(1.5, 10, 0.25), 11.75);

let effects = 0;
const extra = { valueOf() { effects++; return 7; } };
assert.strictEqual(bailout(10, 2000, extra), 2017);
assert.strictEqual(effects, 1, 'bailout must not replay conversion');
assert.throws(() => bailout(0, 20, Symbol('invalid')), TypeError);
assert.ok(Number.isNaN(bailout(NaN, 20, 0)));
assert.strictEqual(bailout(Infinity, 20, 0), Infinity);
assert.strictEqual(bailout(0, 20, 'x'), '20x');
assert.strictEqual(callIncrement(0n, 20), 20n, 'direct JIT entry guard must return a JS result');
let replacements = 0;
assert.strictEqual(changeType(0, 20, { valueOf() { replacements++; return 10; } }), 24);
assert.strictEqual(replacements, 1, 'store bailout must preserve the old parameter and pending RHS');
const receiverResult = callWithThis(0n);
assert.ok(receiverResult[0] === globalThis, 'entry fallback must normalize sloppy this');
assert.strictEqual(receiverResult[1], 10n);
assert.deepStrictEqual(boxedThis.call(7, 0n), ['object', 7, 10n]);
assert.deepStrictEqual(strictThis.call(7, 0n), [7, 10n]);

const initial = { marker: 91 };
let chain = keepChain(initial, 60000);
for (let i = 59999; i >= 0; i--) {
  assert.strictEqual(chain.index, i);
  chain = chain.previous;
}
assert.strictEqual(chain, initial, 'promoted heap parameters must stay rooted across GC');

console.log('PASS JIT parameter registers, OSR, bailout, captures, self-tail calls and GC');
