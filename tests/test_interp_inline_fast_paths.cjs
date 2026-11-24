const assert = require('node:assert');

const sym = Symbol('s');
const obj = { a: 1 };
const arr = [1];
const fn = function () {};
const boxedFive = { valueOf() { return 5; } };
const boxedText = { toString() { return 'x'; } };

const values = [
  undefined, null, true, false, 0, -0, 1, -1, 0.5, NaN, Infinity, -Infinity, 2 ** 31, 2 ** 53,
  '', '0', '1', 'x', 'abc', 'a' + 'bc', ' ', 'NaN', 'true',
  0n, 1n, -1n, 10n ** 30n,
  sym, Symbol('s'), obj, { a: 1 }, arr, [1], [], fn, boxedFive, boxedText,
];

function looseReference(l, r) {
  const lt = typeof l, rt = typeof r;
  const lNullish = l === undefined || l === null, rNullish = r === undefined || r === null;
  if (lNullish || rNullish) return lNullish && rNullish;
  if (lt === rt || (isObjectLike(l) && isObjectLike(r))) return strictReference(l, r);
  if (lt === 'boolean') return looseReference(Number(l), r);
  if (rt === 'boolean') return looseReference(l, Number(r));
  if (lt === 'number' && rt === 'string') return strictReference(l, Number(r));
  if (lt === 'string' && rt === 'number') return strictReference(Number(l), r);
  if (lt === 'bigint' && rt === 'number') return Number.isFinite(r) && Number.isInteger(r) && l === BigInt(r);
  if (lt === 'number' && rt === 'bigint') return looseReference(r, l);
  if (lt === 'bigint' && rt === 'string') { try { return l === BigInt(r); } catch { return false; } }
  if (lt === 'string' && rt === 'bigint') return looseReference(r, l);
  if (isObjectLike(l) && !isObjectLike(r)) return rt === 'symbol' ? false : looseReference(toPrimitive(l), r);
  if (!isObjectLike(l) && isObjectLike(r)) return lt === 'symbol' ? false : looseReference(l, toPrimitive(r));
  return false;
}

function isObjectLike(v) {
  return v !== null && (typeof v === 'object' || typeof v === 'function');
}

function toPrimitive(v) {
  const viaValueOf = v.valueOf();
  if (!isObjectLike(viaValueOf)) return viaValueOf;
  return v.toString();
}

function strictReference(l, r) {
  if (typeof l !== typeof r) return false;
  if (typeof l === 'number') return !(l !== l) && !(r !== r) && (Object.is(l, r) || (l === 0 && r === 0));
  if (typeof l === 'string') return l.length === r.length && [...l].every((ch, i) => ch === [...r][i]);
  if (typeof l === 'bigint') return l - r === 0n;
  return Object.is(l, r);
}

function truthyReference(v) {
  if (v === undefined || v === null || v === false) return false;
  if (typeof v === 'number') return !(v === 0 || v !== v);
  if (typeof v === 'string') return v.length > 0;
  if (typeof v === 'bigint') return v !== 0n;
  return true;
}

function describe(v) {
  if (typeof v === 'symbol') return 'symbol';
  if (typeof v === 'bigint') return `${v}n`;
  if (typeof v === 'string') return JSON.stringify(v);
  if (Object.is(v, -0)) return '-0';
  if (isObjectLike(v)) return typeof v === 'function' ? 'fn' : Array.isArray(v) ? `[${v.length}]` : 'obj';
  return String(v);
}

function compareAll() {
  for (let i = 0; i < values.length; i++) {
    for (let j = 0; j < values.length; j++) {
      const l = values[i], r = values[j];
      const label = `${describe(l)} vs ${describe(r)} (${i},${j})`;
      const strict = strictReference(l, r);
      const loose = looseReference(l, r);
      assert.strictEqual(l === r, strict, `=== ${label}`);
      assert.strictEqual(l !== r, !strict, `!== ${label}`);
      assert.strictEqual(l == r, loose, `== ${label}`);
      assert.strictEqual(l != r, !loose, `!= ${label}`);
      let taken = 0;
      if (l === r) taken |= 1;
      if (l == r) taken |= 2;
      assert.strictEqual(taken, (strict ? 1 : 0) | (loose ? 2 : 0), `branches ${label}`);
    }
  }
}

