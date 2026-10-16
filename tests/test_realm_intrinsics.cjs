const assert = require('node:assert');

function callIt(f, s) {
  return f(s);
}
for (let i = 0; i < 20000; i++) callIt(Function, 'return 1');

const realm = require('ant:internal/realm');
const r = realm.create();
const g = r.global;

for (let i = 0; i < 100; i++) {
  assert.strictEqual(callIt(g.Function, 'return globalThis')(), g);
  assert.strictEqual(callIt(g.eval, 'globalThis'), g);
  assert.strictEqual(callIt(Function, 'return globalThis')(), globalThis);
}

assert.throws(() => g.JSON.parse('{'), g.SyntaxError);
assert.throws(() => g.Array.prototype.map.call([1], 1), g.TypeError);
assert.throws(() => new g.Array(-1), g.RangeError);
assert.ok(realm.evaluate(r, 'try { null.x } catch (e) { e }') instanceof g.TypeError);
assert.ok(realm.evaluate(r, 'new AggregateError([])') instanceof g.AggregateError);
assert.ok(realm.evaluate(r, 'Error()') instanceof g.Error);

assert.strictEqual(
  realm.evaluate(r, `
    const M = Map, S = Set, D = Date, R = RegExp, E = TypeError, DS = DisposableStack;
    globalThis.Map = globalThis.Set = globalThis.Date = globalThis.RegExp = function () {};
    globalThis.TypeError = globalThis.DisposableStack = function () {};
    const ok = [
      Object.getPrototypeOf(M.groupBy([1], (x) => x)) === M.prototype,
      Object.getPrototypeOf(new M()) === M.prototype,
      Object.getPrototypeOf(new S([1]).union(new S([2]))) === S.prototype,
      typeof D() === 'string',
      Object.getPrototypeOf(/a/) === R.prototype,
      Object.getPrototypeOf(new R('a')) === R.prototype,
      'aXbX'.replace(/X/g, '-') === 'a-b-',
      [...'a1b2'.matchAll(/\\d/g)].length === 2,
      (() => { try { null.x } catch (e) { return e instanceof E } })(),
      Object.getPrototypeOf(new DS().move()) === DS.prototype,
    ];
    ok.join();
  `),
  'true,true,true,true,true,true,true,true,true,true'
);
assert.ok(new g.WeakMap() instanceof g.WeakMap);
assert.ok(new g.WeakSet() instanceof g.WeakSet);
assert.ok(new g.WeakRef({}) instanceof g.WeakRef);
assert.ok(new g.FinalizationRegistry(() => {}) instanceof g.FinalizationRegistry);
assert.ok(new g.Map() instanceof g.Map);
assert.ok(!(new g.Map() instanceof Map));
assert.ok(g.Date.prototype.isPrototypeOf(new g.Date(0)));

const { TypeError: T, Map: M } = globalThis;
globalThis.TypeError = globalThis.Map = function () {};
try {
  assert.ok((() => { try { null.x } catch (e) { return e } })() instanceof T);
  assert.strictEqual(Object.getPrototypeOf(M.groupBy([1], (x) => x)), M.prototype);
  assert.ok(structuredClone(new M([[1, 2]])) instanceof M);
} finally {
  globalThis.TypeError = T;
  globalThis.Map = M;
}
assert.ok(structuredClone(new Date(5)) instanceof Date);
assert.strictEqual(structuredClone(new Date(5)).getTime(), 5);
assert.ok(require('node:fs').statSync('.').mtime instanceof Date);

console.log('realm intrinsics: ok');
