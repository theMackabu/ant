const assert = require('node:assert');

const pad = (n, from = 0) => Array.from({ length: n }, (_, i) => i + from).join(',');

const nested = (x) => [x, [1, 2, [3]], { a: 1, b: 'two', c: [4, { d: null }] }, , -5, -0, true, 'str', ...[7, 8]];
const table = () => [{ id: 1, name: 'a', tags: ['x', 'y'] }, { id: 2, name: 'b', tags: [] }, {}, []];
const deep = () => [[[[[[[[[[[1]]]]]]]]]]];
const wide = new Function(`return [${pad(40)}].map((v) => v);`);
const many = new Function(`return [${Array.from({ length: 40 }, (_, i) => `{ k: ${i}, v: [${i}, ${i + 1}] }`).join(',')}];`);
const holes = () => [[1, , 3], { a: [, 2] }, , [, ,]];
const keyed = () => [{ __proto__: null, a: 1 }, { 1: 'one', b: 2 }, { 'quoted key': 1, get g() { return 5; } }];

function snapshot(value) {
  return JSON.stringify(value, (_, v) => (Object.is(v, -0) ? '-0' : v));
}

const expect = {
  nested: snapshot(nested(0)),
  table: snapshot(table()),
  deep: snapshot(deep()),
  many: snapshot(many()),
  holes: snapshot(holes()),
};

for (let i = 0; i < 30000; i++) {
  const n = nested(i);
  assert.strictEqual(n[0], i);
  if (i < 5 || i % 997 === 0) {
    n[0] = 0;
    assert.strictEqual(snapshot(n), expect.nested);
    assert.ok(!(3 in n));
    assert.ok(Object.is(n[5], -0));
  }
  n[1].push(i);
  n[1][2].push(i);
  n[2].c[1].d = i;
  n[2].a = i;

  const t = table();
  if (i % 991 === 0) assert.strictEqual(snapshot(t), expect.table);
  t[0].tags.push(i);
  t[1].tags.length = 5;
  t[0].name = i;
  t[2].extra = i;

  const d = deep();
  if (i % 983 === 0) assert.strictEqual(snapshot(d), expect.deep);
  d[0][0][0][0] = i;

  const m = many();
  if (i % 977 === 0) assert.strictEqual(snapshot(m), expect.many);
  m[39].v.push(i);
  m[0].k = i;

  const h = holes();
  if (i % 971 === 0) {
    assert.strictEqual(snapshot(h), expect.holes);
    assert.ok(!(1 in h[0]) && !(0 in h[1].a) && !(2 in h) && h[3].length === 2 && !(0 in h[3]));
  }
  h[0][1] = i;
}

const k = keyed();
assert.strictEqual(Object.getPrototypeOf(k[0]), null);
assert.deepStrictEqual(Object.keys(k[1]), ['1', 'b']);
assert.strictEqual(k[2].g, 5);
assert.deepStrictEqual(wide(), Array.from({ length: 40 }, (_, i) => i));

const order = [];
const tick = (v) => (order.push(v), v);
const ordered = () => [tick('a'), 1, [2, 3], tick('b'), { c: 4 }, tick('c'), ...[tick('d')], tick('e')];
for (let i = 0; i < 3; i++) {
  order.length = 0;
  assert.deepStrictEqual(ordered(), ['a', 1, [2, 3], 'b', { c: 4 }, 'c', 'd', 'e']);
  assert.deepStrictEqual(order, ['a', 'b', 'c', 'd', 'e']);
}

const boom = () => { throw new Error('boom'); };
const throwing = () => [1, [2], boom(), 3];
for (let i = 0; i < 3; i++) assert.throws(throwing, /boom/);

function* gen() {
  const a = [1, [2, 3], yield 'first', { k: yield 'second' }, 5];
  return a;
}
const it = gen();
assert.strictEqual(it.next().value, 'first');
assert.strictEqual(it.next('x').value, 'second');
assert.deepStrictEqual(it.next('y').value, [1, [2, 3], 'x', { k: 'y' }, 5]);

const big = new Function('x', `return [x, ${pad(70000)}, [1, 2], x];`);
for (let i = 0; i < 4; i++) {
  const b = big(i);
  assert.strictEqual(b.length, 70003);
  assert.strictEqual(b[0], i);
  assert.strictEqual(b[70002], i);
  assert.deepStrictEqual(b[70001], [1, 2]);
  b[70001].push(9);
}

const flatQueries = new Function(`return [${pad(50)}];`);
for (let i = 0; i < 10; i++) {
  const o = flatQueries();
  assert.strictEqual(Object.isFrozen(o), false);
  assert.strictEqual(Object.isSealed(o), false);
  assert.strictEqual(Object.isExtensible(o), true);
  assert.deepStrictEqual(Object.getOwnPropertyDescriptor(o, 0), { value: 0, writable: true, enumerable: true, configurable: true });
  if (i % 2) Object.freeze(o);
  else o.push(1);
}
assert.strictEqual(flatQueries().length, 50);

console.log('ok');
