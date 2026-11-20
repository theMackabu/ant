const assert = require('node:assert');

function keys(o) {
  const out = [];
  for (const k in o) out.push(k);
  return out.join(',');
}

function stable(o, expected) {
  for (let i = 0; i < 300; i++) assert.strictEqual(keys(o), expected);
}

function scenario(make, expected, mutate, after) {
  const o = make();
  stable(o, expected);
  mutate(o);
  stable(o, after);
}

function chain(...levels) {
  let proto = Object.prototype;
  for (let i = levels.length - 1; i >= 0; i--) proto = Object.assign(Object.create(proto), levels[i]);
  return proto;
}

stable({}, '');
stable({ a: 1, b: 2, c: 3 }, 'a,b,c');
stable({ b: 1, 10: 1, a: 1, 2: 1, 1: 1 }, '1,2,10,b,a');
stable(chain({ z: 1 }, { a: 1, b: 2 }), 'z,a,b');
stable(chain({ z: 1 }, { a: 1 }, { b: 1 }, { c: 1 }, { d: 1 }), 'z,a,b,c,d');
stable(chain({ b: 1, z: 1 }, { a: 1, b: 2, z: 3 }, { z: 1, y: 1, a: 1 }), 'b,z,a,y');
stable(Object.assign(Object.create(null), { a: 1, b: 2 }), 'a,b');
stable(Object.create({ 2: 1, b: 1, 1: 1 }), '1,2,b');
stable(Object.assign(Object.create({ 2: 1, b: 1, 1: 1 }), { 3: 1, 1: 1, a: 1 }), '1,3,a,2,b');

{
  const proto = { hidden: 1, shown: 1, tail: 1 };
  const o = Object.create(proto);
  Object.defineProperty(o, 'hidden', { value: 1, enumerable: false });
  o.own = 1;
  stable(o, 'own,shown,tail');
}

{
  const base = { a: 1, b: 2 };
  const mid = Object.create(base);
  Object.defineProperty(mid, 'a', { value: 1, enumerable: false });
  const o = Object.create(mid);
  o.c = 3;
  stable(o, 'c,b');
}

for (const depth of [1, 2, 13, 14, 15, 16, 17, 18, 40]) {
  const deep = [];
  let proto = { base: 1 };
  for (let i = 0; i < depth; i++) {
    proto = Object.create(proto);
    if (i % 3 !== 1) proto['k' + i] = i;
    if (i % 3 !== 1) deep.unshift('k' + i);
  }
  deep.push('base');
  stable(proto, deep.join(','));
  const empty = Object.create(proto);
  stable(empty, deep.join(','));
}

class Base {
  constructor() { this.a = 1; this.b = 2; }
  method() {}
  get accessor() { return 1; }
  static make() { return new this(); }
}
class Derived extends Base {
  constructor() { super(); this.c = 3; }
  other() {}
}
stable(new Base(), 'a,b');
stable(new Derived(), 'a,b,c');
stable(Derived, '');

function Legacy() { this.own = 1; }
Legacy.prototype.inherited = function () {};
Legacy.prototype.value = 1;
stable(new Legacy(), 'own,inherited,value');

stable([1, 2, 3], '0,1,2');
stable([1, , 3], '0,2');
stable(Object.assign([1, 2], { x: 1 }), '0,1,x');
stable([], '');
stable(Array.from({ length: 12 }, (_, i) => i), '0,1,2,3,4,5,6,7,8,9,10,11');
stable({ length: 2, 0: 'a', 1: 'b' }, '0,1,length');
stable((function () { return arguments; })(1, 2, 3), '0,1,2');
{
  const listed = (function () { return arguments; })(1, 2);
  Object.defineProperty(listed, 'length', { value: 2, enumerable: true, configurable: true, writable: true });
  stable(listed, '0,1,length');
  const inherited = Object.create(listed);
  inherited.own = 1;
  stable(inherited, 'own,0,1,length');
  const hidden = (function () { return arguments; })(1, 2);
  hidden.length = 7;
  stable(hidden, '0,1');
  stable(Object.setPrototypeOf(hidden, { length: 1, tail: 1 }), '0,1,tail');
}
stable('abc', '0,1,2');
stable('', '');
stable(new String('ab'), '0,1');
stable(Object.assign(new String('ab'), { x: 1 }), '0,1,x');
stable(Object.create(String.prototype), '');
stable(new String('abc'), '0,1,2');
stable(Object.create(new String('ab')), '0,1');
stable(new Uint8Array(3), '0,1,2');
stable(new Uint8Array(2), '0,1');
stable(new Uint8Array(0), '');
stable(Object.assign(new Uint8Array(2), { x: 1 }), '0,1,x');
stable(Object.create([7, 8]), '0,1');
stable(Object.assign(Object.create([7, 8]), { 1: 1, a: 1 }), '1,a,0');
stable(Object.assign(function () {}, { a: 1, b: 2 }), 'a,b');
stable(new Proxy({ a: 1, b: 2 }, {}), 'a,b');
stable(null, '');
stable(undefined, '');
stable(5, '');
stable(true, '');
stable(Symbol(), '');

