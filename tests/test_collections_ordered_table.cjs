const assert = require('node:assert/strict');

function sameValueZero() {
  const nanBits = new Float64Array(new BigUint64Array([0x7ff8000000000001n, 0xfff8000000000000n, 0x7ff0000000000001n]).buffer);
  const m = new Map([[NaN, 'nan'], [0, 'zero']]);
  for (const nan of nanBits) {
    assert.ok(Number.isNaN(nan));
    assert.equal(m.get(nan), 'nan');
    assert.ok(m.has(nan));
  }
  assert.equal(m.get(-0), 'zero');
  m.set(-0, 'negzero');
  assert.equal(m.size, 2);
  assert.ok(Object.is([...m.keys()][1], 0));
  assert.equal(m.get(0), 'negzero');

  const s = new Set([0, -0, NaN, ...nanBits, 0n, -0n, 1n, 1n, 2n ** 70n, 2n ** 70n, -(2n ** 70n)]);
  assert.equal(s.size, 6);
  assert.ok(Object.is([...s][0], 0));
  assert.ok(s.has(2n ** 70n) && s.has(-(2n ** 70n)) && !s.has(2n ** 71n) && s.has(0n) && !s.has(-1n));
  assert.ok(!s.has('0') && !s.has(false));

  const prefix = 'a'.repeat(40);
  const flat = prefix + 'tail';
  const rope = [prefix, 'tail'].join('');
  const built = prefix.slice(0, 20) + prefix.slice(20) + 't' + 'ail';
  const sm = new Map([[flat, 1]]);
  assert.equal(sm.get(rope), 1);
  assert.equal(sm.get(built), 1);
  sm.set(built, 2);
  assert.equal(sm.size, 1);
  assert.equal(sm.get(flat), 2);
  assert.equal(sm.get(prefix + 'tai'), undefined);
  assert.equal(sm.get(''), undefined);
  sm.set('', 'empty');
  assert.equal(sm.get(''.concat('')), 'empty');
  assert.equal(new Map([['é', 1]]).get('é'), undefined);
  assert.equal(new Map([['\u{1F600}', 1]]).get('\uD83D' + '\uDE00'), 1);

  const o1 = {}, o2 = {}, sym = Symbol('x');
  const om = new Map([[o1, 1], [o2, 2], [sym, 3], [null, 4], [undefined, 5], [true, 6], ['true', 7], [1, 8], ['1', 9]]);
  assert.deepEqual([o1, o2, sym, null, undefined, true, 'true', 1, '1'].map((k) => om.get(k)), [1, 2, 3, 4, 5, 6, 7, 8, 9]);
  assert.equal(om.get({}), undefined);
  assert.equal(om.get(Symbol('x')), undefined);
}

function insertionOrder() {
  const m = new Map();
  for (let i = 0; i < 10; i++) m.set('k' + i, i);
  m.delete('k3');
  m.delete('k0');
  m.set('k3', 33);
  m.set('k5', 55);
  m.set('k0', 0);
  assert.deepEqual([...m.keys()], ['k1', 'k2', 'k4', 'k5', 'k6', 'k7', 'k8', 'k9', 'k3', 'k0']);
  assert.deepEqual([...m.values()], [1, 2, 4, 55, 6, 7, 8, 9, 33, 0]);

  const s = new Set([5, 4, 3, 2, 1]);
  s.delete(3);
  s.add(3);
  s.add(5);
  assert.deepEqual([...s], [5, 4, 2, 1, 3]);
}

