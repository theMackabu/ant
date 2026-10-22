const assert = require('node:assert');
const util = require('node:util');

function churn() {
  let keep = [];
  for (let i = 0; i < 200000; i++) keep.push({ i });
  keep = null;
}

{
  const m = new Map([[1, 'a'], [2, 'b'], [3, 'c'], [4, 'd']]);
  const seen = [];
  m.forEach((value, key) => {
    seen.push(key);
    if (key === 1) m.delete(2);
    if (key === 3) m.set(5, 'e');
  });
  assert.deepStrictEqual(seen, [1, 3, 4, 5]);
}

{
  const m = new Map([[1, 'a'], [2, 'b'], [3, 'c']]);
  const seen = [];
  m.forEach((value, key) => {
    seen.push(key);
    if (key === 1) {
      m.clear();
      m.set(9, 'z');
    }
  });
  assert.deepStrictEqual(seen, [1, 9]);
}

{
  const s = new Set([1, 2, 3, 4]);
  const seen = [];
  s.forEach(value => {
    seen.push(value);
    if (value === 2) s.delete(3);
    if (value === 4) s.add(6);
  });
  assert.deepStrictEqual(seen, [1, 2, 4, 6]);
}

{
  const s = new Set([1, 2, 3, 4]);
  const other = {
    size: 10,
    has(value) {
      if (value === 1) s.clear();
      return true;
    },
    keys() {
      return [][Symbol.iterator]();
    },
  };
  assert.deepStrictEqual([...s.intersection(other)], [1]);

  const t = new Set([1, 2, 3, 4]);
  const deleting = { ...other, has(value) { t.delete(value + 1); return false; } };
  assert.deepStrictEqual([...t.difference(deleting)], [1, 2, 3, 4]);
  assert.deepStrictEqual([...t], [1, 3]);
  assert.strictEqual(new Set([1, 2]).isSubsetOf({ ...other, has: () => true }), true);

  class Odd extends Set {
    has(value) {
      return value % 2 === 1;
    }
  }
  const odd = new Odd([5, 6, 7, 8]);
  assert.deepStrictEqual([...new Set([1, 2, 3, 4]).difference(odd)], [2, 4]);
  assert.deepStrictEqual([...new Set([1, 2, 3, 4]).intersection(odd)], [1, 3]);
  assert.strictEqual(new Set([1, 3]).isSubsetOf(odd), true);
  assert.strictEqual(new Set([2, 4]).isDisjointFrom(odd), true);
  assert.deepStrictEqual([...new Set([1, 2, 3, 4]).difference(new Set([2, 4, 6, 8]))], [1, 3]);
  assert.strictEqual(new Set([1, 2]).isDisjointFrom({ ...other, has: () => false }), true);
}

{
  const m = new Map();
  const key = { toString() { m.clear(); return 'key'; } };
  m.set(key, 1);
  m.set('other', 2);
  assert.match(util.inspect(m), /Map/);
}

{
  const lru = new Map();
  for (let i = 0; i < 20000; i++) {
    lru.set(i, i);
    if (lru.size > 100) lru.delete(lru.keys().next().value);
  }
  assert.strictEqual(lru.size, 100);
  assert.strictEqual(lru.keys().next().value, 19900);
}

{
  const m = new Map([[1, 1], [2, 2], [3, 3], [4, 4], [5, 5]]);
  const a = m.keys();
  const b = m.keys();
  const c = m.keys();
  assert.strictEqual(a.next().value, 1);
  assert.strictEqual(b.next().value, 1);
  assert.strictEqual(b.next().value, 2);
  m.delete(2);
  assert.strictEqual(a.next().value, 3);
  m.delete(3);
  assert.strictEqual(b.next().value, 4);
  assert.strictEqual(a.next().value, 4);
  assert.deepStrictEqual([...c], [1, 4, 5]);
  m.delete(5);
  m.set(6, 6);
  assert.deepStrictEqual([...a], [6]);
  assert.deepStrictEqual([...b], [6]);
}