{
  class List extends Array {}
  const list = List.from([1, 2]);
  list.extra = 1;
  stable(list, '0,1,extra');
}

{
  const proto = [];
  proto.x = 1;
  const o = Object.create(proto);
  o.length = 1;
  o.y = 2;
  stable(o, 'length,y,x');
  const shadowed = Object.setPrototypeOf([5], { length: 1, z: 1 });
  stable(shadowed, '0,z');
}

{
  function Other() {}
  const plain = new Other();
  Object.defineProperty(plain, 'length', { value: 2 });
  stable(plain, '');
  const wrapper = Reflect.construct(String, ['ab'], Other);
  assert.strictEqual(Object.getPrototypeOf(wrapper), Other.prototype);
  stable(wrapper, '0,1');
  stable(plain, '');
}

scenario(() => ({ a: 1, b: 2 }), 'a,b', (o) => { o.c = 3; }, 'a,b,c');
scenario(() => ({ a: 1, b: 2, c: 3 }), 'a,b,c', (o) => { delete o.b; }, 'a,c');
scenario(() => ({ a: 1, b: 2, c: 3 }), 'a,b,c', (o) => { delete o.b; o.b = 1; }, 'a,c,b');
scenario(() => ({ a: 1, b: 2 }), 'a,b', (o) => { Object.defineProperty(o, 'a', { enumerable: false }); }, 'b');
scenario(() => ({ a: 1, b: 2 }), 'a,b', (o) => { Object.defineProperty(o, 'h', { value: 1, enumerable: false }); }, 'a,b');
scenario(() => ({ a: 1, b: 2 }), 'a,b', (o) => { Object.defineProperty(o, 'g', { get() { return 1; }, enumerable: true }); }, 'a,b,g');
scenario(() => ({ a: 1, b: 2 }), 'a,b', (o) => { o[1] = 1; }, '1,a,b');
scenario(() => ({ a: 1, b: 2 }), 'a,b', (o) => { Object.setPrototypeOf(o, { p: 1 }); }, 'a,b,p');
scenario(() => ({ a: 1, b: 2 }), 'a,b', (o) => { Object.setPrototypeOf(o, null); }, 'a,b');
scenario(() => ({ a: 1, b: 2 }), 'a,b', (o) => { Object.freeze(o); }, 'a,b');
scenario(() => chain({ a: 1 }, { p: 1 }), 'a,p', (o) => { Object.getPrototypeOf(o).q = 1; }, 'a,p,q');
scenario(() => chain({ a: 1 }, { p: 1 }), 'a,p', (o) => { Object.getPrototypeOf(o)['q' + 1] = 1; }, 'a,p,q1');
scenario(() => chain({ a: 1 }, { p: 1 }), 'a,p', (o) => { Object.getPrototypeOf(o)[3] = 1; }, 'a,3,p');
scenario(() => chain({ a: 1 }, { p: 1 }), 'a,p', (o) => { Object.assign(Object.getPrototypeOf(o), { q: 1, r: 2 }); }, 'a,p,q,r');
scenario(() => chain({ a: 1 }, { p: 1 }), 'a,p', (o) => { Object.defineProperty(Object.getPrototypeOf(o), 'q', { value: 1, enumerable: true }); }, 'a,p,q');
scenario(() => chain({ a: 1 }, { p: 1 }), 'a,p', (o) => { Object.defineProperty(Object.getPrototypeOf(o), 'q', { get() { return 1; }, enumerable: true }); }, 'a,p,q');
scenario(() => chain({ a: 1 }, { p: 1 }), 'a,p', (o) => { Object.defineProperty(Object.getPrototypeOf(o), 'q', { value: 1, enumerable: false }); }, 'a,p');
scenario(() => chain({ a: 1 }, { p: 1, q: 2 }), 'a,p,q', (o) => { delete Object.getPrototypeOf(o).p; }, 'a,q');
scenario(() => chain({ a: 1 }, { p: 1, q: 2 }), 'a,p,q', (o) => { Object.defineProperty(Object.getPrototypeOf(o), 'p', { enumerable: false }); }, 'a,q');
scenario(() => chain({ a: 1 }, { p: 1 }), 'a,p', (o) => { Object.setPrototypeOf(Object.getPrototypeOf(o), { deep: 1 }); }, 'a,p,deep');
scenario(() => chain({ a: 1 }, { p: 1 }, { deep: 1 }), 'a,p,deep', (o) => { Object.setPrototypeOf(Object.getPrototypeOf(o), null); }, 'a,p');
scenario(() => chain({ a: 1 }, { p: 1 }, { r: 1 }), 'a,p,r', (o) => { Object.getPrototypeOf(Object.getPrototypeOf(o)).s = 1; }, 'a,p,r,s');
scenario(() => chain({ a: 1 }, {}, { r: 1 }), 'a,r', (o) => { Object.defineProperty(Object.getPrototypeOf(o), 'r', { value: 1, enumerable: false }); }, 'a');
scenario(() => chain({ a: 1 }, {}, { r: 1 }), 'a,r', (o) => { Object.getPrototypeOf(o).r = 2; }, 'a,r');
scenario(() => chain({ a: 1 }, { r: 2 }, { r: 1, s: 1 }), 'a,r,s', (o) => { delete Object.getPrototypeOf(o).r; }, 'a,r,s');
scenario(() => chain({ a: 1 }, { p: 1 }), 'a,p', (o) => { Object.setPrototypeOf(Object.getPrototypeOf(o), [4, 5]); }, 'a,p,0,1');
scenario(() => chain({ a: 1 }, { p: 1 }), 'a,p', (o) => { Object.setPrototypeOf(Object.getPrototypeOf(o), new String('xy')); }, 'a,p,0,1');
scenario(() => ({ a: 1 }), 'a', () => { Object.prototype.polluted = 1; }, 'a,polluted');
delete Object.prototype.polluted;
scenario(() => ({ a: 1 }), 'a', () => { Object.defineProperty(Object.prototype, 'toString', { enumerable: true }); }, 'a,toString');
Object.defineProperty(Object.prototype, 'toString', { enumerable: false });
stable({ a: 1 }, 'a');
scenario(() => new Base(), 'a,b', () => { Base.prototype.late = 1; }, 'a,b,late');
delete Base.prototype.late;
scenario(() => new Derived(), 'a,b,c', () => { Base.prototype.late = 1; }, 'a,b,c,late');
delete Base.prototype.late;
scenario(() => new Derived(), 'a,b,c', () => { Object.defineProperty(Derived.prototype, 'other', { enumerable: true }); }, 'a,b,c,other');
scenario(() => Object.create([]), '', (o) => { Object.getPrototypeOf(o).push(1); }, '0');
scenario(() => Object.create(Object.create([])), '', (o) => { Object.getPrototypeOf(Object.getPrototypeOf(o)).push(1, 2); }, '0,1');
scenario(() => [1, 2], '0,1', (o) => { o.push(3); }, '0,1,2');
scenario(() => [1, 2, 3], '0,1,2', (o) => { o.length = 1; }, '0');
scenario(() => [1, 2, 3], '0,1,2', (o) => { delete o[1]; }, '0,2');
scenario(() => [1, 2], '0,1', (o) => { o.x = 1; }, '0,1,x');
scenario(() => [1, 2], '0,1', () => { Array.prototype.extra = 1; }, '0,1,extra');
delete Array.prototype.extra;
stable([1, 2], '0,1');