function mutationDuringIteration() {
  {
    const m = new Map([[1, 1], [2, 2], [3, 3], [4, 4], [5, 5]]);
    const seen = [];
    for (const [k] of m) {
      seen.push(k);
      if (k === 1) m.delete(1);
      if (k === 2) m.delete(4);
      if (k === 3) m.delete(1);
      if (k === 3) m.set(6, 6);
      if (k === 5) m.set(2, 22);
    }
    assert.deepEqual(seen, [1, 2, 3, 5, 6]);
    assert.deepEqual([...m], [[2, 22], [3, 3], [5, 5], [6, 6]]);
  }
  {
    const m = new Map([[1, 1], [2, 2]]);
    const seen = [];
    for (const k of m.keys()) {
      seen.push(k);
      if (seen.length === 1) { m.delete(1); m.set(1, 'again'); }
      if (seen.length > 10) break;
    }
    assert.deepEqual(seen, [1, 2, 1]);
  }
  {
    const s = new Set([1, 2, 3]);
    const seen = [];
    for (const v of s) {
      seen.push(v);
      if (v === 2) { s.clear(); s.add('after'); }
    }
    assert.deepEqual(seen, [1, 2, 'after']);
  }
  {
    const m = new Map([[1, 1], [2, 2], [3, 3]]);
    const seen = [];
    m.forEach((v, k, map) => {
      seen.push(k);
      if (k === 1) map.delete(2);
      if (k === 3) map.clear();
      if (k === 3) map.set(9, 9);
    });
    assert.deepEqual(seen, [1, 3, 9]);
  }
  {
    const m = new Map([[1, 1], [2, 2], [3, 3]]);
    const it = m.entries();
    assert.deepEqual(it.next().value, [1, 1]);
    m.delete(2);
    m.set(4, 4);
    assert.deepEqual([...it], [[3, 3], [4, 4]]);
    m.set(5, 5);
    assert.equal(it.next().done, true);
  }
  {
    const s = new Set([1]);
    const it = s.values();
    s.delete(1);
    s.add(2);
    assert.deepEqual(it.next(), { value: 2, done: false });
    assert.deepEqual(it.next(), { value: undefined, done: true });
  }
}

function compactionWithLiveIterators() {
  const m = new Map();
  for (let i = 0; i < 1000; i++) m.set(i, i);
  const early = m.keys();
  const mid = m.values();
  for (let i = 0; i < 500; i++) mid.next();
  const late = m.keys();
  for (const _ of late);
  const end = m.entries();
  for (let i = 0; i < 999; i++) end.next();

  for (let i = 0; i < 1000; i += 1) if (i % 100 !== 7) m.delete(i);
  for (let i = 0; i < 3000; i++) m.set(10000 + i, i);
  for (let i = 0; i < 3000; i++) m.delete(10000 + i);
  m.set('tail', 't');

  assert.deepEqual([...early], [7, 107, 207, 307, 407, 507, 607, 707, 807, 907, 'tail']);
  assert.deepEqual([...mid], [507, 607, 707, 807, 907, 't']);
  assert.deepEqual([...late], []);
  assert.deepEqual([...end], [[999, 999], ['tail', 't']].slice(1));
  assert.deepEqual([...m.keys()], [7, 107, 207, 307, 407, 507, 607, 707, 807, 907, 'tail']);

  const s = new Set();
  for (let i = 0; i < 64; i++) s.add(i);
  const it = s.values();
  it.next();
  for (let i = 0; i < 64; i++) s.delete(i);
  for (let i = 0; i < 64; i++) s.add(i + 100);
  for (let i = 0; i < 60; i++) s.delete(i + 100);
  assert.deepEqual([...it], [160, 161, 162, 163]);

  const q = new Map();
  for (let i = 0; i < 100; i++) q.set(i, i);
  let next = 100, sum = 0;
  for (let i = 0; i < 20000; i++) {
    const k = q.keys().next().value;
    sum += q.get(k);
    q.delete(k);
    q.set(next, next);
    next++;
  }
  assert.equal(q.size, 100);
  assert.equal(sum, 199990000);
  assert.deepEqual([...q.keys()].slice(0, 3), [20000, 20001, 20002]);
}

