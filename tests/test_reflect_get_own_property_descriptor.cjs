function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const sym = Symbol('own');
const object = { value: 1, [sym]: 2 };

const symbolDesc = Reflect.getOwnPropertyDescriptor(object, sym);
assert(symbolDesc && symbolDesc.value === 2, 'Reflect.getOwnPropertyDescriptor should support symbol keys');

const arrayProtoDesc = Reflect.getOwnPropertyDescriptor(Array.prototype, 'push');
assert(arrayProtoDesc && typeof arrayProtoDesc.value === 'function', 'Reflect.getOwnPropertyDescriptor should support array prototype objects');
assert(typeof Array.prototype.push.apply === 'function', 'reflecting native array methods should preserve Function.prototype.apply fallback');

for (const key of Reflect.ownKeys(Atomics)) {
  assert(
    Reflect.getOwnPropertyDescriptor(Atomics, key) !== undefined,
    'Reflect.getOwnPropertyDescriptor should describe each Reflect.ownKeys result'
  );
}

console.log('reflect:get-own-property-descriptor:ok');
