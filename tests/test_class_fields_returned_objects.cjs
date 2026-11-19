const assert = require('node:assert');

const returning = (make) => class { constructor() { return make(); } };

class OnArray extends returning(() => []) { 0 = 'z'; 1 = 'o'; named = 'n'; }
class OnArrayExplicit extends returning(() => [5]) { 0 = 'z'; 2 = 'w'; constructor() { super(); } }
class OnFrozen extends returning(() => Object.freeze({})) { 0 = 'z'; }
class OnSealed extends returning(() => Object.seal({ a: 1 })) { a = 2; }
class OnPlain extends returning(() => ({ kept: true })) { 0 = 'z'; ['1'] = 'o'; x = 1; }

let defined;
class OnProxy extends returning(() => new Proxy({}, {
  defineProperty(target, key, desc) { defined.push(key); return Reflect.defineProperty(target, key, desc); },
})) { 0 = 'z'; y = 1; }

let setterHits;
class OnAccessor extends returning(() => {
  const o = {};
  Object.defineProperty(o, '0', { set(v) { setterHits++; }, configurable: true });
  Object.defineProperty(o, 'named', { set(v) { setterHits++; }, configurable: true });
  return o;
}) { 0 = 'z'; named = 'n'; }

class OnLocked extends returning(() => Object.defineProperty({}, 'locked', { value: 1, configurable: false })) { locked = 2; }

for (let i = 0; i < 3000; i++) {
  const a = new OnArray();
  assert.ok(Array.isArray(a));
  assert.deepStrictEqual([a[0], a[1], a.length, a.named], ['z', 'o', 2, 'n']);

  const b = new OnArrayExplicit();
  assert.deepStrictEqual([b[0], b[2], b.length], ['z', 'w', 3]);

  assert.throws(() => new OnFrozen(), TypeError);
  assert.throws(() => new OnSealed(), TypeError);

  const p = new OnPlain();
  assert.deepStrictEqual([p.kept, p[0], p[1], p.x], [true, 'z', 'o', 1]);

  defined = [];
  new OnProxy();
  assert.deepStrictEqual(defined, ['0', 'y']);

  setterHits = 0;
  const acc = new OnAccessor();
  assert.strictEqual(setterHits, 0);
  assert.strictEqual(Object.getOwnPropertyDescriptor(acc, '0').value, 'z');
  assert.strictEqual(Object.getOwnPropertyDescriptor(acc, 'named').value, 'n');

  assert.throws(() => new OnLocked(), TypeError);
}

class Plain { a = 1; b = [1, 2]; [Symbol.iterator] = null; 3 = 'three'; }
for (let i = 0; i < 3000; i++) {
  const p = new Plain();
  assert.deepStrictEqual(Object.keys(p), ['3', 'a', 'b']);
}

console.log('ok');