function largeGrowShrink() {
  const N = 200000;
  const m = new Map();
  for (let i = 0; i < N; i++) m.set(i, i * 2);
  assert.equal(m.size, N);
  for (let i = 0; i < N; i += 1) assert.equal(m.get(i), i * 2);
  for (let i = 0; i < N; i++) if (i % 100) m.delete(i);
  assert.equal(m.size, N / 100);
  let expect = 0;
  for (const [k, v] of m) {
    assert.equal(k, expect);
    assert.equal(v, expect * 2);
    expect += 100;
  }
  for (let i = 0; i < N; i += 100) m.delete(i);
  assert.equal(m.size, 0);
  assert.deepEqual([...m], []);
  m.set('x', 1);
  assert.deepEqual([...m], [['x', 1]]);

  const s = new Set();
  for (let i = 0; i < N; i++) s.add('s' + i);
  for (let i = 0; i < N; i++) assert.ok(s.has('s' + i));
  assert.ok(!s.has('s' + N));
  s.clear();
  assert.equal(s.size, 0);
  s.add(1);
  assert.deepEqual([...s], [1]);
}

function setMethods() {
  const a = new Set([1, 2, 3, 4, 5, 'x', -0]);
  const b = new Set([4, 5, 6, 'x', 0, NaN]);
  assert.deepEqual([...a.union(b)], [1, 2, 3, 4, 5, 'x', 0, 6, NaN]);
  assert.deepEqual([...a.intersection(b)], [4, 5, 'x', 0]);
  assert.deepEqual([...b.intersection(a)], [4, 5, 'x', 0]);
  assert.deepEqual([...a.difference(b)], [1, 2, 3]);
  assert.deepEqual([...b.difference(a)], [6, NaN]);
  assert.deepEqual([...a.symmetricDifference(b)], [1, 2, 3, 6, NaN]);
  assert.ok(new Set([4, 5]).isSubsetOf(a));
  assert.ok(!a.isSubsetOf(b));
  assert.ok(a.isSupersetOf(new Set([1, 'x'])));
  assert.ok(new Set([7, 8]).isDisjointFrom(a));
  assert.ok(!b.isDisjointFrom(a));

  const big = new Set();
  for (let i = 0; i < 5000; i++) big.add(i);
  for (let i = 0; i < 5000; i += 2) big.delete(i);
  const small = new Set([1, 2, 3, 4999]);
  assert.deepEqual([...big.intersection(small)], [1, 3, 4999]);
  assert.deepEqual([...small.difference(big)], [2]);
  assert.equal(big.union(small).size, 2501);
  assert.equal([...big.union(small)].at(-1), 2);
  assert.equal(big.difference(small).size, 2497);
  assert.equal(big.symmetricDifference(small).size, 2498);

  const like = { size: 2, has: (v) => v === 1 || v === 'x', keys: () => [1, 'x'][Symbol.iterator]() };
  assert.deepEqual([...a.intersection(like)], [1, 'x']);
  assert.deepEqual([...a.difference(like)], [2, 3, 4, 5, 0]);
  assert.ok(new Set([1]).isSubsetOf(like));

  const s = new Set([1, 2, 3, 4]);
  const shrinking = { size: 10, has(v) { s.delete(v + 1); return true; }, keys: () => [][Symbol.iterator]() };
  assert.deepEqual([...s.intersection(shrinking)], [1, 3]);
}

function structuredCloneRoundTrip() {
  const obj = { tag: 'o' };
  const m = new Map([[1, 'one'], ['two', 2], [obj, obj], [NaN, [1, 2]], [10n ** 30n, 'big']]);
  for (let i = 0; i < 300; i++) m.set('k' + i, i);
  for (let i = 0; i < 300; i += 3) m.delete('k' + i);
  const s = new Set([1, 'a', obj, m]);
  m.set('self', m);
  m.set('set', s);
  const c = structuredClone({ m, s });
  assert.deepEqual([...c.m.keys()].length, m.size);
  assert.deepEqual([...c.m.keys()].map(String), [...m.keys()].map(String));
  const co = [...c.m.keys()][2];
  assert.deepEqual(co, obj);
  assert.equal(c.m.get(co), co);
  assert.equal(c.m.get('self'), c.m);
  assert.equal(c.m.get('set'), c.s);
  assert.ok(c.s.has(co));
  assert.ok(c.s.has(c.m));
  assert.deepEqual(c.m.get(NaN), [1, 2]);
  assert.equal(c.m.get(10n ** 30n), 'big');
  assert.equal(c.m.get('k1'), 1);
  assert.equal(c.m.has('k0'), false);
}

