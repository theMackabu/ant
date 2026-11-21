const assert = require('node:assert');

function Frozen() { return Object.freeze([]); }
function ReadonlyLength() {
  const a = [];
  Object.defineProperty(a, 'length', { writable: false });
  return a;
}
function NonExtensibleObject() { return Object.preventExtensions({}); }

let traps = [];
function Trapped() {
  return new Proxy({}, {
    defineProperty(target, key, desc) {
      traps.push(key);
      return Reflect.defineProperty(target, key, desc);
    },
  });
}

let ctorArgs;
function Recorder(...args) { ctorArgs = args; }

const setterHits = [];
function WithSetter() {
  const o = {};
  Object.defineProperty(o, '0', { set(v) { setterHits.push(v); }, configurable: true });
  return o;
}

function closingIterable() {
  const state = { closed: false, n: 0 };
  state.iterable = {
    [Symbol.iterator]() {
      return {
        next: () => (state.n++ < 3 ? { value: state.n, done: false } : { done: true }),
        return: () => { state.closed = true; return {}; },
      };
    },
  };
  return state;
}

class ReadonlySpecies extends Array {
  static get [Symbol.species]() { return ReadonlyLength; }
}

for (let i = 0; i < 2000; i++) {
  assert.throws(() => Array.of.call(Frozen, 1), TypeError);
  assert.throws(() => Array.of.call(ReadonlyLength, 1), TypeError);
  assert.throws(() => Array.from.call(Frozen, [1]), TypeError);
  assert.throws(() => Array.from.call(NonExtensibleObject, { length: 1, 0: 'a' }), TypeError);

  traps = [];
  Array.from.call(Trapped, [1, 2]);
  assert.deepStrictEqual(traps, ['0', '1', 'length']);
  traps = [];
  Array.of.call(Trapped, 'a');
  assert.deepStrictEqual(traps, ['0', 'length']);

  const state = closingIterable();
  assert.throws(() => Array.from.call(Frozen, state.iterable), TypeError);
  assert.strictEqual(state.closed, true);

  Array.from.call(Recorder, [1, 2]);
  assert.deepStrictEqual(ctorArgs, []);
  Array.from.call(Recorder, { length: 2, 0: 'a', 1: 'b' });
  assert.deepStrictEqual(ctorArgs, [2]);
  Array.of.call(Recorder, 'x', 'y', 'z');
  assert.deepStrictEqual(ctorArgs, [3]);

  setterHits.length = 0;
  const viaSetter = Array.of.call(WithSetter, 'v');
  assert.deepStrictEqual(setterHits, []);
  assert.strictEqual(Object.getOwnPropertyDescriptor(viaSetter, '0').value, 'v');

  assert.strictEqual(Array.from.call(Recorder, 5).length, 0);
  assert.strictEqual(Array.from.call(Recorder, true).length, 0);
  assert.deepStrictEqual(Array.from(5), []);

  assert.throws(() => new ReadonlySpecies(1, 2, 3).splice(0, 0), TypeError);
  assert.throws(() => new ReadonlySpecies(1, 2, 3).splice(0, 1), TypeError);

  assert.throws(() => Array.from([1], 5), TypeError);
  assert.throws(() => Array.from(), TypeError);
  assert.throws(() => Array.from(null), TypeError);

  assert.deepStrictEqual(Array.from([1, 2], (v) => v * 2), [2, 4]);
  assert.deepStrictEqual(Array.from({ length: 2, 0: 'a' }), ['a', undefined]);
  assert.deepStrictEqual(Array.of(1, 2), [1, 2]);
  const arrow = () => {};
  assert.deepStrictEqual(Array.from.call(arrow, 'ab'), ['a', 'b']);
}

class Sub extends Array {}
const sub = Sub.from([1, 2, 3]);
assert.ok(sub instanceof Sub);
assert.deepStrictEqual([...sub], [1, 2, 3]);
assert.strictEqual(Sub.of(4, 5).length, 2);

console.log('ok');
