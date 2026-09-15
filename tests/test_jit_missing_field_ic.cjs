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
assert.strictEqual(description(boxedSymbol), undefined, 'description is inherited, not virtual');

function optionalDescription(object) { return object?.description; }
function descriptionByKey(object, key) { return object[key]; }
for (let i = 0; i < 1000; i++) {
  assert.strictEqual(optionalDescription({}), undefined);
  assert.strictEqual(descriptionByKey({}, ['descr', 'iption'].join('')), undefined);
}
for (const text of [undefined, '', 'interned-description', '\u03bb']) {
  const wrapper = Object(Symbol(text));
  for (let i = 0; i < 1000; i++) {
    const key = ['descr', 'iption'].join('');
    assert.strictEqual(description(wrapper), text);
    assert.strictEqual(optionalDescription(wrapper), text);
    assert.strictEqual(descriptionByKey(wrapper, key), text,
      `computed Symbol-wrapper description ${JSON.stringify(text)} at ${i}`);
    assert.strictEqual(descriptionByKey(wrapper, key + '\0'), undefined);
  }
}
// Warm cached absence, own data, and inherited accessors on the same sites.
function checkDescription(value, expected) {
  const key = ['descr', 'iption'].join('');
  assert.strictEqual(description(value), expected);
  assert.strictEqual(optionalDescription(value), expected);
  assert.strictEqual(descriptionByKey(value, key), expected);
}
for (let i = 0; i < 1000; i++) checkDescription(boxedSymbol, undefined);
Object.setPrototypeOf(boxedSymbol, Symbol.prototype);
checkDescription(boxedSymbol, 'outside-shape');
Object.defineProperty(boxedSymbol, 'description', { value: 'own', configurable: true });
for (let i = 0; i < 1000; i++) checkDescription(boxedSymbol, 'own');
delete boxedSymbol.description;
checkDescription(boxedSymbol, 'outside-shape');
Object.setPrototypeOf(boxedSymbol, null);
checkDescription(boxedSymbol, undefined);

const symbolDescriptionDescriptor = Object.getOwnPropertyDescriptor(Symbol.prototype, 'description');
const primitiveSymbol = Symbol('inherited');
const inheritedWrapper = Object(primitiveSymbol);
try {
  for (let i = 0; i < 1000; i++) {
    checkDescription(primitiveSymbol, 'inherited');
    checkDescription(inheritedWrapper, 'inherited');
  }
  delete Symbol.prototype.description;
  for (let i = 0; i < 1000; i++) {
    checkDescription(primitiveSymbol, undefined);
    checkDescription(inheritedWrapper, undefined);
  }
  Object.defineProperty(Symbol.prototype, 'description', { value: 'replacement', configurable: true });
  for (let i = 0; i < 1000; i++) {
    checkDescription(primitiveSymbol, 'replacement');
    checkDescription(inheritedWrapper, 'replacement');
  }
  let getterCalls = 0;
  let receiver;
  Object.defineProperty(Symbol.prototype, 'description', {
    configurable: true,
    get() { getterCalls++; receiver = this; return 'getter'; },
  });
  checkDescription(inheritedWrapper, 'getter');
  assert.strictEqual(receiver, inheritedWrapper);
  assert.strictEqual(getterCalls, 3, 'each read invokes the replacement getter once');
} finally {
  Object.defineProperty(Symbol.prototype, 'description', symbolDescriptionDescriptor);
}
checkDescription(primitiveSymbol, 'inherited');
checkDescription(inheritedWrapper, 'inherited');

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

// Callable prototypes use their object identity, not their closure address.
function callableMissing(object) { return object.callableAbsent; }
function callablePrototype() {}
const callableChild = Object.create(callablePrototype);
for (let i = 0; i < 2000; i++) assert.strictEqual(callableMissing(callableChild), undefined);
callablePrototype.callableAbsent = 73;
assert.strictEqual(callableMissing(callableChild), 73);
delete callablePrototype.callableAbsent;
for (let i = 0; i < 2000; i++) assert.strictEqual(callableMissing(callableChild), undefined);
let callableReads = 0;
Object.defineProperty(callablePrototype, 'callableAbsent', {
  get() { callableReads++; return this === callableChild ? 74 : -1; },
});
assert.strictEqual(callableMissing(callableChild), 74);
assert.strictEqual(callableReads, 1);
console.log('PASS callable prototype missing field IC');
