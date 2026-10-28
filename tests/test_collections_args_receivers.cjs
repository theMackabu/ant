const assert = require('node:assert');

const m = new Map();
assert.strictEqual(m.has(), false);
assert.strictEqual(m.get(), undefined);
assert.strictEqual(m.set(1), m);
assert.strictEqual(m.get(1), undefined);
assert.strictEqual(m.has(1), true);
assert.strictEqual(m.delete(), false);
assert.strictEqual(m.set(undefined, 'u').get(), 'u');
assert.strictEqual(m.has(), true);
assert.strictEqual(m.delete(), true);
assert.strictEqual(m.has(undefined), false);
m.set();
assert.strictEqual(m.has(undefined), true);
assert.strictEqual(m.get(undefined), undefined);

const s = new Set([undefined]);
assert.strictEqual(s.has(), true);
assert.strictEqual(s.add(), s);
assert.strictEqual(s.size, 1);
assert.strictEqual(s.delete(), true);
assert.strictEqual(s.size, 0);
assert.strictEqual(s.delete(), false);

const wm = new WeakMap();
assert.strictEqual(wm.has(), false);
assert.strictEqual(wm.get(), undefined);
assert.strictEqual(wm.delete(), false);
assert.throws(() => wm.set(), TypeError);
assert.throws(() => wm.set(1, 2), TypeError);
const key = {};
assert.strictEqual(wm.set(key), wm);
assert.strictEqual(wm.has(key), true);
assert.strictEqual(wm.get(key), undefined);

const ws = new WeakSet();
assert.strictEqual(ws.has(), false);
assert.strictEqual(ws.delete(), false);
assert.throws(() => ws.add(), TypeError);
assert.throws(() => ws.add(1), TypeError);

function forEachFn() {}
const instances = { Map: new Map(), Set: new Set(), WeakMap: new WeakMap(), WeakSet: new WeakSet() };
const methods = [
  [Map, ['get', 'set', 'has', 'delete', 'clear', 'forEach', 'keys', 'values', 'entries', Symbol.iterator]],
  [Set, ['add', 'has', 'delete', 'clear', 'forEach', 'values', 'entries', Symbol.iterator]],
  [WeakMap, ['get', 'set', 'has', 'delete']],
  [WeakSet, ['add', 'has', 'delete']],
];

for (const [ctor, names] of methods) {
  const others = Object.keys(instances).filter(k => k !== ctor.name).map(k => instances[k]);
  const receivers = [{}, [], Object.create(ctor.prototype), ctor.prototype, 1, 'x', undefined, null, ...others];
  for (const name of names) {
    const fn = ctor.prototype[name];
    for (const receiver of receivers)
      assert.throws(() => fn.call(receiver, {}, forEachFn), TypeError, `${ctor.name}.${String(name)}`);
  }
}

for (const [ctor, name] of [[Map, 'size'], [Set, 'size']]) {
  const getter = Object.getOwnPropertyDescriptor(ctor.prototype, name).get;
  for (const receiver of [{}, [], Object.create(ctor.prototype), ctor === Map ? new Set() : new Map()]) {
    assert.throws(() => getter.call(receiver), TypeError);
  }
}
assert.throws(() => Map.prototype.size, TypeError);
assert.throws(() => Set.prototype.size, TypeError);

class SubMap extends Map {}
const sub = new SubMap([[1, 'one']]);
assert.strictEqual(Map.prototype.get.call(sub, 1), 'one');
assert.strictEqual(sub.size, 1);

for (const name of ['union', 'intersection', 'difference', 'symmetricDifference', 'isSubsetOf', 'isSupersetOf', 'isDisjointFrom']) {
  if (typeof Set.prototype[name] !== 'function') continue;
  assert.throws(() => Set.prototype[name].call({}, new Set()), TypeError, name);
}

function getPair(map, a, b) { return map.get(`${a}:${b}`); }
function hasPair(map, a, b) { return map.has(`${a}:${b}`); }
function getOne(map, k) { return map.get(k); }
const pairs = new Map([['1:2', 'x']]);
for (let i = 0; i < 20000; i++) {
  assert.strictEqual(getPair(pairs, 1, 2), 'x');
  assert.strictEqual(hasPair(pairs, 1, 2), true);
  assert.strictEqual(getOne(pairs, '1:2'), 'x');
  assert.strictEqual(getOne(pairs), undefined);
}
const fake = { get: Map.prototype.get, has: Map.prototype.has };
assert.throws(() => getPair(fake, 1, 2), TypeError);
assert.throws(() => hasPair(fake, 1, 2), TypeError);
assert.throws(() => getOne(fake, '1:2'), TypeError);

const seen = [];
new Map([[1, 2]]).forEach(function (v, k, map) { seen.push(v, k, map.size, this.tag); }, { tag: 't' });
assert.deepStrictEqual(seen, [2, 1, 1, 't']);
const pushed = [];
new Set(['a']).forEach(pushed.push, pushed);
assert.strictEqual(pushed.length, 3);
assert.throws(() => new Map().forEach(), TypeError);
assert.throws(() => new Set().forEach(1), TypeError);

console.log('collections args and receivers ok');
