const assert = require('node:assert');

class SharedBase { x() { return 'x'; } }
class SharedPrivate extends SharedBase {
  #value = 1;
  #method() { return this.#value + SharedPrivate.offset; }
  get #accessor() { return this.#value * 2; }
  set #accessor(next) { this.#value = next; }
  #viaSuper() { return super.x(); }
  static offset = 10;
  static early = new SharedPrivate().run();
  static sameMethod(a, b) { return a.#method === b.#method; }
  static has(value) { return #method in value; }
  run() { this.#accessor = 5; return [this.#method(), this.#accessor, this.#viaSuper()].join(); }
}
class SharedPrivateChild extends SharedPrivate { #own() { return 'own'; } go() { return this.#own() + this.run(); } }
const makePrivateClass = () => class { #p() { return 1; } static same(a, b) { return a.#p === b.#p; } };
const FirstPrivate = makePrivateClass();
const SecondPrivate = makePrivateClass();
assert.strictEqual(SharedPrivate.early, '15,10,x');
for (let i = 0; i < 3000; i++) {
  const first = new SharedPrivate();
  const second = new SharedPrivateChild();
  assert.strictEqual(first.run(), '15,10,x');
  assert.strictEqual(second.go(), 'own15,10,x');
  assert.strictEqual(SharedPrivate.sameMethod(first, second), true);
  assert.strictEqual(SharedPrivate.has(first), true);
  assert.strictEqual(SharedPrivate.has({}), false);
  assert.strictEqual(FirstPrivate.same(new FirstPrivate(), new FirstPrivate()), true);
  assert.throws(() => FirstPrivate.same(new FirstPrivate(), new SecondPrivate()), TypeError);
}

const perEvaluation = [];
var evaluation = 0;
while (evaluation < 3) perEvaluation.push(class Each { #id = evaluation; #read() { return this.#id; } #self() { return Each; } read() { return this.#read(); } static self(o) { return o.#self(); } static index = evaluation }), evaluation++;
assert.deepStrictEqual(perEvaluation.map((Class) => new Class().read()), [3, 3, 3]);
assert.deepStrictEqual(perEvaluation.map((Class) => Class.self(new Class()).index), [0, 1, 2]);
assert.throws(() => perEvaluation[0].self(new perEvaluation[1]()), TypeError);

const computedKeys = [];
var keyIndex = 0;
while (keyIndex < 3) computedKeys.push(class { ['k' + keyIndex] = keyIndex }), keyIndex++;
assert.deepStrictEqual(computedKeys.map((Class) => Object.keys(new Class())[0]), ['k0', 'k1', 'k2']);

class MethodsBeforeFields {
  early = this.#late();
  present = #late in this;
  viaAccessor = this.#value;
  #late() { return 4; }
  get #value() { return 7; }
}
assert.deepStrictEqual({ ...new MethodsBeforeFields() }, { early: 4, present: true, viaAccessor: 7 });

class PrivateBase { x() { return 'x'; } get g() { return 'g'; } static sx() { return 'sx'; } }
class PrivateSuper extends PrivateBase {
  #method() { return super.x(); }
  get #getter() { return super.g; }
  static #staticMethod() { return super.sx(); }
  #arrow() { return (() => super.x())(); }
  read() { return [this.#method(), this.#getter, PrivateSuper.#staticMethod(), this.#arrow()]; }
}
class PrivateSuperInBase {
  #method() { return super.toString === Object.prototype.toString; }
  read() { return this.#method(); }
}
for (let i = 0; i < 3000; i++) {
  assert.deepStrictEqual(new PrivateSuper().read(), ['x', 'g', 'sx', 'x']);
  assert.strictEqual(new PrivateSuperInBase().read(), true);
}

console.log('ok');