{
  const proto = { p: 1 };
  const first = Object.create(proto);
  const second = Object.create(proto);
  first.a = 1;
  second.a = 1;
  stable(first, 'a,p');
  stable(second, 'a,p');
  proto.q = 1;
  stable(second, 'a,p,q');
  stable(first, 'a,p,q');
  second.q = 2;
  stable(second, 'a,q,p');
  stable(first, 'a,p,q');
}

{
  function Point(x, y) { this.x = x; this.y = y; }
  const points = [];
  for (let i = 0; i < 50; i++) points.push(new Point(i, i));
  for (const point of points) assert.strictEqual(keys(point), 'x,y');
  points[7].z = 1;
  delete points[9].x;
  Object.defineProperty(points[11], 'y', { enumerable: false });
  for (let i = 0; i < 50; i++) {
    assert.strictEqual(keys(points[i]), i === 7 ? 'x,y,z' : i === 9 ? 'y' : i === 11 ? 'x' : 'x,y');
  }
}

{
  const o = { a: 1, b: 2, c: 3 };
  for (let round = 0; round < 300; round++) {
    const seen = [];
    for (const k in o) {
      seen.push(k);
      if (k === 'a') o['added' + round] = 1;
    }
    assert.strictEqual(seen.length, 3 + round);
    assert.strictEqual(seen[0], 'a');
  }
}

