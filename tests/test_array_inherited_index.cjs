// A store to a missing array element (a hole, or past the end) consults the
// prototype chain like any other property store: an inherited setter runs
// instead of creating the element, and an inherited read-only property
// blocks it. Own-property queries on a hole never see the inherited one.
// Stores used to write the dense slot directly, and hasOwn/descriptors on a
// hole reported the prototype's property as own.
//
// Compiled code adds elements inline only while Array.prototype and
// Object.prototype have never held an index-like key; that is sticky, so the
// sections that put one there come last.
const assert = require('node:assert');

function set(arr, i, v) { arr[i] = v; }
function setStrict(arr, i, v) { 'use strict'; arr[i] = v; }
function fill(arr, n) { for (let i = 0; i < n; i++) arr[i] = i; return arr; }
function append(arr, v) { arr[arr.length] = v; return arr.length; }
function fillHole(arr, i, v) { arr[i] = v; return Object.hasOwn(arr, i); }
function pushOne(arr, v) { return arr.push(v); }

for (let i = 0; i < 5000; i++) {
  set([], i & 7, i);
  setStrict([], i & 7, 'x');
  fill([], 16);
  fill(new Array(16), 16);
  append([1, 2], i);
  fillHole([0, , 2], 1, i);
  pushOne([], i);
}

// own prototypes with setters, read-only and writable index properties
const seen = [];
const proto = Object.create(Array.prototype);
Object.defineProperty(proto, '2', { set(v) { seen.push('2:' + v); }, configurable: true });
Object.defineProperty(proto, '5', { set(v) { seen.push('5:' + v); }, configurable: true });
Object.defineProperty(proto, '1', { value: 'ro', writable: false });
proto[7] = 'shadowable';

const holey = [0, , , 3];
Object.setPrototypeOf(holey, proto);

set(holey, 2, 'hole');
set(holey, 5, 'append');
assert.deepStrictEqual(seen, ['2:hole', '5:append']);
assert.strictEqual(Object.hasOwn(holey, 2), false);
assert.strictEqual(Object.hasOwn(holey, 5), false);
assert.strictEqual(holey.length, 4);

set(holey, 1, 'blocked');
assert.strictEqual(Object.hasOwn(holey, 1), false);
assert.strictEqual(holey[1], 'ro');
assert.throws(() => setStrict(holey, 1, 'blocked'), TypeError);

// an inherited writable data property does not stop the store
set(holey, 7, 'own');
assert.strictEqual(Object.hasOwn(holey, 7), true);
assert.strictEqual(holey[7], 'own');
assert.strictEqual(proto[7], 'shadowable');

// own-property queries on holes
assert.strictEqual(Object.hasOwn(holey, 1), false);
assert.strictEqual(holey.hasOwnProperty(2), false);
assert.strictEqual(Object.getOwnPropertyDescriptor(holey, 1), undefined);
assert.strictEqual(1 in holey, true);
assert.strictEqual(holey[0], 0);
assert.strictEqual(Object.getOwnPropertyDescriptor(holey, 0).value, 0);

// a writable property nearer on the chain shadows a setter further up, so
// the store makes an ordinary element
const far = Object.create(Array.prototype);
Object.defineProperty(far, '1', { set() { throw new Error('shadowed setter ran'); } });
const near = Object.create(far);
Object.defineProperty(near, '1', { value: 'near', writable: true, configurable: true });
const shadowed = [0, , 2];
Object.setPrototypeOf(shadowed, near);
set(shadowed, 1, 'own');
assert.strictEqual(Object.hasOwn(shadowed, 1), true);
assert.deepStrictEqual(Object.keys(shadowed), ['0', '1', '2']);

// compiled appends, hole fills and pushes add elements inline
assert.strictEqual(append([1, 2], 3), 3);
assert.strictEqual(fillHole([0, , 2], 1, 'x'), true);
const holes = fill(new Array(4), 4);
assert.deepStrictEqual(holes, [0, 1, 2, 3]);
assert.strictEqual(holes.includes(3), true);
assert.strictEqual(fill(new Array(3000), 3000)[2999], 2999);

const pushed = [];
for (let i = 0; i < 20; i++) assert.strictEqual(pushOne(pushed, i), i + 1);
assert.strictEqual(pushed.includes(19), true);

// ... but not to arrays that cannot grow
for (const lock of [Object.preventExtensions, Object.seal, Object.freeze]) {
  const locked = lock([0, , 2]);
  assert.strictEqual(fillHole(locked, 1, 'x'), false);
  assert.strictEqual(append(locked, 'y'), 3);
  assert.strictEqual(locked.length, 3);
  assert.throws(() => setStrict(locked, 3, 'y'), TypeError);
  assert.throws(() => pushOne(locked, 'z'), TypeError);
  assert.strictEqual(locked.length, 3);
}
assert.throws(() => Object.freeze([1]).push(), TypeError);
assert.strictEqual(Object.seal([1]).push(), 1);

// ... and only with Array.prototype.push itself
const realPush = Array.prototype.push;
Array.prototype.push = function (v) { return 'replaced:' + v; };
try {
  assert.strictEqual(pushOne([], 1), 'replaced:1');
} finally {
  Array.prototype.push = realPush;
}
assert.strictEqual(pushOne([7], 8), 2);

// a setter added to Object.prototype after compilation is still honoured
let late = '';
Object.defineProperty(Object.prototype, '2', { set(v) { late += v + ';'; }, configurable: true });
try {
  const arr = fill([], 2);
  assert.strictEqual(append(arr, 'append'), 2);
  const sparse = fill(new Array(4), 2);
  assert.strictEqual(fillHole(sparse, 2, 'hole'), false);
  assert.strictEqual(pushOne([0, 1], 'push'), 3);
  assert.strictEqual(late, 'append;hole;push;');
} finally {
  delete Object.prototype[2];
}

// setters on Array.prototype itself, which is an array; push stores through
// them too
const calls = [];
Object.defineProperty(Array.prototype, '3', { set(v) { calls.push(v); }, configurable: true });
try {
  const arr = [0, 1, 2];
  set(arr, 3, 'index');
  arr.push('push', 'after');
  assert.deepStrictEqual(calls, ['index', 'push']);
  assert.strictEqual(Object.hasOwn(arr, 3), false);
  assert.strictEqual(arr[4], 'after');
  assert.strictEqual(arr.length, 5);
} finally {
  delete Array.prototype[3];
}

// with the setter gone, the same stores land in the array again
const plain = [0, 1, 2];
set(plain, 3, 'own');
plain.push('pushed');
assert.deepStrictEqual(plain, [0, 1, 2, 'own', 'pushed']);

console.log('PASS array stores to missing elements respect the prototype chain');
