function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const nativeMethodLengths = [
  [Map.prototype.set, 2, 'Map.prototype.set'],
  [Map.prototype.get, 1, 'Map.prototype.get'],
  [Map.prototype.has, 1, 'Map.prototype.has'],
  [Map.prototype.delete, 1, 'Map.prototype.delete'],
  [Map.prototype.clear, 0, 'Map.prototype.clear'],
  [Map.prototype.entries, 0, 'Map.prototype.entries'],
  [Map.prototype.keys, 0, 'Map.prototype.keys'],
  [Map.prototype.values, 0, 'Map.prototype.values'],
  [Map.prototype.forEach, 1, 'Map.prototype.forEach'],
  [Set.prototype.add, 1, 'Set.prototype.add'],
  [Set.prototype.has, 1, 'Set.prototype.has'],
  [Set.prototype.delete, 1, 'Set.prototype.delete'],
  [Set.prototype.clear, 0, 'Set.prototype.clear'],
  [Set.prototype.values, 0, 'Set.prototype.values'],
  [Set.prototype.entries, 0, 'Set.prototype.entries'],
  [Set.prototype.forEach, 1, 'Set.prototype.forEach'],
  [WeakMap.prototype.set, 2, 'WeakMap.prototype.set'],
  [WeakMap.prototype.get, 1, 'WeakMap.prototype.get'],
  [WeakMap.prototype.has, 1, 'WeakMap.prototype.has'],
  [WeakMap.prototype.delete, 1, 'WeakMap.prototype.delete'],
  [WeakSet.prototype.add, 1, 'WeakSet.prototype.add'],
  [WeakSet.prototype.has, 1, 'WeakSet.prototype.has'],
  [WeakSet.prototype.delete, 1, 'WeakSet.prototype.delete'],
];

for (const [method, expected, label] of nativeMethodLengths) {
  assert(method.length === expected, `${label}.length should be ${expected}, got ${method.length}`);
}

const uncurryThis = Function.prototype.bind.bind(Function.prototype.call);
const functionCall = uncurryThis(Function.prototype.call);
const mapProbe = new Map();

for (const key of Reflect.ownKeys(Map.prototype)) {
  if (key === 'constructor') continue;
  const desc = Reflect.getOwnPropertyDescriptor(Map.prototype, key);
  if (
    typeof desc.value === 'function' &&
    desc.value.length === 0 &&
    Symbol.iterator in (functionCall(desc.value, mapProbe) ?? {})
  ) {
    const factory = uncurryThis(desc.value);
    const next = uncurryThis(factory(mapProbe).next);
    assert(typeof next === 'function', 'uncurried safe Map iterator next should be callable');
  }
}

const mapSet = uncurryThis(Map.prototype.set);
const map = new Map();
mapSet(map, 'a', 1);
assert(map.get('a') === 1, 'uncurried Map.prototype.set should work');

console.log('collection:method-lengths:ok');
