const assert = require('node:assert');

const collect = typeof globalThis.gc === 'function' ? globalThis.gc : () => {};

function churn(target, set, rounds) {
  const a = { which: 'a', onlyA: 1 };
  const b = { which: 'b', onlyB: 2 };
  for (let i = 0; i < rounds; i++) {
    const proto = i & 1 ? a : b;
    set(target, proto);
    if ((i & 8191) === 0) {
      assert.strictEqual(Object.getPrototypeOf(target), proto);
      assert.strictEqual(target.which, i & 1 ? 'a' : 'b');
      assert.strictEqual(target.onlyA, i & 1 ? 1 : undefined);
      assert.strictEqual(target.onlyB, i & 1 ? undefined : 2);
    }
  }
  return [a, b];
}

const viaAccessor = (target, proto) => { target.__proto__ = proto; };
const viaSetPrototypeOf = (target, proto) => { Object.setPrototypeOf(target, proto); };
const viaReflect = (target, proto) => { Reflect.setPrototypeOf(target, proto); };

function fn() { return 7; }
fn.extra = 1;
churn(fn, viaAccessor, 300000);
collect();
assert.strictEqual(fn(), 7);
assert.strictEqual(fn.extra, 1);
assert.strictEqual(fn.which, 'a');
assert.strictEqual(typeof fn.call, 'undefined');

const plain = { q: 1, r: 2 };
churn(plain, viaAccessor, 200000);
churn(plain, viaSetPrototypeOf, 200000);
churn(plain, viaReflect, 50000);
collect();
assert.deepStrictEqual(Object.keys(plain), ['q', 'r']);
assert.strictEqual(plain.which, 'a');

const empty = {};
churn(empty, viaAccessor, 100000);
assert.strictEqual(empty.which, 'a');

const list = [1, 2, 3];
list.tag = 'x';
churn(list, viaAccessor, 100000);
assert.strictEqual(list.length, 3);
assert.strictEqual(list.tag, 'x');
assert.strictEqual(list.map, undefined);
Object.setPrototypeOf(list, Array.prototype);
assert.deepStrictEqual(list.map(v => v * 2), [2, 4, 6]);

const many = [];
const shared = [{ id: 0 }, { id: 1 }, { id: 2 }];
for (let i = 0; i < 3000; i++) {
  const o = { n: i };
  for (let j = 0; j < 7; j++) Object.setPrototypeOf(o, shared[(i + j) % 3]);
  many.push(o);
}
collect();
for (let i = 0; i < many.length; i++) {
  assert.strictEqual(many[i].n, i);
  assert.strictEqual(many[i].id, (i + 6) % 3);
}

const p0 = { level: 0 };
const p1 = { level: 1 };
const deep = {};
for (let i = 0; i < 1500; i++) {
  deep['k' + i] = i;
  Object.setPrototypeOf(deep, i & 1 ? p1 : p0);
  if (i % 3 === 0) Object.setPrototypeOf(deep, i & 1 ? p0 : p1);
}
collect();
assert.strictEqual(Object.keys(deep).length, 1500);
assert.strictEqual(deep.k0, 0);
assert.strictEqual(deep.k1499, 1499);
assert.strictEqual(deep.level, 1);

const twins = [];
for (let t = 0; t < 4; t++) {
  const o = {};
  for (let i = 0; i < 40; i++) {
    o['k' + i] = i + t;
    Object.setPrototypeOf(o, i & 1 ? p1 : p0);
  }
  twins.push(o);
}
for (let t = 0; t < 4; t++) {
  assert.strictEqual(twins[t].k39, 39 + t);
  assert.strictEqual(twins[t].level, 1);
}
p1.level = 'changed';
assert.strictEqual(twins[0].level, 'changed');
assert.strictEqual(deep.level, 'changed');

const steady = { q: 1, r: 2 };
churn(steady, viaSetPrototypeOf, 1000);
const settled = process.memoryUsage().rss;
churn(steady, viaSetPrototypeOf, 1500000);
assert.ok(process.memoryUsage().rss - settled < 64 * 1024 * 1024);

const pile = [];
for (let i = 0; i < 200000; i++) pile.push({ i });
pile.length = 0;
collect();

console.log('ok');
