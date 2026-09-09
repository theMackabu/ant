'use strict';
function same(actual, expected, message = 'property value') {
  if (!Object.is(actual, expected)) throw new Error(message);
}

function read(object) { return object.marker; }
function readApply(fn) { return fn.apply; }
function first() {}
function second() {}
first.marker = 11;
second.marker = 22;
const object = Object.create(first);
const originalApply = Function.prototype.apply;
for (let i = 0; i < 1000; i++) {
  same(read(object), 11);
  same(readApply(first), originalApply);
}

first.marker = 33;
same(read(object), 33, 'read current value from cached callable holder');
Object.setPrototypeOf(object, second);
for (let i = 0; i < 1000; i++) same(read(object), 22);

let gets = 0;
Object.defineProperty(second, 'marker', {
  configurable: true,
  get() { gets++; same(this, object); return -0; },
});
for (let i = 0; i < 1000; i++) same(Object.is(read(object), -0), true);
same(gets, 1000, 'accessor is invoked once with the receiver');
delete second.marker;
same(read(object), undefined);

const parent = { marker: 44 };
Object.setPrototypeOf(second, parent);
for (let i = 0; i < 1000; i++) same(read(object), 44);
second.marker = 55;
same(read(object), 55, 'new callable prototype property shadows its parent');
object.marker = 66;
same(read(object), 66, 'own property shadows callable prototype');
delete object.marker;
same(read(object), 55);

const replacement = function () { return 'replacement'; };
try {
  Function.prototype.apply = replacement;
  for (let i = 0; i < 1000; i++) same(readApply(first), replacement);
  first.apply = originalApply;
  same(readApply(first), originalApply);
  delete first.apply;
  same(readApply(first), replacement);
} finally {
  Function.prototype.apply = originalApply;
}
same(readApply(first), originalApply);
const grandparent = { marker: 77 };
const middle = Object.create(grandparent);
const child = Object.create(middle);
for (let i = 0; i < 1000; i++) same(read(child), 77);
middle.marker = 88;
same(read(child), 88, 'ordinary intermediate prototype shadows holder');
function readChangingKind(value) { return value.marker; }
const changingKind = Object.create({ marker: 99 });
for (let i = 0; i < 1000; i++) same(readChangingKind(changingKind), 99);
Object.setPrototypeOf(changingKind, first);
for (let i = 0; i < 1000; i++) same(readChangingKind(changingKind), 33);
Object.setPrototypeOf(changingKind, parent);
same(readChangingKind(changingKind), 44, 'prototype kind changes after compilation');
console.log('PASS callable prototype field cache');