function groupByAndConstructors() {
  const g = Map.groupBy([1, 2, 3, 4, 5, 6, -0], (x) => (Object.is(x, -0) ? 0 : x % 3));
  assert.deepEqual([...g.keys()], [1, 2, 0]);
  assert.deepEqual(g.get(0), [3, 6, -0]);
  const strs = Map.groupBy(['aa', 'b', 'cc', 'd'], (x) => 'len' + x.length);
  assert.deepEqual([...strs], [['len2', ['aa', 'cc']], ['len1', ['b', 'd']]]);

  const src = new Map([[1, 'a'], [2, 'b']]);
  const copy = new Map(src);
  src.set(3, 'c');
  assert.deepEqual([...copy], [[1, 'a'], [2, 'b']]);
  class M extends Map {
    constructor(it) { super(it); this.extra = 1; }
    get(k) { return 'sub:' + super.get(k); }
  }
  const sub = new M([[1, 2]]);
  assert.equal(sub.get(1), 'sub:2');
  assert.equal(sub.size, 1);
  assert.ok(sub instanceof Map);
  class S extends Set {}
  const ss = new S([1, 1, 2]);
  assert.equal(ss.size, 2);
  assert.deepEqual([...ss], [1, 2]);
  assert.deepEqual([...new Set('hello')], ['h', 'e', 'l', 'o']);
}

function upsertFamily() {
  if (typeof Map.prototype.getOrInsert !== 'function') return;
  const m = new Map();
  assert.equal(m.getOrInsert(-0, 'a'), 'a');
  assert.ok(Object.is([...m.keys()][0], 0));
  assert.equal(m.getOrInsert(0, 'b'), 'a');
  assert.equal(m.getOrInsertComputed('k', (k) => { m.set('k', 'inner'); return k + '!'; }), 'k!');
  assert.deepEqual([...m], [[0, 'a'], ['k', 'k!']]);
}

function freshValuesSurviveCollections() {
  function* gen() { for (let i = 0; i < 40000; i++) yield [{ i }, { v: i }]; }
  const m = new Map(gen());
  const s = new Set((function* () { for (let i = 0; i < 40000; i++) yield { i }; })());
  const g = Map.groupBy(Array.from({ length: 40000 }, (_, i) => i), (i) => ({ k: i % 7 }));
  const src = new Map();
  for (let i = 0; i < 40000; i++) src.set(i, { i, pad: [i] });
  const cloned = structuredClone(src);
  const { serialize, deserialize } = require('node:v8');
  const revived = deserialize(serialize(src));
  const junk = [];
  for (let r = 0; r < 4; r++) for (let i = 0; i < 40000; i++) junk.push({ r, i, pad: [i] });
  let n = 0;
  for (const [k, v] of m) { assert.equal(k.i, v.v); assert.equal(k.i, n++); }
  n = 0;
  for (const o of s) assert.equal(o.i, n++);
  let total = 0;
  for (const [k, v] of g) { assert.equal(typeof k.k, 'number'); total += v.length; }
  assert.equal(total, 40000);
  for (const copy of [cloned, revived]) {
    n = 0;
    for (const [k, v] of copy) { assert.equal(k, n++); assert.equal(v.i, k); assert.equal(v.pad[0], k); }
    assert.equal(n, 40000);
  }
  assert.equal(junk.length, 160000);
}