{
  const proto = { p: 1 };
  const o = Object.create(proto);
  o.a = 1;
  for (let round = 0; round < 300; round++) {
    const seen = [];
    for (const k in o) {
      seen.push(k);
      if (round === 150 && k === 'a') proto.added = 1;
    }
    assert.strictEqual(seen.join(','), round <= 150 ? 'a,p' : 'a,p,added');
  }
}

{
  const o = { a: 1, b: 2 };
  let total = 0;
  for (let round = 0; round < 300; round++) {
    for (const outer in o) for (const inner in o) total += outer.length + inner.length;
  }
  assert.strictEqual(total, 300 * 8);
}

{
  const dictionary = {};
  for (let i = 0; i < 1500; i++) dictionary['k' + i] = i;
  for (let i = 0; i < 1500; i += 2) delete dictionary['k' + i];
  let count = 0;
  for (let round = 0; round < 20; round++) for (const k in dictionary) count += k.length > 1 ? 1 : 0;
  assert.strictEqual(count, 20 * 750);
  dictionary.tail = 1;
  assert.strictEqual(keys(dictionary).split(',').pop(), 'tail');
}

{
  const shapes = [];
  for (let i = 0; i < 400; i++) {
    const o = {};
    o['u' + i] = 1;
    o.shared = 2;
    shapes.push(o);
  }
  for (let round = 0; round < 3; round++) {
    for (let i = 0; i < 400; i++) assert.strictEqual(keys(shapes[i]), 'u' + i + ',shared');
  }
}

{
  const churn = { a: 1, b: 2 };
  for (let i = 0; i < 20000; i++) {
    const garbage = { x: i, y: [i], z: 'v' + i };
    if (keys(garbage) !== 'x,y,z') assert.fail('fresh object keys');
    if (keys(churn) !== 'a,b') assert.fail('cached keys after allocation');
  }
}

{
  function Wide() { this.a = 1; this.b = 2; this.c = 3; this.d = 4; }
  for (let i = 0; i < 60; i++) {
    Object.defineProperty(Wide.prototype, 'method' + i, { value() {}, writable: true, configurable: true });
  }
  const inherited = new Wide();
  const bare = Object.assign(Object.create(null), { a: 1, b: 2, c: 3, d: 4 });
  function walk(o) { let n = 0; for (const k in o) n += k.length; return n; }
  function cost(o) {
    let best = Infinity;
    for (let round = 0; round < 9; round++) {
      const start = process.hrtime.bigint();
      for (let i = 0; i < 5000; i++) walk(o);
      const elapsed = Number(process.hrtime.bigint() - start);
      if (elapsed < best) best = elapsed;
    }
    return best;
  }
  assert.strictEqual(walk(inherited), 4);
  assert.strictEqual(walk(bare), 4);
  cost(inherited);
  const withProto = cost(inherited);
  const withoutProto = cost(bare);
  assert.ok(withProto < withoutProto * 3, `prototype methods cost ${withProto}ns against ${withoutProto}ns without them`);
}

console.log('ok');
