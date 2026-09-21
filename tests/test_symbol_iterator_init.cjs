function assert(value, message) {
  if (!value) throw new Error(message);
}

function descriptor(object, key, writable, enumerable, configurable) {
  const d = Object.getOwnPropertyDescriptor(object, key);
  assert(d && d.writable === writable && d.enumerable === enumerable &&
    d.configurable === configurable, `wrong descriptor for ${String(key)}`);
}

for (const name of Object.getOwnPropertyNames(Symbol)) {
  if (typeof Symbol[name] === 'symbol') descriptor(Symbol, name, false, false, false);
}
descriptor(Symbol, 'prototype', false, false, false);
for (const name of ['for', 'keyFor']) descriptor(Symbol, name, true, false, true);
for (const name of ['constructor', 'toString', 'valueOf']) {
  descriptor(Symbol.prototype, name, true, false, true);
}
assert(Symbol.prototype.constructor === Symbol, 'Symbol constructor backlink');
descriptor(Symbol.prototype, Symbol.toPrimitive, false, false, true);
descriptor(Symbol.prototype, Symbol.toStringTag, false, false, true);
descriptor(Array.prototype, Symbol.iterator, true, false, true);
descriptor(String.prototype, Symbol.iterator, true, false, true);
descriptor(Array.prototype, Symbol.unscopables, false, false, true);
descriptor(Promise.prototype, Symbol.toStringTag, false, false, true);
assert(Array.prototype[Symbol.iterator] === Array.prototype.values, 'array values identity');

for (const fn of [async function () {}, function* () {}, async function* () {}]) {
  descriptor(Object.getPrototypeOf(fn), Symbol.toStringTag, false, false, true);
}
for (const iterator of [[].values(), ''[Symbol.iterator]()]) {
  const proto = Object.getPrototypeOf(iterator);
  descriptor(proto, 'next', true, false, true);
  descriptor(proto, Symbol.toStringTag, false, false, true);
}

const unscopables = Array.prototype[Symbol.unscopables];
assert(Object.getPrototypeOf(unscopables) === null, 'unscopables prototype');
const names = ['at', 'copyWithin', 'entries', 'fill', 'find', 'findIndex', 'findLast',
  'findLastIndex', 'flat', 'flatMap', 'includes', 'keys', 'toReversed', 'toSorted',
  'toSpliced', 'values'];
assert(Object.keys(unscopables).length === names.length, 'unscopables key count');
for (const name of names) {
  assert(unscopables[name] === true, `unscopables ${name}`);
  descriptor(unscopables, name, true, true, true);
  const lookup = Function('array', `const ${name} = 'outer'; with (array) { return ${name}; }`);
  assert(lookup([]) === 'outer', `with lookup for ${name}`);
}

const factories = [
  () => [1, 2].values(),
  () => 'ab'[Symbol.iterator](),
  () => new Map([[1, 2]]).entries(),
  () => new Set([1, 2]).values(),
];
if (typeof Uint8Array === 'function') factories.push(() => new Uint8Array([1, 2]).values());
if (typeof Headers === 'function') factories.push(() => new Headers({ a: 'b' }).entries());

for (const factory of factories) {
  let calls = 0;
  const iterator = factory();
  iterator.next = () => { calls++; return { done: true }; };
  assert(new Set(iterator).size === 0 && calls === 1, 'own next override');

  const proto = Object.getPrototypeOf(factory());
  const original = Object.getOwnPropertyDescriptor(proto, 'next');
  try {
    Object.defineProperty(proto, 'next', {
      ...original, value() { return { done: true }; },
    });
    assert(new Set(factory()).size === 0, 'prototype next override');
  } finally {
    Object.defineProperty(proto, 'next', original);
  }

  const getterIterator = factory();
  let reads = 0;
  Object.defineProperty(getterIterator, 'next', {
    get() { reads++; return () => ({ done: true }); },
  });
  assert(new Set(getterIterator).size === 0 && reads === 1, 'next getter');

  for (const next of [123, () => 123]) {
    const invalid = factory();
    invalid.next = next;
    let caught;
    try { new Set(invalid); } catch (e) { caught = e; }
    assert(caught instanceof TypeError, 'invalid overridden next');
  }
}

const originalAdd = Set.prototype.add;
const functionReturn = [1].values();
functionReturn.return = () => Array.prototype.push;
assert(functionReturn.some(() => true), 'native function is a valid iterator return result');

class Tagged extends Iterator {
  constructor() { super(); this[Symbol.toStringTag] = 'Tagged'; }
  next() { return { done: true }; }
}
assert(Object.prototype.toString.call(new Tagged()) === '[object Tagged]', 'Iterator subclass tag');
const tagDescriptor = Object.getOwnPropertyDescriptor(Iterator.prototype, Symbol.toStringTag);
assert(typeof tagDescriptor.get === 'function' && typeof tagDescriptor.set === 'function' &&
  !tagDescriptor.enumerable && tagDescriptor.configurable, 'Iterator tag accessor');

try {
  const marker = {};
  const closing = [1].values();
  let returns = 0;
  Set.prototype.add = function () {
    closing.return = () => { returns++; throw new Error('cleanup'); };
    throw marker;
  };
  let caught;
  try { new Set(closing); } catch (e) { caught = e; }
  assert(caught === marker && returns === 1, 'native iterator close preserves original throw');

  const cached = [1, 2].values();
  Set.prototype.add = function (value) {
    cached.next = () => ({ done: true });
    return originalAdd.call(this, value);
  };
  assert(new Set(cached).size === 2, 'next method remains captured during iteration');
} finally {
  Set.prototype.add = originalAdd;
}

console.log('PASS Symbol descriptors, intrinsic wiring, unscopables, and iterator overrides');
