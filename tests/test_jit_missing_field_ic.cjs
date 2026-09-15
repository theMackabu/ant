'use strict';
const assert = require('node:assert');

function missingField(object) { return object.absentJitField; }
function optionalMissingField(object) { return object?.absentJitField; }
function loopMissingField(object, n) {
  let count = 0;
  for (let i = 0; i < n; i++) if (optionalMissingField(object) === undefined) count++;
  return count;
}
function warm(object) {
  for (let i = 0; i < 500; i++) {
    assert.strictEqual(missingField(object), undefined);
    assert.strictEqual(optionalMissingField(object), undefined);
  }
  assert.strictEqual(loopMissingField(object, 2000), 2000);
}

for (const object of [{}, Object.create(null)]) {
  warm(object);
  object.absentJitField = 41;
  assert.strictEqual(missingField(object), 41);
  assert.strictEqual(optionalMissingField(object), 41);
  assert.strictEqual(loopMissingField(object, 2000), 0);
  delete object.absentJitField;
  warm(object);
}

for (const write of [
  (object) => { object.absentJitField = 42; },
  (object) => { object[['absent', 'JitField'].join('')] = 42; },
  (object) => Object.defineProperty(object, 'absentJitField', { value: 42, configurable: true }),
]) {
  const base = {};
  const middle = Object.create(base);
  const object = Object.create(middle);
  warm(object);
  write(base);
  assert.strictEqual(missingField(object), 42);
  delete base.absentJitField;
  warm(object);
  write(middle);
  assert.strictEqual(optionalMissingField(object), 42);
}

const object = {};
warm(object);
Object.setPrototypeOf(object, { absentJitField: 43 });
assert.strictEqual(missingField(object), 43);
const different = Object.create({ absentJitField: 44 });
assert.strictEqual(missingField(different), 44, 'same own shape, different prototype');

let reads = 0;
const base = {};
const child = Object.create(base);
warm(child);
Object.defineProperty(base, 'absentJitField', {
  configurable: true,
  get() { reads++; assert.strictEqual(this, child); return 45; },
});
assert.strictEqual(optionalMissingField(child), 45);
assert.strictEqual(reads, 1, 'getter is not replayed');
delete base.absentJitField;
warm(child);
Object.setPrototypeOf(base, new Proxy({}, {
  get(target, key, receiver) {
    assert.strictEqual(receiver, child);
    reads++;
    return 46;
  },
}));
assert.strictEqual(optionalMissingField(child), 46);
assert.strictEqual(reads, 2, 'new prototype proxy is observed once');

function index(object) { return object['0']; }
for (let i = 0; i < 500; i++) assert.strictEqual(index({}), undefined);
assert.strictEqual(index([47]), 47, 'numeric keys retain dense-element fallback');
assert.strictEqual(index(new String('x')), 'x', 'string-wrapper indices are not cached absent');
function description(object) { return object.description; }
for (let i = 0; i < 500; i++) assert.strictEqual(description({}), undefined);
const boxedSymbol = Object(Symbol('outside-shape'));
Object.setPrototypeOf(boxedSymbol, null);
// Ant supplies this virtual wrapper property even without Symbol.prototype.
if (typeof Ant !== 'undefined') {
  assert.strictEqual(description(boxedSymbol), 'outside-shape');
}

function optionalDescription(object) { return object?.description; }
function descriptionByKey(object, key) { return object[key]; }
for (let i = 0; i < 1000; i++) {
  assert.strictEqual(optionalDescription({}), undefined);
  assert.strictEqual(descriptionByKey({}, ['descr', 'iption'].join('')), undefined);
}
for (const text of [undefined, '', 'interned-description', '\u03bb']) {
  const wrapper = Object(Symbol(text));
  if (typeof Ant !== 'undefined') Object.setPrototypeOf(wrapper, null);
  for (let i = 0; i < 1000; i++) {
    const key = ['descr', 'iption'].join('');
    assert.strictEqual(description(wrapper), text);
    assert.strictEqual(optionalDescription(wrapper), text);
    assert.strictEqual(descriptionByKey(wrapper, key), text,
      `computed Symbol-wrapper description ${JSON.stringify(text)} at ${i}`);
    assert.strictEqual(descriptionByKey(wrapper, key + '\0'), undefined);
  }
}
assert.strictEqual(optionalMissingField(null), undefined);
assert.strictEqual(optionalMissingField(undefined), undefined);
assert.throws(() => missingField(null), TypeError);

// Minor collections must not allow a cached prototype address to identify a new object.
for (let round = 0; round < 12; round++) {
  warm(Object.create({}));
  for (let i = 0; i < 20000; i++) {
    const allocation = { i };
    if (allocation.i < 0) throw new Error('unreachable');
  }
  assert.strictEqual(missingField(Object.create({ absentJitField: round })), round);
}
console.log('PASS missing field IC');
