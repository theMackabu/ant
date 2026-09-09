'use strict';
function Fresh(value) { this.value = value; }
function same(actual, expected, label) {
  if (!Object.is(actual, expected)) throw new Error(label + ': ' + actual + ' != ' + expected);
}
// Exceed collection thresholds so fresh allocations also exercise recycled slots.
for (let i = 0; i < 50000; i++) {
  const object = new Fresh(i);
  same(object.value, i, 'fresh constructor value');
  same(Object.getPrototypeOf(object), Fresh.prototype, 'fresh prototype');
  same(Object.isExtensible(object), true, 'fresh extensibility');
  same(Object.keys(object).join(','), 'value', 'no stale properties');
  same(object.previous, undefined, 'no stale accessor');
  Object.defineProperty(object, 'previous', { get() { return i; }, configurable: true });
  object.extra = [i, -0];
  Object.freeze(object);
  const array = new Array(3);
  same(array.length, 3, 'fresh array length');
  same(0 in array, false, 'fresh array holes');
  array[1] = i;
  same(array[1], i, 'fresh array value');
  same(Object.isExtensible(array), true, 'fresh array flags');
}
console.log('PASS object arena reuse');
