const assert = require('node:assert');

const N = 8000;
const ArrayIteratorPrototype = Object.getPrototypeOf([][Symbol.iterator]());
const StringIteratorPrototype = Object.getPrototypeOf(''[Symbol.iterator]());
const MapIteratorPrototype = Object.getPrototypeOf(new Map()[Symbol.iterator]());
const SetIteratorPrototype = Object.getPrototypeOf(new Set()[Symbol.iterator]());
const IteratorPrototype = Object.getPrototypeOf(ArrayIteratorPrototype);

function setReturn(object, value) { object.return = value; }
function setNext(object, value) { object.next = value; }
function setEval(object, value) { object.eval = value; }
function setThen(object, value) { object.then = value; }
function setConstructor(object, value) { object.constructor = value; }

function warm(set, key) {
  const filler = () => {};
  for (let i = 0; i < N; i++) {
    const fresh = i & 1 ? {} : { [key]: 0 };
    set(fresh, filler);
    const inherited = Object.create(IteratorPrototype);
    set(inherited, filler);
    assert.strictEqual(fresh[key], filler);
    assert.strictEqual(inherited[key], filler);
  }
}

function breakEarly(iterable) {
  let seen = 0;
  for (const item of iterable) { seen++; break; }
  return seen;
}
function drain(iterable) {
  const out = [];
  for (const item of iterable) out.push(item);
  return out;
}

for (let i = 0; i < N; i++) {
  assert.strictEqual(breakEarly([1, 2, 3]), 1);
  assert.strictEqual(breakEarly('abc'), 1);
  assert.strictEqual(breakEarly(new Set([1, 2])), 1);
  assert.strictEqual(breakEarly(new Map([[1, 2]])), 1);
}
assert.deepStrictEqual(drain([1, 2, 3]), [1, 2, 3]);

warm(setReturn, 'return');
const closed = [];
for (const [name, holder, iterable] of [
  ['array', ArrayIteratorPrototype, [1, 2, 3]],
  ['string', StringIteratorPrototype, 'abc'],
  ['map', MapIteratorPrototype, new Map([[1, 2]])],
  ['set', SetIteratorPrototype, new Set([1, 2])],
]) {
  setReturn(holder, function () { closed.push(name); return {}; });
  assert.strictEqual(breakEarly(iterable), 1, name);
  for (let i = 0; i < N; i++) setReturn(holder, i & 1 ? function () { closed.push(`${name} odd`); return {}; } : function () { closed.push(`${name} even`); return {}; });
  assert.strictEqual(breakEarly(iterable), 1, name);
  delete holder.return;
  assert.strictEqual(breakEarly(iterable), 1, name);
}
assert.deepStrictEqual(closed, ['array', 'array odd', 'string', 'string odd', 'map', 'map odd', 'set', 'set odd']);

setReturn(IteratorPrototype, function () { closed.push('shared'); return {}; });
assert.strictEqual(breakEarly([1, 2]), 1);
assert.strictEqual(breakEarly(new Set([1])), 1);
delete IteratorPrototype.return;
setReturn(Object.prototype, function () { closed.push('object'); return {}; });
assert.strictEqual(breakEarly([1, 2]), 1);
assert.strictEqual(breakEarly({ [Symbol.iterator]() { return { next() { return { done: false, value: 1 }; } }; } }), 1);
delete Object.prototype.return;
assert.deepStrictEqual(closed.slice(8), ['shared', 'shared', 'object', 'object']);
assert.strictEqual(breakEarly([1, 2]), 1);
assert.strictEqual(closed.length, 12);

warm(setNext, 'next');
for (const [name, holder, make, expected] of [
  ['array', ArrayIteratorPrototype, () => [1, 2, 3], [1, 2, 3]],
  ['string', StringIteratorPrototype, () => 'abc', ['a', 'b', 'c']],
  ['map', MapIteratorPrototype, () => new Map([[1, 2]]), [[1, 2]]],
  ['set', SetIteratorPrototype, () => new Set([1, 2]), [1, 2]],
]) {
  const original = holder.next;
  let calls = 0;
  setNext(holder, function () { calls++; return original.call(this); });
  assert.deepStrictEqual(drain(make()), expected, name);
  assert.strictEqual(calls, expected.length + 1, name);
  assert.deepStrictEqual([...make()], expected, name);
  assert.strictEqual(calls, 2 * expected.length + 2, name);
  for (let i = 0; i < N; i++) setNext(holder, i & 1 ? function () { return { done: true, value: undefined }; } : original);
  assert.deepStrictEqual(drain(make()), [], name);
  assert.deepStrictEqual([...make()], [], name);
  const [first] = make();
  assert.strictEqual(first, undefined, name);
  setNext(holder, original);
  assert.deepStrictEqual(drain(make()), expected, name);
}

warm(setEval, 'eval');
const realEval = globalThis.eval;
function callEval(source) { return eval(source); }
for (let i = 0; i < N; i++) assert.strictEqual(callEval('1 + 1'), 2);
setEval(globalThis, (source) => `fake:${source}`);
assert.strictEqual(callEval('1 + 1'), 'fake:1 + 1');
assert.strictEqual(eval('2 + 2'), 'fake:2 + 2');
for (let i = 0; i < N; i++) setEval(globalThis, i & 1 ? (source) => `odd:${source}` : (source) => `even:${source}`);
assert.strictEqual(callEval('3'), 'odd:3');
setEval(globalThis, realEval);
assert.strictEqual(callEval('1 + 1'), 2);
let local = 5;
assert.strictEqual(eval('local + 1'), 6);

warm(setThen, 'then');
warm(setConstructor, 'constructor');
(async () => {
  const promise = Promise.resolve(1);
  let thenCalls = 0;
  setThen(promise, function (resolve) { thenCalls++; resolve('own'); });
  assert.strictEqual(await Promise.resolve().then(() => promise), 'own');
  assert.strictEqual(thenCalls, 1);

  class Derived extends Array {}
  const array = [1, 2, 3];
  setConstructor(array, Derived);
  assert.strictEqual(array.map((x) => x) instanceof Derived, true);
  assert.strictEqual([1].map((x) => x) instanceof Derived, false);

  setThen(Object.prototype, function (resolve) { resolve('inherited'); });
  const viaObject = await { plain: true };
  delete Object.prototype.then;
  assert.strictEqual(viaObject, 'inherited');
  assert.deepStrictEqual(await { plain: true }, { plain: true });
  console.log('ok');
})();
