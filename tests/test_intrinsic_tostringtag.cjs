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

console.log('intrinsic toStringTag OK');
