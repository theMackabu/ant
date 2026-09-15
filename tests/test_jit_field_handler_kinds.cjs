'use strict';
const assert = require('node:assert');

function initiallyMissing(value) { return value.handlerKindField; }
function initiallyNullPrototype(value) { return value.handlerKindField; }
function initiallyOwn(value) { return value.handlerKindField; }
function initiallyInherited(value) { return value.handlerKindField; }
function initiallyPrimitive(value) { return value.handlerKindField; }
function optional(value) { return value?.handlerKindField; }
function optionalDriver(value, expected) {
  let matches = 0;
  for (let i = 0; i < 1000; i++) if (optional(value) === expected) matches++;
  return matches;
}
function warm(read, value, expected) {
  for (let i = 0; i < 600; i++) assert.strictEqual(read(value), expected);
}

warm(initiallyMissing, {}, undefined);
warm(initiallyNullPrototype, Object.create(null), undefined);
warm(initiallyOwn, { handlerKindField: 1 }, 1);
warm(initiallyInherited, Object.create({ handlerKindField: 2 }), 2);
warm(initiallyPrimitive, 3, undefined);
assert.strictEqual(optionalDriver({}, undefined), 1000);

const readers = [initiallyMissing, initiallyNullPrototype, initiallyOwn, initiallyInherited, initiallyPrimitive, optional];
function check(value, expected) {
  for (const read of readers) warm(read, value, expected);
  assert.strictEqual(optionalDriver(value, expected), 1000);
}

// Reuse each compiled read site as its handler changes. An own property at
// slot zero, including one containing undefined, is different from absence.
const own = { handlerKindField: 10 };
const nullProto = Object.create(null);
const parent = { handlerKindField: 20 };
const inherited = Object.create(parent);
for (let round = 0; round < 3; round++) {
  check({}, undefined);
  check(own, 10);
  own.handlerKindField = undefined;
  check(own, undefined);
  delete own.handlerKindField;
  check(own, undefined);
  own.handlerKindField = 10;
  check(inherited, 20);
  check(Object.create({ handlerKindField: 21 }), 21);
  check(nullProto, undefined);
  nullProto.handlerKindField = 30;
  check(nullProto, 30);
  delete nullProto.handlerKindField;
  check(nullProto, undefined);
}

// Primitive hit/miss payloads must not be interpreted as object handlers.
for (const [value, proto] of [
  [4, Number.prototype], ['text', String.prototype], [true, Boolean.prototype],
]) {
  const old = Object.getOwnPropertyDescriptor(proto, 'handlerKindField');
  try {
    check(value, undefined);
    Object.defineProperty(proto, 'handlerKindField', { value: 40, configurable: true });
    check(value, 40);
    check({}, undefined);
    check(proto, 40);
    check(value, 40);
    delete proto.handlerKindField;
    check(value, undefined);
    check(own, 10);
  } finally {
    if (old) Object.defineProperty(proto, 'handlerKindField', old);
    else delete proto.handlerKindField;
  }
}

function callable() {}
check(callable, undefined);
callable.handlerKindField = 50;
check(callable, 50);
delete callable.handlerKindField;
check(callable, undefined);

// Changing a guarded prototype to an accessor or proxy must execute effects
// exactly once and keep the original receiver.
delete parent.handlerKindField;
check(inherited, undefined);
let effects = 0;
Object.defineProperty(parent, 'handlerKindField', {
  configurable: true,
  get() { effects++; assert.strictEqual(this, inherited); return 60; },
});
for (const read of readers) assert.strictEqual(read(inherited), 60);
assert.strictEqual(effects, readers.length);
delete parent.handlerKindField;
check(inherited, undefined);
Object.setPrototypeOf(parent, new Proxy({}, {
  get(target, key, receiver) {
    assert.strictEqual(key, 'handlerKindField');
    assert.strictEqual(receiver, inherited);
    effects++;
    return 61;
  },
}));
for (const read of readers) assert.strictEqual(read(inherited), 61);
assert.strictEqual(effects, readers.length * 2);
assert.strictEqual(optionalDriver(null, undefined), 1000);
assert.strictEqual(optionalDriver(undefined, undefined), 1000);
console.log('PASS field handler kinds');
