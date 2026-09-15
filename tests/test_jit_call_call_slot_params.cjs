'use strict';
const assert = require('node:assert');

function add(left) { return right => left + right; }
function sum(value, count) {
  let total = 0;
  while (count-- > 0) {
    value++;
    total += add(1)(value);
  }
  return total;
}

// The loop promotes numeric parameters, but the curried call reads its
// second argument through a frame-slot pointer after invoking the first call.
for (let i = 0; i < 300; i++) {
  assert.strictEqual(sum(0, 1000), 501500);
}
// Also cover the generic two-call fallback rather than closure-step fusion.
function plusOne(value) { return value + 1; }
function select(ignored) { return plusOne; }
function fallback(value, count) {
  let total = 0;
  while (count-- > 0) {
    value++;
    total += select(1)(value);
  }
  return total;
}
function getPayload(value) { return value.payload; }
function selectPayload(ignored) { return getPayload; }
function objectParameter(value, count) {
  let result = value; // An ordinary read makes this parameter a cache candidate.
  while (count-- > 0) result = selectPayload(0)(value);
  return result;
}
for (let i = 0; i < 300; i++) {
  assert.strictEqual(fallback(0, 1000), 501500);
  assert.strictEqual(objectParameter({ payload: i }, 1000), i);
}
// A captured parameter must be read after the first call mutates it.
function identity(value) { return value; }
function captured(value, count) {
  function first(ignored) { value++; return identity; }
  let total = 0;
  while (count-- > 0) total += first(0)(value);
  return total;
}
for (let i = 0; i < 300; i++) assert.strictEqual(captured(0, 1000), 500500);
console.log('PASS curried call parameter slots');
