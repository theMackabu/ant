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

{
  class Ret { constructor(o) { return o; } }
  class Tagged extends Ret { tag = 1; }
  class List extends Array { tag = 1; 0 = 'z'; 1 = 'b'; '01' = 'c'; [Symbol.for('q')] = 2; }
  const list = new List();
  assert.deepStrictEqual([list.tag, list[0], list.length, list['01'], list[Symbol.for('q')]], [1, 'z', 2, 'c', 2]);
  assert.deepStrictEqual(Object.getOwnPropertyDescriptor(list, 'tag'), { value: 1, writable: true, enumerable: true, configurable: true });
  assert.throws(() => new (class extends Array { length = 5 })(), TypeError);
  assert.throws(() => new Tagged(Object.freeze([])), TypeError);
  assert.throws(() => new Tagged(Object.preventExtensions([1])), TypeError);

  let hit = 0;
  const withSetter = [];
  Object.defineProperty(withSetter, 'tag', { set() { hit++; }, configurable: true });
  new Tagged(withSetter);
  assert.deepStrictEqual([hit, withSetter.tag], [0, 1]);

  const pinned = [];
  Object.defineProperty(pinned, 'tag', { value: 0, writable: true, configurable: false });
  assert.throws(() => new Tagged(pinned), TypeError);

  class Later extends Promise { tag = 1; }
  const later = new Later(() => {});
  assert.ok(later instanceof Promise);
  assert.strictEqual(later.tag, 1);

  function* gen() {}
  const it = gen();
  new Tagged(it);
  assert.strictEqual(it.tag, 1);
}

{
  class Ret { constructor(o) { return o; } }
  class Index extends Ret { 0 = 'z'; }
  class Past extends Ret { 5 = 'z'; }
  class Len extends Ret { length = 5; }
  class Named extends Ret { tag = 1; [Symbol.for('q')] = 2; '01' = 3; }
  assert.throws(() => new Index(new String('ab')), TypeError);
  assert.throws(() => new Len(new String('ab')), TypeError);
  const text = new String('ab');
  new Past(text);
  new Named(text);
  assert.deepStrictEqual([text[0], text[5], text.tag, text[Symbol.for('q')], text['01'], text.length], ['a', 'z', 1, 2, 3, 2]);
  assert.deepStrictEqual(Object.keys(text), ['0', '1', '5', 'tag', '01']);

  class Text extends String { tag = 1; 9 = 'far'; }
  const own = new Text('ab');
  assert.deepStrictEqual([own.tag, own[9], own[0], String(own)], [1, 'far', 'a', 'ab']);
  assert.throws(() => new (class extends String { 1 = 'x' })('ab'), TypeError);
  assert.throws(() => new (class extends String { length = 1 })('ab'), TypeError);

  for (const boxed of [new Number(1), new Boolean(true), Object(Symbol('s')), Object(1n)]) {
    new Index(boxed); new Len(boxed); new Named(boxed);
    assert.deepStrictEqual([boxed[0], boxed.length, boxed.tag], ['z', 5, 1]);
  }

  class Keyed extends Map { tag = 1; 0 = 'z'; length = 2; }
  const keyed = new Keyed([[1, 2]]);
  assert.deepStrictEqual([keyed.tag, keyed[0], keyed.length, keyed.size, keyed.get(1)], [1, 'z', 2, 1, 2]);

  class Both { #p() { return 1; } tag = 1; 0 = 'z'; length = 2; sum() { return this.#p() + this.tag; } }
  const both = new Both();
  assert.deepStrictEqual([both.sum(), both[0], both.length], [2, 'z', 2]);
}

class Plain { a = 1; b = [1, 2]; [Symbol.iterator] = null; 3 = 'three'; }
for (let i = 0; i < 3000; i++) {
  const p = new Plain();
  assert.deepStrictEqual(Object.keys(p), ['3', 'a', 'b']);
}

console.log('ok');