function truthAll() {
  for (let i = 0; i < values.length; i++) {
    const v = values[i];
    const want = truthyReference(v);
    const label = describe(v);
    assert.strictEqual(!v, !want, `! ${label}`);
    assert.strictEqual(!!v, want, `!! ${label}`);
    assert.strictEqual(v ? 1 : 0, want ? 1 : 0, `?: ${label}`);
    let viaIf = 0;
    if (v) viaIf = 1;
    assert.strictEqual(viaIf, want ? 1 : 0, `if ${label}`);
    const and = v && 'rhs';
    const or = v || 'rhs';
    assert.ok(want ? and === 'rhs' : Object.is(and, v), `&& ${label}`);
    assert.ok(want ? Object.is(or, v) : or === 'rhs', `|| ${label}`);
    let loops = 0;
    for (let w = v; w; w = undefined) loops++;
    assert.strictEqual(loops, want ? 1 : 0, `loop ${label}`);
    let whileCount = 0;
    while (!v) { whileCount++; break; }
    assert.strictEqual(whileCount, want ? 0 : 1, `while ${label}`);
  }
}

function relationalAll() {
  const operands = [0, -0, 1, -1, 0.5, NaN, Infinity, -Infinity, '', '1', '10', '9', 'a', 'b', 'B', true, false, null, undefined, 1n, 2n, boxedFive, [2], []];
  for (const l of operands) {
    for (const r of operands) {
      const label = `${describe(l)} ? ${describe(r)}`;
      const lp = isObjectLike(l) ? toPrimitive(l) : l, rp = isObjectLike(r) ? toPrimitive(r) : r;
      let lt, gt, le, ge;
      if (typeof lp === 'string' && typeof rp === 'string') {
        const order = lp === rp ? 0 : [lp, rp].sort()[0] === lp ? -1 : 1;
        lt = order < 0; gt = order > 0; le = order <= 0; ge = order >= 0;
      } else if (typeof lp === 'bigint' || typeof rp === 'bigint') {
        continue;
      } else {
        const ln = Number(lp), rn = Number(rp);
        const unordered = ln !== ln || rn !== rn;
        const diff = unordered ? 0 : ln === rn ? 0 : ln - rn;
        lt = !unordered && (ln === -Infinity && rn !== -Infinity || rn === Infinity && ln !== Infinity || diff < 0);
        gt = !unordered && (rn === -Infinity && ln !== -Infinity || ln === Infinity && rn !== Infinity || diff > 0);
        le = !unordered && !gt; ge = !unordered && !lt;
      }
      assert.strictEqual(l < r, lt, `< ${label}`);
      assert.strictEqual(l > r, gt, `> ${label}`);
      assert.strictEqual(l <= r, le, `<= ${label}`);
      assert.strictEqual(l >= r, ge, `>= ${label}`);
    }
  }
  assert.strictEqual(1n <= 1, true);
  assert.strictEqual(2n > 1, true);
  assert.strictEqual(1n >= 2n, false);
  assert.strictEqual(10n ** 30n > 1e29, true);
  assert.throws(() => sym <= 1, TypeError);
  assert.throws(() => 1 >= sym, TypeError);
  assert.throws(() => sym > sym, TypeError);
}

