const assert = require('node:assert');

const tag = o => {
  const d = Object.getOwnPropertyDescriptor(o, Symbol.toStringTag);
  return d && [d.value, d.writable, d.enumerable, d.configurable];
};

const GeneratorFunction = Object.getPrototypeOf(function* () {});
const AsyncGeneratorFunction = Object.getPrototypeOf(async function* () {});
const GeneratorPrototype = GeneratorFunction.prototype;
const AsyncGeneratorPrototype = AsyncGeneratorFunction.prototype;

assert.deepStrictEqual([tag(GeneratorPrototype), tag(AsyncGeneratorPrototype)], [['Generator', false, false, true], ['AsyncGenerator', false, false, true]], '%GeneratorPrototype% and %AsyncGeneratorPrototype% own their tags');
assert.deepStrictEqual([tag(GeneratorFunction), tag(AsyncGeneratorFunction)], [['GeneratorFunction', false, false, true], ['AsyncGeneratorFunction', false, false, true]]);
assert.strictEqual(Object.getPrototypeOf((function* () {})()).__proto__, GeneratorPrototype);
assert.strictEqual(Object.getPrototypeOf((async function* () {})()).__proto__, AsyncGeneratorPrototype);
assert.deepStrictEqual([Object.prototype.toString.call((function* () {})()), Object.prototype.toString.call((async function* () {})())], ['[object Generator]', '[object AsyncGenerator]']);

for (const [proto, ctor] of [[GeneratorPrototype, GeneratorFunction], [AsyncGeneratorPrototype, AsyncGeneratorFunction]]) {
  const d = Object.getOwnPropertyDescriptor(proto, 'constructor');
  assert.deepStrictEqual([d.value, d.writable, d.enumerable, d.configurable], [ctor, false, false, true], 'the prototype links back to its function prototype');
  assert.deepStrictEqual(Object.keys(proto), [], 'next, return and throw are not enumerable');
  assert.deepStrictEqual(['next', 'return', 'throw'].map(k => Object.getOwnPropertyDescriptor(proto, k).enumerable), [false, false, false]);
}

const forInGen = [];
for (const k in (function* () {})()) forInGen.push(k);
assert.deepStrictEqual(forInGen, [], 'a generator object has nothing to enumerate');

const collections = [Map, Set, WeakMap, WeakSet, WeakRef, FinalizationRegistry];
assert.deepStrictEqual(collections.map(C => tag(C.prototype)), collections.map(C => [C.name, false, false, true]), 'collection tags are non-writable and non-enumerable');
assert.deepStrictEqual(
  [new Map().entries(), new Set().values()].map(it => tag(Object.getPrototypeOf(it))),
  [['Map Iterator', false, false, true], ['Set Iterator', false, false, true]],
  'collection iterator tags match'
);

const AsyncFunctionPrototype = Object.getPrototypeOf(async function () {});
assert.deepStrictEqual([typeof GeneratorFunction, typeof AsyncGeneratorFunction, typeof AsyncFunctionPrototype], ['object', 'object', 'object'], 'function kind prototypes are ordinary objects');
assert.deepStrictEqual([Object.getPrototypeOf(GeneratorFunction), Object.getPrototypeOf(AsyncFunctionPrototype)], [Function.prototype, Function.prototype]);
const keys = o => Reflect.ownKeys(o).map(String);
assert.deepStrictEqual([keys(GeneratorFunction), keys(AsyncGeneratorFunction), keys(AsyncFunctionPrototype)], [
  ['prototype', 'constructor', 'Symbol(Symbol.toStringTag)'],
  ['prototype', 'constructor', 'Symbol(Symbol.toStringTag)'],
  ['constructor', 'Symbol(Symbol.toStringTag)'],
]);
assert.deepStrictEqual([keys(GeneratorPrototype), keys(AsyncGeneratorPrototype)], [
  ['constructor', 'next', 'return', 'throw', 'Symbol(Symbol.toStringTag)'],
  ['constructor', 'next', 'return', 'throw', 'Symbol(Symbol.toStringTag)'],
], 'generator prototypes list keys in spec order');

const attrs = (o, k) => { const d = Object.getOwnPropertyDescriptor(o, k); return [d.writable, d.enumerable, d.configurable]; };
for (const kind of [GeneratorFunction, AsyncGeneratorFunction, AsyncFunctionPrototype]) {
  const C = kind.constructor;
  assert.deepStrictEqual([keys(C), attrs(C, 'length'), attrs(C, 'name'), attrs(C, 'prototype'), attrs(kind, 'constructor'), C.prototype], [['length', 'name', 'prototype'], [false, false, true], [false, false, true], [false, false, false], [false, false, true], kind], C.name);
}

const ts = v => Object.prototype.toString.call(v);
assert.deepStrictEqual(
  [new String('a'), new Number(1), new Boolean(false), Object(1n), new Date(0), new Proxy([], {}), new Proxy(() => 1, {}), new Int8Array(1), Buffer.alloc(1), Uint8Array.prototype, [].values(), { get [Symbol.toStringTag]() { return 'G'; } }].map(ts),
  ['[object String]', '[object Number]', '[object Boolean]', '[object BigInt]', '[object Date]', '[object Array]', '[object Function]', '[object Int8Array]', '[object Uint8Array]', '[object Object]', '[object Array Iterator]', '[object G]'],
  'builtin tags and accessor tags'
);
assert.throws(() => ts({ get [Symbol.toStringTag]() { throw new RangeError('t'); } }), RangeError);
const revocable = Proxy.revocable([], {}); revocable.revoke();
assert.throws(() => ts(revocable.proxy), TypeError);
assert.deepStrictEqual([tag(BigInt.prototype), Object.hasOwn(Uint8Array.prototype, Symbol.toStringTag), Object.hasOwn(Buffer.prototype, Symbol.toStringTag)], [['BigInt', false, false, true], false, false]);

const AsyncIteratorPrototype = Object.getPrototypeOf(AsyncGeneratorPrototype);
const asyncDispose = AsyncIteratorPrototype[Symbol.asyncDispose];
assert.deepStrictEqual([typeof asyncDispose, asyncDispose.name, asyncDispose.length, Object.hasOwn(AsyncGeneratorPrototype, Symbol.asyncDispose)], ['function', '[Symbol.asyncDispose]', 0, false], 'asyncDispose lives on %AsyncIteratorPrototype%');

(async () => {
  let closed = false;
  const it = (async function* () { try { yield 1; } finally { closed = true; } })();
  await it.next();
  assert.deepStrictEqual([await it[Symbol.asyncDispose](), closed], [undefined, true]);
  assert.deepStrictEqual([await asyncDispose.call({}), await asyncDispose.call({ return: () => Promise.resolve(5) })], [undefined, undefined], 'any object works and the result is dropped');
  await assert.rejects(asyncDispose.call({ return: 1 }), TypeError);
  await assert.rejects(asyncDispose.call({ return() { throw new RangeError('x'); } }), RangeError);
  console.log('intrinsic toStringTag OK');
})();
