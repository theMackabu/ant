const assert = require('node:assert');
function sum(a, b) { return a + b; }
function tailCall(fn, a, b) { return fn(a, b); }
function tailMethod(receiver, value) { return receiver.add(value); }
function add(value) { return this.offset + value; }
const receiver = {offset: 4, add};
for (let i = 0; i < 1000; i++) {
  assert.strictEqual(tailCall(sum, i, 2), i + 2);
  assert.strictEqual(tailMethod(receiver, i), i + 4);
}
assert.strictEqual(tailCall(sum, 'a', 'b'), 'ab');
assert.strictEqual(tailCall(sum.bind(null, 5), 7, 9), 12);
assert.strictEqual(tailCall((a, b) => a - b, 9, 3), 6);
assert.strictEqual(tailMethod({offset: 10, add}, 3), 13);
assert.strictEqual(tailMethod({add: add.bind({offset: 20}, 3)}, 5), 23);
let gets = 0;
const observed = {add, get offset() { gets++; return 6; }};
assert.strictEqual(tailMethod(observed, 4), 10);
assert.strictEqual(gets, 1);
const error = new Error('getter');
assert.throws(() => tailMethod({add, get offset() {throw error;}}, 4), e => e === error);
function captureAndReturn(value) {
  const read = () => value;
  return read();
}
for (let i = 0; i < 1000; i++) assert.strictEqual(captureAndReturn(i), i);
function withFinally(fn) {
  try { return fn(); } finally { gets++; }
}
for (let i = 0; i < 1000; i++) assert.strictEqual(withFinally(() => 42), 42);
assert.strictEqual(gets, 1001);
function selfTail(count, total) {
  if (count === 0) return total;
  return selfTail(count - 1, total + 1);
}
for (let i = 0; i < 200; i++) assert.strictEqual(selfTail(10, 0), 10);
assert.strictEqual(selfTail(5000, 0), 5000);

function readValues(object, index) { return object.values[index]; }
function tailReadValues(object, index) { return readValues(object, index); }
const good = {values: [1, 2]};
for (let i = 0; i < 500; i++) assert.strictEqual(readValues(good, 0), 1);
for (let i = 0; i < 500; i++) assert.strictEqual(tailReadValues(good, 0), 1);
let reads = 0;
assert.strictEqual(tailReadValues({get values() {reads++; return ['x'];}}, 0), 'x');
assert.strictEqual(reads, 1, 'element fallback must not replay a getter');

function strictThis() { 'use strict'; return this; }
function sloppyThis() { return this; }
function tailStrictThis(fn) { return fn(); }
function tailSloppyThis(fn) { return fn(); }
const callers = {tailStrictThis, tailSloppyThis};
for (let i = 0; i < 1000; i++) {
  assert.strictEqual(callers.tailStrictThis(strictThis), undefined);
  assert.strictEqual(callers.tailSloppyThis(sloppyThis), globalThis);
}
assert.strictEqual(callers.tailStrictThis(strictThis.bind(null)), null);
assert.strictEqual(callers.tailSloppyThis(sloppyThis.bind(null)), globalThis);
function tailPrimitive() { return (3).readThis(); }
Number.prototype.readThis = sloppyThis;
try {
  for (let i = 0; i < 1000; i++) {
    const boxed = tailPrimitive();
    assert.strictEqual(typeof boxed, 'object');
    assert.strictEqual(boxed.valueOf(), 3);
  }
} finally { delete Number.prototype.readThis; }

function updateRead(box, source) { box.writes = box.writes + 1; return source.value; }
function tailUpdateRead(box, source) { return updateRead(box, source); }
const box = {writes: 0};
for (let i = 0; i < 1000; i++) assert.strictEqual(tailUpdateRead(box, {value: 7}), 7);
assert.strictEqual(box.writes, 1000);
let readEffects = 0;
assert.strictEqual(tailUpdateRead(box, {get value() {readEffects++; return 9;}}), 9);
assert.strictEqual(box.writes, 1001, 'getter fallback must not replay the store');
assert.strictEqual(readEffects, 1);
assert.throws(() => tailUpdateRead(box, {get value() {throw error;}}), e => e === error);
assert.strictEqual(box.writes, 1002, 'throwing getter must not replay the store');

function updateLength(box, source) { box.writes = box.writes + 1; return source.value.length; }
function tailUpdateLength(box, source) { return updateLength(box, source); }
for (let i = 0; i < 1000; i++) assert.strictEqual(tailUpdateLength(box, {value: [1, 2]}), 2);
let stores = 0;
const setterBox = {set writes(value) {stores++;}};
assert.strictEqual(tailUpdateLength(setterBox, {value: {get length() {return 5;}}}), 5);
assert.strictEqual(stores, 1, 'length fallback must not replay the setter');
console.log('PASS tail call inlining');