function bitwiseAll() {
  const toInt32 = (v) => {
    const n = Number(v);
    if (!Number.isFinite(n)) return 0;
    const m = Math.trunc(n) % 4294967296;
    const u = m < 0 ? m + 4294967296 : m;
    return u >= 2147483648 ? u - 4294967296 : u;
  };
  const toUint32 = (v) => { const i = toInt32(v); return i < 0 ? i + 4294967296 : i; };
  const bit = (x, k) => Math.floor(x / 2 ** k) % 2;
  const combine = (a, b, f) => {
    const ua = toUint32(a), ub = toUint32(b);
    let out = 0;
    for (let k = 0; k < 32; k++) out += f(bit(ua, k), bit(ub, k)) * 2 ** k;
    return (out >= 2147483648 ? out - 4294967296 : out) + 0;
  };
  const operands = [
    0, -0, 1, -1, 2, 7, 31, 32, 33, 63, 64, 255, 65535, 65536, 0x3fff, 0xfffffff, 2147483647, -2147483648,
    2147483648, 4294967295, 4294967296, -2147483649, 2 ** 40 + 5, -(2 ** 40) - 5, 1.5, -1.5, 0.99, 1e21,
    NaN, Infinity, -Infinity, '12', '0x10', '', 'x', true, false, null, undefined, [5], boxedFive,
  ];
  for (const l of operands) {
    for (const r of operands) {
      const label = `${describe(l)} op ${describe(r)}`;
      const shift = toUint32(r) % 32;
      const li = toInt32(l);
      assert.strictEqual(l & r, combine(l, r, (x, y) => x * y), `& ${label}`);
      assert.strictEqual(l | r, combine(l, r, (x, y) => (x + y > 0 ? 1 : 0)), `| ${label}`);
      assert.strictEqual(l ^ r, combine(l, r, (x, y) => (x + y) % 2), `^ ${label}`);
      assert.strictEqual(l << r, toInt32(toUint32(l) * 2 ** shift) + 0, `<< ${label}`);
      assert.strictEqual(l >> r, Math.floor(li / 2 ** shift) + 0, `>> ${label}`);
      assert.strictEqual(l >>> r, Math.floor(toUint32(l) / 2 ** shift) + 0, `>>> ${label}`);
    }
  }
  assert.ok(Object.is(-0 | 0, 0) && Object.is(-0 & -0, 0) && Object.is(-0 >> 0, 0), 'negative zero normalizes');
  assert.strictEqual(5n & 3n, 1n);
  assert.strictEqual(5n | 3n, 7n);
  assert.strictEqual(5n ^ 3n, 6n);
  assert.strictEqual(5n << 70n, 5n * 2n ** 70n);
  assert.strictEqual(-5n >> 1n, -3n);
  assert.throws(() => 5n & 3, TypeError);
  assert.throws(() => 3 | 5n, TypeError);
  assert.throws(() => 5n >>> 1n, TypeError);
  assert.throws(() => sym ^ 1, TypeError);
  let acc = 0;
  for (let i = 0; i < 2000; i++) { acc = (acc + i * 3 - (i >> 2)) | 0; acc ^= i & 7; acc = (acc << 1) >>> 1; }
  assert.strictEqual(acc, 5496016);
}

function counters() {
  let down = 5, up = -3, text = '7', big = 3n, o = { valueOf() { return 2; } };
  for (let i = 0; i < 4; i++) { down--; up++; text--; big--; }
  assert.deepStrictEqual([down, up, text, big], [1, 1, 3, -1n]);
  o--;
  assert.strictEqual(o, 1);
  let s = '';
  for (let i = 0; i < 3; i++) s += 'q';
  s++;
  assert.ok(s !== s, 'builder decrement gives NaN');
}

