const assert = require('node:assert');

const message = (run) => {
  try {
    run();
  } catch (error) {
    return `${error.constructor.name}: ${error.message}`;
  }
  return 'no throw';
};

function target() {}

for (let round = 0; round < 300; round++) {
  assert.match(message(() => target(...null)), /^TypeError: null is not iterable/);
  assert.match(message(() => target(...undefined)), /^TypeError: undefined is not iterable/);
  assert.strictEqual(message(() => { for (const x of undefined) void x; }), 'TypeError: undefined is not iterable');
  assert.strictEqual(message(() => { for (const x of null) void x; }), 'TypeError: null is not iterable');
  assert.strictEqual(message(() => { const [a] = null; return a; }), 'TypeError: null is not iterable');
  assert.strictEqual(message(() => [...undefined]), 'TypeError: undefined is not iterable');
  assert.match(message(() => { for (const x of 5) void x; }), /^TypeError: .+ is not iterable$/);
  assert.match(message(() => { for (const x of {}) void x; }), /^TypeError: .+ is not iterable$/);

  assert.strictEqual(
    message(() => { class A extends null { constructor() { super(); } } new A(); }),
    'TypeError: Super constructor null of A is not a constructor');
  assert.strictEqual(
    message(() => { new (class extends null { constructor() { super(); } })(); }),
    'TypeError: Super constructor null of anonymous class is not a constructor');
  assert.strictEqual(
    message(() => { class P {} class A extends P { constructor() { super(); } } Object.setPrototypeOf(A, null); new A(); }),
    'TypeError: Super constructor null of A is not a constructor');
  assert.match(
    message(() => { class P {} class A extends P {} Object.setPrototypeOf(A, () => {}); new A(); }),
    /^TypeError: Super constructor .+ of A is not a constructor$/);

  assert.strictEqual(message(() => Object.setPrototypeOf(Object.preventExtensions({}), {})), 'TypeError: #<Object> is not extensible');
  assert.strictEqual(
    message(() => Object.setPrototypeOf(new Proxy(Object.preventExtensions({}), {}), {})),
    'TypeError: #<Object> is not extensible');
  assert.match(
    message(() => Object.setPrototypeOf(new Proxy({}, { setPrototypeOf: () => false }), {})),
    /^TypeError: 'setPrototypeOf' on proxy: trap returned fals/);

  assert.strictEqual(
    message(() => { const o = { m() { super.x = 1; } }; Object.setPrototypeOf(o, null); o.m(); }),
    "TypeError: Cannot set properties of null (setting 'x')");
  assert.strictEqual(
    message(() => { const o = { m() { super['y'] = 1; } }; Object.setPrototypeOf(o, null); o.m(); }),
    "TypeError: Cannot set properties of null (setting 'y')");

  assert.strictEqual(
    message(() => { 'use strict'; Object.defineProperty(Object.preventExtensions({}), 'x', { value: 1 }); }),
    'TypeError: Cannot define property x, object is not extensible');
  assert.strictEqual(
    message(() => { 'use strict'; Object.preventExtensions({}).x = 1; }),
    'TypeError: Cannot add property x, object is not extensible');
  assert.strictEqual(
    message(() => { 'use strict'; undefined = 1; }),
    "TypeError: Cannot assign to read only property 'undefined' of object '#<Object>'");
  assert.strictEqual(message(() => Object.setPrototypeOf(null, {})), 'TypeError: Object.setPrototypeOf called on null or undefined');
}

{
  let suppressed;
  try {
    (function () {
      using resource = { [Symbol.dispose]() { throw new Error('dispose'); } };
      void resource;
      throw new Error('body');
    })();
  } catch (error) {
    suppressed = error;
  }
  assert.strictEqual(suppressed.message, 'An error was suppressed during disposal');
}

console.log('ok');
