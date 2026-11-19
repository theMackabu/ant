const assert = require('node:assert');

const sym = Symbol('static');
const desc = (o, k) => Object.getOwnPropertyDescriptor(o, k);
const data = (value) => ({ value, writable: true, enumerable: true, configurable: true });

for (let round = 0; round < 300; round++) {
  class Plain { static a = round; static b = Plain.a + 1; static [sym] = 's'; static 7 = 'seven'; static ['c' + 'd'] = 'computed'; static fn = function () {}; static arrow = () => Plain; static builtin = Math.max; }
  assert.deepStrictEqual(desc(Plain, 'a'), data(round));
  assert.deepStrictEqual(desc(Plain, 'b'), data(round + 1));
  assert.deepStrictEqual(desc(Plain, sym), data('s'));
  assert.deepStrictEqual(desc(Plain, '7'), data('seven'));
  assert.deepStrictEqual(desc(Plain, 'cd'), data('computed'));
  assert.strictEqual(Plain.fn.name, 'fn');
  assert.strictEqual(Plain.arrow(), Plain);
  assert.strictEqual(Plain.builtin, Math.max);
  assert.strictEqual(Plain.builtin.name, 'max');
  assert.deepStrictEqual(Reflect.ownKeys(Plain), ['7', 'length', 'name', 'prototype', 'a', 'b', 'cd', 'fn', 'arrow', 'builtin', sym]);

  class Named { static name = 'renamed'; static length = 9; static caller = 1; static arguments = 2; }
  assert.deepStrictEqual(desc(Named, 'name'), data('renamed'));
  assert.deepStrictEqual(desc(Named, 'length'), data(9));
  assert.deepStrictEqual(desc(Named, 'caller'), data(1));
  assert.deepStrictEqual(desc(Named, 'arguments'), data(2));
  assert.throws(() => class { static ['proto' + 'type'] = 1; }, TypeError);
  const Anonymous = class { static name = 'late'; static length = round; };
  assert.deepStrictEqual(desc(Anonymous, 'name'), data('late'));
  assert.deepStrictEqual(desc(Anonymous, 'length'), data(round));
  const Inferred = class { static other = 1; };
  assert.deepStrictEqual(desc(Inferred, 'name'), { value: 'Inferred', writable: false, enumerable: false, configurable: true });
  assert.deepStrictEqual(desc(class { constructor(a, b) {} static x = 1; }, 'length'), { value: 2, writable: false, enumerable: false, configurable: true });
  assert.deepStrictEqual(Reflect.ownKeys(class { static name = 1; static z = 2; }), ['length', 'name', 'prototype', 'z']);

  class Method { static m() { return 1; } static m = 2; static get g() { return 1; } static g = 3; }
  assert.deepStrictEqual(desc(Method, 'm'), data(2));
  assert.deepStrictEqual(desc(Method, 'g'), data(3));

  class Parent { static inherited = 1; static set trap(v) { throw new Error('setter must not run'); } }
  class Child extends Parent { static inherited = 2; static trap = 3; }
  assert.deepStrictEqual(desc(Child, 'inherited'), data(2));
  assert.deepStrictEqual(desc(Child, 'trap'), data(3));
  assert.strictEqual(Parent.inherited, 1);

  assert.throws(() => class { static a = Object.freeze(this); static b = 1; }, TypeError);
  assert.throws(() => class { static a = Object.preventExtensions(this); static b = 1; }, TypeError);
  assert.throws(() => class { static a = 1; static x = Object.defineProperty(this, 'a', { configurable: false }); static a = 2; }, TypeError);
  class Sealed { static a = 1; static b = 2; static { Object.seal(this); this.a = 3; } }
  assert.deepStrictEqual(desc(Sealed, 'a'), { value: 3, writable: true, enumerable: true, configurable: false });
  assert.throws(() => class { static a = Object.defineProperty(this, 'b', { value: 1 }); static b = 2; }, TypeError);
  class Redefined { static a = Object.defineProperty(this, 'b', { value: 1, configurable: true }); static b = 2; }
  assert.deepStrictEqual(desc(Redefined, 'b'), data(2));
}

console.log('ok');
