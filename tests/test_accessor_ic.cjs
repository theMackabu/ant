const assert = require('node:assert');

let calls = 0;
class Point { constructor(v) { this.v = v; } get x() { calls++; return this.v; } }
const readX = o => o.x;
let sum = 0;
for (let i = 0; i < 30000; i++) sum += readX(new Point(i & 7));
assert.deepStrictEqual([sum, calls], [105000, 30000], 'a cached getter runs exactly once per read');

const thrower = { get boom() { throw new RangeError('b'); } };
const readBoom = o => o.boom;
let caught = 0;
for (let i = 0; i < 5000; i++) { try { readBoom(thrower); } catch (e) { if (e instanceof RangeError) caught++; } }
assert.strictEqual(caught, 5000, 'a throwing getter throws on every read');

const morph = { n: 0, get g() { this.n++; if (this.n === 3000) Object.defineProperty(morph, 'g', { value: 'data', configurable: true }); return this.n; } };
const readG = o => o.g;
let last;
for (let i = 0; i < 6000; i++) last = readG(morph);
assert.deepStrictEqual([last, morph.n], ['data', 3000], 'an accessor redefined as data is read as data');

const self = { get q() { delete self.q; self.q = 'plain'; return 'getter'; } };
const readQ = o => o.q;
assert.deepStrictEqual([readQ(self), readQ(self)], ['getter', 'plain'], 'a getter that reshapes its receiver');

const setOnly = { set w(v) {} };
const readW = o => o.w;
for (let i = 0; i < 3000; i++) readW(setOnly);
assert.strictEqual(readW(setOnly), undefined, 'a setter-only accessor reads undefined');

const shapes = Array.from({ length: 12 }, (_, k) => k % 2
  ? { [`p${k}`]: 1, get z() { return k; } }
  : Object.create({ get z() { return -k; } }, { ['q' + k]: { value: 1 } }));
const readZ = o => o.z;
let zs = 0;
for (let i = 0; i < 24000; i++) zs += readZ(shapes[i % 12]);
assert.strictEqual(zs, 12000, 'own and inherited getters across a megamorphic site');

let depth = 0;
const rec = { get d() { depth++; return depth < 5 ? readD(rec) : depth; } };
const readD = o => o.d;
assert.strictEqual(readD(rec), 5, 'a getter can re-enter its own site');

const base = { get me() { return this; } };
const child = Object.create(base);
const readMe = o => o.me;
for (let i = 0; i < 3000; i++) assert.strictEqual(readMe(child), child, 'the receiver is this');

const alloc = { get big() { return Array.from({ length: 64 }, (_, k) => ({ k })); } };
const readBig = o => o.big;
let total = 0;
for (let i = 0; i < 3000; i++) total += readBig(alloc).length;
assert.strictEqual(total, 192000, 'a getter that allocates under GC pressure');

const proto = { get v() { return 1; } };
const inst = Object.create(proto);
const readV = o => o.v;
for (let i = 0; i < 3000; i++) readV(inst);
Object.defineProperty(proto, 'v', { get() { return 2; }, configurable: true });
assert.strictEqual(readV(inst), 2, 'a replaced prototype getter is seen by a warm site');

console.log('accessor ic OK');
