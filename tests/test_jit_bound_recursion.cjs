const assert = require('node:assert');

const ITER = 3000;

const o = { k: 2 };
function f(n) { if (n <= 0) return this.k; return 1 + bf(n - 1); }
const bf = f.bind(o);
let r1 = 0;
for (let i = 0; i < ITER; i++) r1 += bf(i % 50);
assert.strictEqual(r1, 79500);

function g(acc, n) { if (n <= 0) return acc; return bg(n - 1) + 1; }
const bg = g.bind(null, 7);
let r2 = 0;
for (let i = 0; i < ITER; i++) r2 += bg(i % 50);
assert.strictEqual(r2, 94500);

function both(a, n) { if (n <= 0) return this.k * 10 + a; return 1 + bboth(n - 1); }
const bboth = both.bind({ k: 3 }, 4);
let r3 = 0;
for (let i = 0; i < ITER; i++) r3 += bboth(i % 40);
assert.strictEqual(r3, ITER * 34 + 58500);

function deep(a, b, n) { if (n <= 0) return this.k + a * 100 + b * 10; return 1 + bbdeep(n - 1); }
const bdeep = deep.bind({ k: 5 }, 1);
const bbdeep = bdeep.bind({ k: 9 }, 2);
let r4 = 0;
for (let i = 0; i < ITER; i++) r4 += bbdeep(i % 30);
assert.strictEqual(r4, ITER * 125 + 43500);
assert.strictEqual(bdeep(3, 0), 135);

function Tree(depth) {
  this.depth = depth;
  this.self = this instanceof Tree;
  this.child = depth > 0 ? new BTree(depth - 1) : null;
}
const BTree = Tree.bind({ k: 'ignored' });
let r5 = 0;
for (let i = 0; i < ITER; i++) {
  let t = new BTree(i % 10);
  while (t) {
    assert.strictEqual(t.self, true);
    r5 += t.depth;
    t = t.child;
  }
}
assert.strictEqual(r5, 49500);

function Pair(tag, depth) {
  this.tag = tag;
  this.depth = depth;
  this.next = depth > 0 ? new BPair(depth - 1) : null;
}
const BPair = Pair.bind(null, 'p');
let r6 = 0;
for (let i = 0; i < ITER; i++) {
  let p = new BPair(i % 8);
  while (p) {
    assert.strictEqual(p.tag, 'p');
    r6 += p.depth;
    p = p.next;
  }
}
assert.strictEqual(r6, 31500);

const arrowHost = {
  k: 11,
  run(count) {
    const a = (n) => (n <= 0 ? this.k : 1 + ba(n - 1));
    const ba = a.bind({ k: 99 });
    const c = (x, n) => (n <= 0 ? this.k + x : 1 + bc(n - 1));
    const bc = c.bind({ k: 77 }, 5);
    let sum = 0;
    for (let i = 0; i < count; i++) sum += ba(i % 20) + bc(i % 20);
    return sum;
  }
};
assert.strictEqual(arrowHost.run(ITER), ITER * 27 + 57000);

function args3(a, b, c) {
  if (a <= 0) return [b, c];
  return bargs(a - 1, b, c, 'extra');
}
const bargs = args3.bind(null);
function args3b(a, b, c) {
  if (!(b > 0)) return [a, c];
  return bargs2(b - 1);
}
const bargs2 = args3b.bind(null, 'A');
for (let i = 0; i < ITER; i++) {
  assert.deepStrictEqual(bargs(i % 12, i), [i, undefined]);
  assert.deepStrictEqual(bargs(i % 12), [undefined, undefined]);
  assert.deepStrictEqual(bargs2(i % 12, 'c', 'd'), ['A', i % 12 === 0 ? 'c' : undefined]);
  assert.deepStrictEqual(bargs2(), ['A', undefined]);
}

function mixed(n) {
  if (n <= 0) return this.k;
  return n + bmixed(n - 1);
}
const mixedThis = { k: 1 };
const bmixed = mixed.bind(mixedThis);
let r7 = 0;
for (let i = 0; i < ITER; i++) {
  if (i === ITER - 500) mixedThis.k = 'x';
  if (i === ITER - 250) mixedThis.k = 1.5;
  const v = bmixed(i % 25);
  r7 += typeof v === 'string' ? v.length : v;
}
function mixedRef(n, k) { return n <= 0 ? k : n + mixedRef(n - 1, k); }
let expect7 = 0;
for (let i = 0; i < ITER; i++) {
  const k = i >= ITER - 250 ? 1.5 : i >= ITER - 500 ? 'x' : 1;
  const v = mixedRef(i % 25, k);
  expect7 += typeof v === 'string' ? v.length : v;
}
assert.strictEqual(r7, expect7);

function flip(acc, n) {
  if (n <= 0) return acc + this.k;
  const r = bflip(n - 1);
  return n === flipAt ? r + 'z' : r;
}
let flipAt = -1;
const bflip = flip.bind({ k: 1 }, 10);
for (let i = 0; i < ITER; i++) {
  if (i === ITER - 500) flipAt = 5;
  const n = i % 12;
  assert.strictEqual(bflip(n), flipAt === 5 && n >= 5 ? '11z' : 11);
}

const methodTarget = { k: 4 };
function meth(n) { if (n <= 0) return this.k; return 1 + holder.m(n - 1); }
const holder = { k: 1000, m: meth.bind(methodTarget) };
let r8 = 0;
for (let i = 0; i < ITER; i++) r8 += holder.m(i % 30);
assert.strictEqual(r8, ITER * 4 + 43500);

function late(n) {
  if (n <= 0) return this === undefined || this === globalThis ? 0 : this.k;
  return 1 + lateRef(n - 1);
}
let lateRef = late;
let r9 = 0;
for (let i = 0; i < ITER; i++) r9 += lateRef(i % 20);
assert.strictEqual(r9, 28500);
lateRef = late.bind({ k: 6 });
let r10 = 0;
for (let i = 0; i < ITER; i++) r10 += lateRef(i % 20);
assert.strictEqual(r10, ITER * 6 + 28500);

function lateArgs(a, n) {
  if (n <= 0) return a;
  return 1 + lateArgsRef(n - 1);
}
let lateArgsRef = (n) => lateArgs(0, n);
let r11 = 0;
for (let i = 0; i < ITER; i++) r11 += lateArgsRef(i % 20);
assert.strictEqual(r11, 28500);
lateArgsRef = lateArgs.bind(null, 3);
let r12 = 0;
for (let i = 0; i < ITER; i++) r12 += lateArgsRef(i % 20);
assert.strictEqual(r12, ITER * 3 + 28500);

function makeUpval(k) {
  let b;
  function inner(n) { return n <= 0 ? this.k : 1 + b(n - 1); }
  b = inner.bind({ k });
  return b;
}
const upA = makeUpval(2);
const upB = makeUpval(5);
let r13 = 0;
for (let i = 0; i < ITER; i++) r13 += upA(i % 15) + upB(i % 15);
assert.strictEqual(r13, ITER * 7 + 42000);

const named = function rec(n) { return n <= 0 ? (this && this.k) : 1 + rec.call(this, n - 1); };
const bnamed = named.bind({ k: 8 });
let r14 = 0;
for (let i = 0; i < ITER; i++) r14 += bnamed(i % 15);
assert.strictEqual(r14, ITER * 8 + 21000);

console.log('jit:bound-recursion:ok');