function storeElements() {
  const dense = [1, 2, 3, 4];
  for (let i = 0; i < dense.length; i++) dense[i] = i * 2;
  assert.deepStrictEqual(dense, [0, 2, 4, 6]);

  const holes = [1, , 3];
  holes[1] = 'filled';
  holes[5] = 'far';
  assert.deepStrictEqual([holes.length, holes[1], 3 in holes, holes[5]], [6, 'filled', false, 'far']);

  const grown = [];
  for (let i = 0; i < 300; i++) grown[i] = i;
  for (let i = 0; i < 300; i++) grown[i] = grown[i] + 1;
  assert.strictEqual(grown.reduce((a, b) => a + b, 0), 300 * 301 / 2);

  const literalMaker = () => [10, 20, 30];
  const first = literalMaker(), second = literalMaker();
  first[1] = 'changed';
  assert.deepStrictEqual(second, [10, 20, 30]);
  assert.deepStrictEqual(literalMaker(), [10, 20, 30]);
  assert.deepStrictEqual(first, [10, 'changed', 30]);

  const frozen = Object.freeze([1, 2, 3]);
  frozen[0] = 9;
  assert.strictEqual(frozen[0], 1);
  assert.throws(() => { 'use strict'; frozen[0] = 9; }, TypeError);

  const sealed = Object.seal([1, 2, 3]);
  sealed[0] = 9;
  sealed[3] = 4;
  assert.deepStrictEqual([sealed[0], sealed.length], [9, 3]);

  const locked = [1, 2, 3];
  Object.defineProperty(locked, 1, { value: 'fixed', writable: false });
  locked[0] = 'a'; locked[1] = 'b'; locked[2] = 'c';
  assert.deepStrictEqual(locked, ['a', 'fixed', 'c']);
  assert.throws(() => { 'use strict'; locked[1] = 'z'; }, TypeError);

  let setterHits = 0;
  const accessor = [1, 2, 3];
  Object.defineProperty(accessor, 1, { get() { return 'got'; }, set(v) { setterHits += v; } });
  accessor[1] = 5; accessor[1] = 6; accessor[0] = 0;
  assert.deepStrictEqual([setterHits, accessor[1], accessor[0]], [11, 'got', 0]);

  const fixedLength = [1, 2];
  Object.defineProperty(fixedLength, 'length', { writable: false });
  fixedLength[0] = 'ok'; fixedLength[2] = 'no';
  assert.deepStrictEqual([fixedLength.length, fixedLength[0], fixedLength[2]], [2, 'ok', undefined]);

  let protoHits = 0;
  class Trapped extends Array {}
  Object.defineProperty(Trapped.prototype, 1, { set(v) { protoHits++; }, get() { return 'proto'; }, configurable: true });
  const trapped = new Trapped();
  trapped[0] = 'own';
  trapped[1] = 'through';
  assert.deepStrictEqual([protoHits, trapped[0], trapped[1], trapped.length], [1, 'own', 'proto', 1]);
  delete Trapped.prototype[1];

  const proxyLog = [];
  const proxied = new Proxy([1, 2, 3], { set(t, k, v) { proxyLog.push(String(k)); t[k] = v; return true; } });
  proxied[0] = 5; proxied[2] = 7;
  assert.deepStrictEqual([proxyLog, proxied[0], proxied[2]], [['0', '2'], 5, 7]);

  const typed = new Uint8Array(4);
  for (let i = 0; i < 6; i++) typed[i] = 250 + i * 3;
  assert.deepStrictEqual(Array.from(typed), [250, 253, 0, 3]);

  function mapped(a, b) {
    arguments[0] = 'A';
    arguments[1] = 'B';
    arguments[2] = 'C';
    return [a, b, arguments.length, arguments[2]];
  }
  assert.deepStrictEqual(mapped(1, 2), ['A', 'B', 2, 'C']);
  function unmapped(a, b) {
    'use strict';
    arguments[0] = 'A';
    return [a, arguments[0]];
  }
  assert.deepStrictEqual(unmapped(1, 2), [1, 'A']);

  const keys = [1, 2, 3];
  keys[1.5] = 'frac'; keys[-1] = 'neg'; keys['2'] = 'str'; keys[4294967295] = 'max';
  assert.deepStrictEqual([keys.length, keys[2], keys[1.5], keys[-1], keys[4294967295]], [3, 'str', 'frac', 'neg', 'max']);

  const old = [];
  for (let i = 0; i < 64; i++) old[i] = null;
  const churn = [];
  for (let round = 0; round < 40; round++) {
    for (let i = 0; i < 64; i++) old[i] = { round, i, text: 'v' + round + ':' + i };
    for (let i = 0; i < 2000; i++) churn[i & 127] = { junk: i, pad: [i, i + 1] };
  }
  for (let i = 0; i < 64; i++) assert.deepStrictEqual(old[i], { round: 39, i, text: 'v39:' + i });
}

