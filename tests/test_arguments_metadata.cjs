const assert = require('node:assert');
const { types } = require('node:util');

function snapshot(a, b) {
  'use strict';
  return arguments;
}

for (let i = 0; i < 300; i++) {
  const args = snapshot(i, i + 1);
  assert.strictEqual(args.length, 2);
  assert.strictEqual(args[0], i);
  assert.strictEqual(args[1], i + 1);
}

const first = snapshot(1, 2);
const second = snapshot(3, 4);
assert.notStrictEqual(first, second);
assert.strictEqual(types.isArgumentsObject(first), true);
assert.strictEqual(types.isArgumentsObject(second), true);
first[0] = 99;
Object.defineProperty(first, Symbol.toStringTag, { value: 'Changed', configurable: true });
Object.defineProperty(first, Symbol.iterator, { value: () => null, configurable: true });
assert.strictEqual(Object.prototype.toString.call(first), '[object Changed]');
assert.strictEqual(Object.prototype.toString.call(second), '[object Arguments]');
assert.strictEqual(second[0], 3);
assert.deepStrictEqual(Array.from(second), [3, 4]);
assert.throws(() => second.callee, TypeError);
Object.freeze(first);
const third = snapshot(5, 6);
third[0] = 7;
assert.strictEqual(third[0], 7);
assert.strictEqual(Object.prototype.toString.call(third), '[object Arguments]');
assert.strictEqual(snapshot().length, 0);
assert.strictEqual(snapshot(7)[0], 7);
const grown = snapshot(1, 2);
grown[20] = 9;
assert.strictEqual(grown[0], 1);
assert.strictEqual(grown[1], 2);
assert.strictEqual(grown[20], 9);

// The runtime must still build ordinary argument data if the array iterator
// has been removed. Restore it before using iteration or assertion utilities.
const iterator = Object.getOwnPropertyDescriptor(Array.prototype, Symbol.iterator);
let withoutIterator;
try {
  delete Array.prototype[Symbol.iterator];
  withoutIterator = snapshot(11, 12);
} finally {
  Object.defineProperty(Array.prototype, Symbol.iterator, iterator);
}
assert.strictEqual(withoutIterator.length, 2);
assert.strictEqual(withoutIterator[0], 11);
assert.strictEqual(Object.prototype.toString.call(withoutIterator), '[object Arguments]');

// Retained objects must keep independent metadata and elements across GC.
const retained = [];
for (let i = 0; i < 40000; i++) {
  const args = snapshot({ value: i }, i);
  if (i % 1000 === 0) retained.push(args);
}
for (let i = 0; i < retained.length; i++) {
  const args = retained[i];
  assert.strictEqual(args[0].value, i * 1000);
  assert.strictEqual(args[1], i * 1000);
  assert.strictEqual(Object.prototype.toString.call(args), '[object Arguments]');
}
console.log('PASS arguments identity, metadata mutation, iteration and GC');
