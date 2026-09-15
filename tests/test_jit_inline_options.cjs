const assert = require('node:assert');

function optionsValue(value, options) {
  options = options || {};
  if (typeof value === 'number') return options.inlineOption ? 2 : 3;
  return 0;
}
function localOptions(value, options) {
  const opts = options || {};
  return opts.inlineOption ? 2 : 3;
}
function conditionalOptions(value, options) {
  if (!options) options = {};
  return options.inlineOption ? 2 : 3;
}
function conditionalDriver(n, options) {
  let sum = 0;
  for (let i = 0; i < n; i++) sum += conditionalOptions(i, options);
  return sum;
}
function optionsDriver(n, options) {
  let sum = 0;
  for (let i = 0; i < n; i++) sum += optionsValue(i, options);
  return sum;
}
function localDriver(n, options) {
  let sum = 0;
  for (let i = 0; i < n; i++) sum += localOptions(i, options);
  return sum;
}
for (let i = 0; i < 200; i++) {
  assert.strictEqual(optionsDriver(100), 300);
  assert.strictEqual(localDriver(100), 300);
  assert.strictEqual(conditionalDriver(100), 300);
}
for (const value of [undefined, null, false, 0, '', NaN, 0n, {}, { inlineOption: false }]) {
  assert.strictEqual(optionsDriver(100, value), 300);
  assert.strictEqual(localDriver(100, value), 300);
  assert.strictEqual(conditionalDriver(100, value), 300);
}
assert.strictEqual(optionsDriver(100, { inlineOption: true }), 200);
assert.strictEqual(conditionalDriver(100, { inlineOption: true }), 200);
Object.prototype.inlineOption = true;
assert.strictEqual(optionsDriver(100), 200, 'prototype additions invalidate an absent read');
delete Object.prototype.inlineOption;
let reads = 0;
const receivers = [];
Object.defineProperty(Object.prototype, 'inlineOption', {
  configurable: true,
  get() { reads++; receivers.push(this); return true; },
});
try {
  assert.strictEqual(optionsDriver(3), 6);
  assert.strictEqual(reads, 3, 'getters run once per call');
  assert.notStrictEqual(receivers[0], receivers[1], 'getters receive fresh options objects');
  assert.notStrictEqual(receivers[1], receivers[2]);
} finally {
  delete Object.prototype.inlineOption;
}

// The private empty object stays live through the function's traced site cache.
const ring = new Array(64);
for (let round = 0; round < 8; round++) {
  for (let i = 0; i < 25000; i++) ring[i & 63] = { inlineOption: true, i };
  assert.strictEqual(optionsDriver(100), 300, 'empty site survives allocation churn');
  assert.strictEqual(localDriver(100), 300);
}

function reassign(value, object) { value = value + 1; return object.result + value; }
function reassignDriver(n, object) {
  let sum = 0;
  for (let i = 0; i < n; i++) sum += reassign(i, object);
  return sum;
}
assert.strictEqual(reassignDriver(2000, { result: 1 }), 2000 * 2003 / 2);
let traps = 0;
const proxy = new Proxy({}, { get() { traps++; return 1; } });
assert.strictEqual(reassignDriver(3, proxy), 9, 'fallback receives original arguments');
assert.strictEqual(traps, 3);

function makeObject() { return {}; }
function escaping(n) {
  let previous;
  for (let i = 0; i < n; i++) {
    const next = makeObject();
    assert.notStrictEqual(next, previous, 'escaping objects retain identity');
    previous = next;
  }
}
escaping(1000);

function maybeEscape(flag) {
  const object = {};
  let result;
  if (flag) result = object;
  else result = 1;
  return result;
}
function joinedEscape() { return maybeEscape(true); }
for (let i = 0; i < 500; i++) {
  assert.notStrictEqual(joinedEscape(), joinedEscape(), 'branch joins preserve escape information');
}

function passObject(callback) { const object = {}; return callback(object); }
function passedDriver(callback) { return passObject(callback); }
let previous;
for (let i = 0; i < 500; i++) {
  passedDriver(object => {
    assert.notStrictEqual(object, previous, 'call arguments escape');
    previous = object;
  });
}

function storedObject(box) { box.value = {}; return box.value; }
function storedDriver(box) { return storedObject(box); }
const box = {};
for (let i = 0; i < 500; i++) {
  const old = box.value;
  assert.notStrictEqual(storedDriver(box), old, 'property stores escape');
}
console.log('PASS inline options');
