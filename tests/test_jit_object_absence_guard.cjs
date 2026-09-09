'use strict';
const assert = require('node:assert');

function read(object) { return object.marker; }
function add(object, value) { object.marker = value; }
function computed(object, value) { object[['mar', 'ker'].join('')] = value; }
function define(object, value) {
  Object.defineProperty(object, 'marker', { value, configurable: true, writable: true });
}
function accessor(object, value) {
  Object.defineProperty(object, 'marker', { get() { return value; }, configurable: true });
}

// Train the add transition on the same root shape as the intermediate prototype.
// Then shadow a cached inherited property through each property-add route.
for (const write of [add, computed, define, accessor]) {
  const grandparent = { marker: 11 };
  const middle = Object.create(grandparent);
  const child = Object.create(middle);
  for (let i = 0; i < 1000; i++) {
    assert.strictEqual(read(child), 11);
    write(Object.create(grandparent), i);
  }
  write(middle, 22);
  assert.strictEqual(read(child), 22, 'intermediate prototype shadows cached holder');
  delete middle.marker;
  for (let i = 0; i < 1000; i++) assert.strictEqual(read(child), 11);
  write(middle, 33);
  assert.strictEqual(read(child), 33, 'a fresh probe rearms the guard');
}

// Absence of well-known symbols also needs object-specific invalidation.
function number(object) { return +object; }
const conversionBase = { valueOf() { return 7; } };
const conversionMiddle = Object.create(conversionBase);
const conversionChild = Object.create(conversionMiddle);
for (let i = 0; i < 1000; i++) assert.strictEqual(number(conversionChild), 7);
conversionMiddle[Symbol.toPrimitive] = () => 19;
assert.strictEqual(number(conversionChild), 19);

function Ctor() {}
const instance = new Ctor();
function check(object) { return object instanceof Ctor; }
for (let i = 0; i < 1000; i++) assert.strictEqual(check(instance), true);
Object.defineProperty(Ctor, Symbol.hasInstance, { value() { return false; } });
assert.strictEqual(check(instance), false);

console.log('PASS object absence guard invalidation');