function hotIteration() {
  function entries(m) {
    let out = '';
    for (const [k, v] of m) {
      out += k + ':' + v + ',';
      if (k === 2) m.delete(3);
      if (k === 4) m.set(9, 'nine');
      if (k === 9) m.delete(1);
    }
    return out;
  }
  function setEntries(s) {
    let out = '';
    for (const [a, b] of s.entries()) {
      out += a + '=' + b + ',';
      if (a === 1) { s.delete(2); s.add(2); }
    }
    return out;
  }
  function mixed(m) {
    let sum = 0;
    for (const v of m.values()) sum += v;
    for (const k of m.keys()) sum += k;
    for (const e of m.entries()) sum += e[0] * e[1];
    return sum;
  }
  function clearing(m) {
    let seen = 0;
    for (const [k] of m) {
      seen++;
      if (k === 1) { m.clear(); m.set(100, 1); }
    }
    return seen;
  }
  const holey = new Map();
  for (let i = 0; i < 40; i++) holey.set(i, i);
  for (let i = 0; i < 40; i += 3) holey.delete(i);
  let expect = 0;
  for (const [k, v] of holey) expect += k + v + k * v;
  for (let r = 0; r < 400; r++) {
    assert.equal(entries(new Map([[1, 'a'], [2, 'b'], [3, 'c'], [4, 'd']])), '1:a,2:b,4:d,9:nine,');
    assert.equal(setEntries(new Set([1, 2, 3])), '1=1,3=3,2=2,');
    assert.equal(mixed(holey), expect);
    assert.equal(clearing(new Map([[1, 1], [2, 2], [3, 3]])), 2);
  }
}

function weakSetConstructorPrunes() {
  const ws = new WeakSet((function* () { for (let i = 0; i < 40000; i++) yield { i }; })());
  let stale = 0;
  for (let r = 0; r < 4; r++) {
    const fresh = [];
    for (let i = 0; i < 40000; i++) fresh.push({ r, i });
    for (const o of fresh) if (ws.has(o)) stale++;
  }
  assert.equal(stale, 0);
}

function emptyCollections() {
  const m = new Map();
  const it = m.entries();
  m.set('a', 1);
  m.set('b', 2);
  assert.deepEqual([...it], [['a', 1], ['b', 2]]);

  const s = new Set();
  const sit = s.values();
  s.add(1);
  assert.deepEqual([...sit], [1]);

  const quiet = new Map();
  const done = quiet.keys();
  assert.deepEqual(done.next(), { value: undefined, done: true });
  quiet.set('x', 1);
  assert.deepEqual(done.next(), { value: undefined, done: true });

  const cleared = new Map();
  cleared.clear();
  assert.equal(cleared.delete('missing'), false);
  assert.equal(cleared.size, 0);
  cleared.set('k', 'v');
  assert.equal(cleared.get('k'), 'v');

  if (typeof Map.prototype.upsert === 'function') {
    const self = new Map();
    assert.equal(self.upsert('k', v => v + 1, () => { self.set('other', 0); return 1; }), 1);
    assert.deepEqual([...self], [['other', 0], ['k', 1]]);
  }

  if (typeof Map.prototype.getOrInsertComputed === 'function') {
    const computed = new Map();
    assert.equal(computed.getOrInsertComputed('k', () => { computed.set('first', 1); return 2; }), 2);
    assert.deepEqual([...computed], [['first', 1], ['k', 2]]);

    const plain = new Map();
    assert.equal(plain.getOrInsert('k', 3), 3);
    assert.equal(plain.get('k'), 3);
  }

  let calls = 0;
  new Map().forEach(() => calls++);
  new Set().forEach(() => calls++);
  assert.equal(calls, 0);

  assert.equal(structuredClone(new Map()).size, 0);
  assert.equal(structuredClone(new Set()).size, 0);
  assert.equal(require('node:util').inspect(new Map()), 'Map(0) {}');
  assert.deepEqual([...new Set().union(new Set([1]))], [1]);
  assert.deepEqual([...new Set([1]).union(new Set())], [1]);
  assert.equal(new Set().isSubsetOf(new Set([1])), true);
  assert.equal(new Set().intersection(new Set([1])).size, 0);

  const many = [];
  for (let i = 0; i < 20000; i++) many.push(i & 1 ? new Map() : new Set());
  for (let i = 0; i < 200000; i++) ({ i });
  for (let i = 0; i < many.length; i += 97) {
    const c = many[i];
    if (c instanceof Map) c.set(i, i); else c.add(i);
    assert.equal(c.size, 1);
  }
}

weakSetConstructorPrunes();
emptyCollections();
sameValueZero();
hotIteration();
insertionOrder();
mutationDuringIteration();
compactionWithLiveIterators();
largeGrowShrink();
setMethods();
structuredCloneRoundTrip();
groupByAndConstructors();
upsertFamily();
freshValuesSurviveCollections();
console.log('ok');
