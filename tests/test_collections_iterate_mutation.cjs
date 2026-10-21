const assert = require('node:assert');

function collect(iterable, mutate) {
  const out = [];
  for (const value of iterable) {
    out.push(value);
    mutate?.(value);
  }
  return out;
}

{
  const s = new Set([0]);
  assert.deepStrictEqual(collect(s, v => v < 4 && s.add(v + 1)), [0, 1, 2, 3, 4]);
}

{
  const m = new Map([[0, 'a']]);
  assert.deepStrictEqual(collect(m.keys(), k => k < 3 && m.set(k + 1, 'x')), [0, 1, 2, 3]);
}

{
  const s = new Set([1, 2, 3]);
  assert.deepStrictEqual(collect(s, v => v === 1 && (s.delete(2), s.add(4))), [1, 3, 4]);
}

{
  const s = new Set([1, 2, 3]);
  assert.deepStrictEqual(collect(s, v => v === 2 && s.delete(2)), [1, 2, 3]);
}

{
  const s = new Set([1, 2, 3]);
  assert.deepStrictEqual(collect(s, v => v === 1 && s.delete(1)), [1, 2, 3]);
}

{
  const s = new Set([1, 2, 3]);
  assert.deepStrictEqual(collect(s, v => v === 1 && (s.clear(), s.add(9))), [1, 9]);
}

{
  const m = new Map([[1, 1], [2, 2], [3, 3]]);
  assert.deepStrictEqual(collect(m.values(), v => v === 2 && (m.delete(3), m.set(4, 4))), [1, 2, 4]);
}

{
  const s = new Set([1]);
  const it = s.values();
  assert.deepStrictEqual(it.next(), { value: 1, done: false });
  assert.deepStrictEqual(it.next(), { value: undefined, done: true });
  s.add(2);
  assert.deepStrictEqual(it.next(), { value: undefined, done: true });
}

{
  const s = new Set([1]);
  const it = s.values();
  assert.strictEqual(it.next().value, 1);
  s.add(2);
  assert.strictEqual(it.next().value, 2);
}

{
  const s = new Set([1, 2, 3, 4]);
  const a = s.values();
  const b = s.values();
  assert.strictEqual(a.next().value, 1);
  assert.strictEqual(b.next().value, 1);
  assert.strictEqual(b.next().value, 2);
  s.delete(1);
  s.delete(2);
  assert.strictEqual(a.next().value, 3);
  assert.strictEqual(b.next().value, 3);
}

{
  const m = new Map([['a', 1]]);
  const entries = m.entries();
  assert.deepStrictEqual(entries.next().value, ['a', 1]);
  m.set('b', 2);
  assert.deepStrictEqual(entries.next().value, ['b', 2]);
}

function churnModel() {
  const list = Array.from({ length: 50 }, (_, i) => i);
  const seen = [];
  for (let i = 0; i < list.length; i++) {
    const v = list[i];
    seen.push(v);
    if (v % 3 === 0) {
      const at = list.indexOf(v + 1);
      if (at >= 0) {
        list.splice(at, 1);
        if (at < i) i--;
      }
    }
    if (v < 50 && v % 7 === 0 && !list.includes(100 + v)) list.push(100 + v);
  }
  return seen;
}

const churnExpected = churnModel();
for (let round = 0; round < 200; round++) {
  const s = new Set();
  for (let i = 0; i < 50; i++) s.add(i);
  const seen = [];
  for (const v of s) {
    seen.push(v);
    if (v % 3 === 0) s.delete(v + 1);
    if (v < 50 && v % 7 === 0) s.add(100 + v);
  }
  assert.deepStrictEqual(seen, churnExpected);
}

{
  const iterators = [];
  for (let i = 0; i < 1000; i++) {
    const m = new Map([[i, i]]);
    const it = m.keys();
    it.next();
    iterators.push(it);
  }
  globalThis.gc?.();
  assert.strictEqual(iterators.filter(it => it.next().done).length, 1000);
}

console.log('collection iteration under mutation: ok');
