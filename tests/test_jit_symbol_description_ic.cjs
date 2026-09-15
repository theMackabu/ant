'use strict';
const assert = require('node:assert');
function named(value) { return value.description; }
function computed(value, key) { return value[key]; }
function check(value, expected) {
  const key = ['descr', 'iption'].join('');
  assert.strictEqual(named(value), expected);
  assert.strictEqual(computed(value, key), expected);
}
const symbol = Symbol('first');
const wrapper = Object(symbol);
for (let i = 0; i < 1000; i++) {
  check(symbol, 'first');
}
for (let i = 0; i < 1000; i++) {
  check(wrapper, 'first');
}
// Computed IC hits must check the whole key before reusing a field handler.
for (let i = 0; i < 1000; i++) {
  assert.strictEqual(computed(wrapper, 'description'), 'first');
  assert.strictEqual(computed(wrapper, 'description\0'), undefined);
  assert.strictEqual(computed(wrapper, 'other'), undefined);
}
let conversions = 0;
const convertedKey = { [Symbol.toPrimitive]() { conversions++; return 'description'; } };
assert.strictEqual(computed(wrapper, convertedKey), 'first');
assert.strictEqual(conversions, 1);
const symbolKey = Symbol('description');
Object.defineProperty(wrapper, symbolKey, { value: 'symbol key' });
assert.strictEqual(computed(wrapper, symbolKey), 'symbol key');
function optionalComputed(value, key) { return value?.[key]; }
for (let i = 0; i < 1000; i++) {
  assert.strictEqual(optionalComputed(wrapper, 'description'), 'first');
  assert.strictEqual(optionalComputed(wrapper, 'description\0'), undefined);
}
assert.strictEqual(optionalComputed(null, 'description'), undefined);
assert.strictEqual(optionalComputed(undefined, 'description'), undefined);

// A cache stores the accessor, never the previous receiver's result.
for (const text of [undefined, '', 'second', '\u03bb']) {
  check(Symbol(text), text);
  check(Object(Symbol(text)), text);
}
Object.defineProperty(wrapper, 'description', { value: undefined, configurable: true });
check(wrapper, undefined);
delete wrapper.description;
check(wrapper, 'first');
Object.setPrototypeOf(wrapper, null);
check(wrapper, undefined);
Object.setPrototypeOf(wrapper, { description: 'custom chain' });
check(wrapper, 'custom chain');
Object.setPrototypeOf(wrapper, Symbol.prototype);
check(wrapper, 'first');

const original = Object.getOwnPropertyDescriptor(Symbol.prototype, 'description');
try {
  let reads = 0;
  Object.defineProperty(Symbol.prototype, 'description', {
    configurable: true,
    get() { reads++; return this === symbol ? 'primitive replacement' : 'boxed replacement'; },
  });
  check(symbol, 'primitive replacement');
  check(wrapper, 'boxed replacement');
  assert.strictEqual(reads, 4);
  delete Symbol.prototype.description;
  check(symbol, undefined);
  check(wrapper, undefined);
} finally {
  Object.defineProperty(Symbol.prototype, 'description', original);
}
check(symbol, 'first');
check(wrapper, 'first');

// Resolved native getters may be installed under other names too.
const alias = Object(symbol);
Object.defineProperty(alias, 'alias', { get: original.get });
for (let i = 0; i < 1000; i++) assert.strictEqual(computed(alias, 'alias'), 'first');
const invalidReceiver = {};
Object.defineProperty(invalidReceiver, 'alias', { get: original.get });
assert.throws(() => computed(invalidReceiver, 'alias'), TypeError);
let traps = 0;
const proxy = new Proxy(wrapper, { get(target, key) { traps++; return 'trapped'; } });
check(proxy, 'trapped');
assert.strictEqual(traps, 2);
const failure = new Error('replacement failure');
Object.defineProperty(wrapper, 'description', { get() { throw failure; } });
assert.throws(() => named(wrapper), err => err === failure);
assert.throws(() => computed(wrapper, 'description'), err => err === failure);
console.log('PASS Symbol description accessor IC');