{
  const s = new Set([1, 2, 3]);
  const a = s.values();
  const b = s.values();
  a.next();
  s.clear();
  const fresh = s.values();
  s.add(7);
  s.add(8);
  assert.deepStrictEqual([...a], [7, 8]);
  assert.deepStrictEqual([...b], [7, 8]);
  assert.deepStrictEqual([...fresh], [7, 8]);
}

{
  const s = new Set();
  const early = s.values();
  s.add(1);
  const done = s.values();
  assert.deepStrictEqual([...done], [1]);
  s.add(2);
  assert.deepStrictEqual(done.next(), { value: undefined, done: true });
  assert.deepStrictEqual([...early], [1, 2]);
}

{
  const m = new Map();
  const leaked = [];
  for (let r = 0; r < 5000; r++) {
    const k = r % 300;
    if (m.has(k)) {
      const v = m.get(k);
      m.delete(k);
      m.set(k, v);
    } else {
      m.set(k, r);
      if (m.size > 200) {
        const it = m.keys();
        m.delete(it.next().value);
        if (r % 500 === 0) leaked.push(it);
      }
    }
  }
  assert.strictEqual(m.size, 200);
  const rest = [...m.keys()];
  assert.ok(leaked.length > 0);
  for (const it of leaked) {
    const got = [...it];
    assert.ok(got.length > 0);
    assert.deepStrictEqual(got, rest.slice(rest.length - got.length));
  }
}

{
  const iters = [];
  let m = new Map([[1, 1], [2, 2]]);
  for (let i = 0; i < 50; i++) {
    const it = m.keys();
    it.next();
    iters.push(it);
  }
  m.delete(2);
  m.set(3, 3);
  assert.deepStrictEqual([...iters[0]], [3]);
  m = null;
  churn();
  for (const it of iters.slice(1)) assert.deepStrictEqual([...it], [3]);
}

{
  let s = new Set([1, 2, 3]);
  const kept = s.values();
  kept.next();
  for (let i = 0; i < 100; i++) s.values().next();
  churn();
  s.delete(2);
  assert.deepStrictEqual([...kept], [3]);
  s = null;
  churn();
  assert.deepStrictEqual(kept.next(), { value: undefined, done: true });
}

{
  const m = new Map();
  for (let i = 0; i < 64; i++) m.set(i, i);
  const keep = [];
  for (let round = 0; round < 40; round++) {
    let lead = m.keys();
    lead.next();
    const follow = m.keys();
    follow.next();
    follow.next();
    lead.next();
    keep.push(follow);
    lead = null;
    churn();
    m.delete(2);
    m.set(2, 2);
    if (round % 7 === 0) m.clear();
    for (let i = 0; i < 64; i++) if (!m.has(i)) m.set(i, i);
  }
  for (const it of keep) {
    const rest = [...it];
    assert.ok(rest.every(key => m.has(key)));
  }
}

{
  const s = new Set([1, 2, 3, 4, 5, 6]);
  const walkers = [];
  const seen = [];
  s.forEach(value => {
    seen.push(value);
    const walker = s.values();
    while (walker.next().value !== value);
    walkers.push(walker);
    if (value === 3) s.delete(4);
  });
  assert.deepStrictEqual(seen, [1, 2, 3, 5, 6]);
  churn();
  s.add(7);
  assert.deepStrictEqual(walkers.map(w => [...w].length), [5, 4, 3, 2, 1]);
}

{
  const seen = [];
  new Map([[1, 'a']]).forEach(Array.prototype.push, seen);
  new Set([2]).forEach(Array.prototype.push, seen);
  assert.deepStrictEqual(seen.map(String), ['a', '1', '[object Map]', '2', '2', '[object Set]']);
  assert.throws(() => new Map().forEach(42), TypeError);
  assert.throws(() => new Set().forEach(), TypeError);
}

console.log('collection callbacks under mutation: ok');
