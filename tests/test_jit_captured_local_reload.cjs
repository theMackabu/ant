'use strict';
const assert = require('node:assert');
const values = [3, 5, 7, 11, 13, 17, 19, 23];

function capturedRead(array, count) {
  var index = 0, sum = 0;
  function touch() { index = 2; }
  for (var outer = 0; outer < 2; outer++)
    for (var inner = 0; inner < count; inner++) {
      sum += array[index];
      touch();
      // No branch between the mutation and read: a join used to hide the bug.
      sum += array[index];
    }
  return sum;
}

function capturedType(next) {
  var value = 0;
  function touch() { value = next; }
  touch();
  return value;
}

function capturedArithmetic(next) {
  var value = 0;
  function touch() { value = next; }
  touch();
  return value + 1;
}

function capturedFunction() {
  var fn = () => 3;
  function touch() { fn = () => 7; }
  const first = fn();
  touch();
  return first + fn();
}

for (let i = 0; i < 1000; i++) {
  assert.strictEqual(capturedRead(values, 2), 52, `same-block captured read ${i}`);
  assert.strictEqual(capturedType(2), 2);
  assert.strictEqual(capturedArithmetic(2), 3);
  assert.strictEqual(capturedFunction(), 10);
}
function capturedLoopUpdate(next) {
  var index = 0, seen = 0;
  function touch() { index = next; }
  for (; seen < 1; index++) { seen++; touch(); }
  return index;
}
for (let i = 0; i < 1000; i++) assert.strictEqual(capturedLoopUpdate(4), 5);
assert.strictEqual(capturedLoopUpdate('4'), 5);
assert.strictEqual(capturedLoopUpdate(4n), 5n);
const symbol = Symbol('captured');
const object = { valueOf() { return 6; } };
for (const value of ['2', undefined, null, false, 4n, symbol, object, -0, NaN]) {
  assert.ok(Object.is(capturedType(value), value), `captured type ${typeof value}`);
}
assert.strictEqual(capturedArithmetic('2'), '21');
assert.strictEqual(capturedArithmetic(object), 7);
assert.throws(() => capturedArithmetic(symbol), TypeError);
assert.throws(() => capturedArithmetic(4n), TypeError);
console.log('PASS captured local reload');
