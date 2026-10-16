const assert = require('node:assert');
const realm = require('ant:internal/realm');

const hot = (f) => { let v; for (let i = 0; i < 3000; i++) v = f(i); return v; };

function caller(f) { return f(); }
function methodCaller(o) { return o.m(); }
function ctorCaller(C) { return new C(); }
const mainF = () => [];
const mainO = { m() { return {}; } };
class MainC {}
for (let i = 0; i < 20000; i++) { caller(mainF); methodCaller(mainO); ctorCaller(MainC); }

const r = realm.create();
const g = r.global;
const isB = (v) => v instanceof g.Array || Object.getPrototypeOf(v) === g.Object.prototype;

const mkB = realm.evaluate(r, '() => []');
assert.strictEqual(mkB() instanceof Array, false);
assert.strictEqual(mkB() instanceof g.Array, true);
assert.strictEqual(hot(() => mkB()) instanceof g.Array, true);

g.mkA = () => [];
assert.strictEqual(realm.evaluate(r, 'mkA()') instanceof Array, true);
assert.strictEqual(realm.evaluate(r, 'let v; for (let i = 0; i < 3000; i++) v = mkA(); v') instanceof Array, true);

const objB = realm.evaluate(r, '({ m() { return {}; } })');
assert.ok(isB(hot(() => objB.m())));

const CB = realm.evaluate(r, 'class C { constructor() { this.a = []; } }; C');
const made = hot(() => new CB());
assert.ok(made instanceof CB);
assert.ok(made.a instanceof g.Array);
assert.strictEqual(Object.getPrototypeOf(CB.prototype), g.Object.prototype);

const tiny = realm.evaluate(r, '(x) => ({ x })');
const tinyOut = hot((i) => tiny(i));
assert.ok(isB(tinyOut));
assert.strictEqual(tinyOut.x, 2999);

const mapped = [1, 2].map(mkB);
assert.ok(mapped instanceof Array && mapped[0] instanceof g.Array);
const mappedB = realm.evaluate(r, '[1, 2].map(mkA)');
assert.ok(mappedB instanceof g.Array && mappedB[0] instanceof Array);

const genB = realm.evaluate(r, 'function* gen() { yield []; yield {}; } gen');
const it = genB();
assert.ok(it.next().value instanceof g.Array);
assert.ok(isB(it.next().value));
assert.strictEqual(Object.getPrototypeOf(it), genB.prototype);

const fresh = realm.evaluate(r, 'function fresh() {} fresh');
assert.strictEqual(Object.getPrototypeOf(fresh), g.Function.prototype);
assert.strictEqual(Object.getPrototypeOf(fresh.prototype), g.Object.prototype);
for (const name of ['Symbol', 'RegExp', 'Map', 'Set', 'WeakMap', 'ArrayBuffer', 'DataView'])
  assert.strictEqual(Object.getPrototypeOf(g[name]), g.Function.prototype, name);

const throwB = realm.evaluate(r, '() => { throw new TypeError("b"); }');
let caught;
for (let i = 0; i < 3000; i++) { try { throwB(); } catch (e) { caught = e; } }
assert.ok(caught instanceof g.TypeError);
assert.ok([] instanceof Array);
assert.strictEqual(Object.getPrototypeOf({}), Object.prototype);

assert.strictEqual(realm.evaluate(r, 'function h() { return this; } h')(), g);

const tailB = realm.evaluate(r, '(function () { function tc(n) { return n ? mkA(n - 1) : []; } return tc; })()');
g.mkA = (n) => (n ? tailB(n - 1) : []);
assert.ok(tailB(10) instanceof g.Array);
assert.ok(tailB(11) instanceof Array);

const boundB = mkB.bind(null);
assert.ok(hot(() => boundB()) instanceof g.Array);

const accessor = realm.evaluate(r, '({ get v() { return []; }, set v(x) { this.last = []; } })');
assert.ok(hot(() => accessor.v) instanceof g.Array);
accessor.v = 1;
assert.ok(accessor.last instanceof g.Array);

const proxied = new Proxy({}, { get: realm.evaluate(r, '() => []') });
assert.ok(proxied.anything instanceof g.Array);

for (let i = 0; i < 20000; i++) { caller(mkB); methodCaller(objB); ctorCaller(CB); }
assert.ok(caller(mkB) instanceof g.Array);
assert.ok(isB(methodCaller(objB)));
assert.ok(ctorCaller(CB).a instanceof g.Array);
assert.ok(caller(mainF) instanceof Array);

let r2, fB2, last;
for (let i = 0; i < 40000; i++) {
  if (i === 20000) { r2 = realm.create(); fB2 = realm.evaluate(r2, '() => []'); }
  last = i >= 20000 ? fB2() : mainF();
}
assert.ok(last instanceof r2.global.Array);

const asyncB = realm.evaluate(r, 'async function af() { await null; return []; } af');
assert.ok(asyncB() instanceof g.Promise);
Promise.resolve().then(mkB).then((v) => {
  assert.ok(v instanceof g.Array);
  return asyncB();
}).then((v) => {
  assert.ok(v instanceof g.Array);
  setTimeout(() => {
    assert.ok(mkB() instanceof g.Array);
    assert.ok([] instanceof Array);
    console.log('realm calls: ok');
  }, 1);
});