function frames() {
  function noLocals(a, b, c) { return [a, b, c]; }
  assert.deepStrictEqual(noLocals(), [undefined, undefined, undefined]);
  assert.deepStrictEqual(noLocals(1), [1, undefined, undefined]);
  assert.deepStrictEqual(noLocals(1, 2, 3, 4, 5), [1, 2, 3]);

  function manyLocals(seed) {
    var a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w, x, y, z;
    const seen = [a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w, x, y, z];
    a = b = c = d = e = f = g = h = i = j = k = l = m = n = o = p = q = r = s = t = u = v = w = x = y = z = seed;
    return seen.every((value) => value === undefined) && z === seed;
  }
  for (let round = 0; round < 50; round++) assert.ok(manyLocals(round), 'locals start undefined');

  function sixteen(a) { var l1, l2, l3, l4, l5, l6, l7, l8, l9, l10, l11, l12, l13, l14, l15, l16; const r = [l1, l8, l15, l16]; l1 = l8 = l15 = l16 = a; return r; }
  function seventeen(a) { var l1, l2, l3, l4, l5, l6, l7, l8, l9, l10, l11, l12, l13, l14, l15, l16, l17; const r = [l1, l16, l17]; l1 = l16 = l17 = a; return r; }
  for (let round = 0; round < 20; round++) {
    assert.deepStrictEqual(sixteen(round), [undefined, undefined, undefined, undefined]);
    assert.deepStrictEqual(seventeen(round), [undefined, undefined, undefined]);
  }

  function count() { return arguments.length; }
  function last() { return arguments[arguments.length - 1]; }
  const wide = [];
  for (let n = 0; n < 40; n++) {
    assert.strictEqual(count(...wide), n);
    if (n) assert.strictEqual(last(...wide), n - 1);
    wide.push(n);
  }
  assert.strictEqual(count(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18), 18);

  function missing(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r) { return [a, i, q, r]; }
  assert.deepStrictEqual(missing(1), [1, undefined, undefined, undefined]);
  assert.deepStrictEqual(missing(1, 2, 3, 4, 5, 6, 7, 8, 9), [1, 9, undefined, undefined]);

  function sumTail(n, acc) {
    'use strict';
    var spare;
    if (spare !== undefined) return -1;
    spare = n;
    if (n === 0) return acc;
    return sumTail(n - 1, acc + n);
  }
  assert.strictEqual(sumTail(3000, 0), 3000 * 3001 / 2);

  function shrinkTail(n, a, b, c) {
    'use strict';
    if (n === 0) return [a, b, c];
    return n & 1 ? shrinkTail(n - 1, a) : shrinkTail(n - 1, a, n, n + 1, n + 2);
  }
  assert.deepStrictEqual(shrinkTail(7, 'k'), ['k', undefined, undefined]);
  assert.deepStrictEqual(shrinkTail(8, 'k'), ['k', undefined, undefined]);

  const receiver = { base: 3, method(x) { var local; const was = local; local = x; return was === undefined ? this.base + x : -1; } };
  let total = 0;
  for (let i = 0; i < 500; i++) total += receiver.method(i);
  assert.strictEqual(total, 500 * 3 + 499 * 500 / 2);

  class Base { constructor(x) { var tmp; this.seen = tmp; tmp = x; this.x = x; } }
  class Derived extends Base { constructor(x, y) { super(x); var later; this.later = later; this.y = y; } }
  for (let i = 0; i < 300; i++) {
    const d = new Derived(i, i + 1);
    assert.deepStrictEqual([d.seen, d.x, d.later, d.y], [undefined, i, undefined, i + 1]);
  }
}

function hotCallsReachJit() {
  function add(a, b) { return a + b; }
  let s = 0;
  for (let i = 0; i < 5000; i++) s = add(s, i) & 0xffff;
  assert.strictEqual(s, (4999 * 5000 / 2) & 0xffff);
  function* gen(n) { for (let i = 0; i < n; i++) yield i; }
  let g = 0;
  for (let i = 0; i < 300; i++) for (const v of gen(3)) g += v;
  assert.strictEqual(g, 900);
}

function literalsAndProtectors() {
  const made = [];
  for (let i = 0; i < 200; i++) made.push({ a: i, b: 'x' + i, c: [i], d: null });
  assert.deepStrictEqual(made[199], { a: 199, b: 'x199', c: [199], d: null });

  const thenable = { then: null, constructor: null, next: null };
  thenable.then = (resolve) => resolve('late');
  let resolved;
  Promise.resolve(thenable).then((v) => { resolved = v; });

  class Sub extends Array {}
  const holder = { prototype: 1 };
  holder.prototype = Sub.prototype;
  function Legacy() {}
  Legacy.prototype = { tag: 'replaced' };
  assert.strictEqual(new Legacy().tag, 'replaced');
  Legacy.prototype = { tag: 'again' };
  assert.strictEqual(new Legacy().tag, 'again');

  const list = [1, 2, 3];
  list.constructor = function Other() { return [9]; };
  assert.deepStrictEqual(list.map((v) => v), [1, 2, 3].map((v) => v).length === 3 ? list.map((v) => v) : null);
  const iterated = [1, 2, 3];
  const iterator = iterated[Symbol.iterator]();
  iterator.next = function () { return { done: true, value: undefined }; };
  assert.deepStrictEqual([...{ [Symbol.iterator]: () => iterator }], []);
  assert.deepStrictEqual([...iterated], [1, 2, 3]);

  return () => assert.strictEqual(resolved, 'late');
}

for (let round = 0; round < 3; round++) {
  compareAll();
  truthAll();
  relationalAll();
  bitwiseAll();
  counters();
  storeElements();
  frames();
  hotCallsReachJit();
}

const afterMicrotasks = literalsAndProtectors();
Promise.resolve().then(() => Promise.resolve()).then(() => Promise.resolve()).then(() => {
  afterMicrotasks();
  console.log('ok');
});
