const assert = require('node:assert');
const realm = require('ant:internal/realm');

const r = realm.create();
const g = r.global;

assert.notStrictEqual(g.Array, Array);
assert.notStrictEqual(g.Object, Object);
assert.strictEqual(realm.evaluate(r, 'globalThis'), g);
assert.strictEqual(realm.evaluate(r, '1 + 2'), 3);

const arr = realm.evaluate(r, '[1, 2, 3]');
assert.strictEqual(arr instanceof Array, false);
assert.strictEqual(Array.isArray(arr), true);
assert.strictEqual(Object.getPrototypeOf(arr), g.Array.prototype);
assert.strictEqual(Object.getPrototypeOf(realm.evaluate(r, '({})')), g.Object.prototype);

assert.strictEqual(realm.evaluate(r, 'var x = 41; x + 1'), 42);
assert.strictEqual(g.x, 41);
assert.strictEqual(typeof x, 'undefined');
assert.strictEqual(realm.evaluate(r, 'let y = 5; y'), 5);
assert.strictEqual(realm.evaluate(r, 'y * 2'), 10);
assert.strictEqual(typeof y, 'undefined');

assert.strictEqual(realm.evaluate(r, 'Symbol.iterator'), Symbol.iterator);

let thrown;
try { realm.evaluate(r, 'throw new TypeError("boom")'); } catch (e) { thrown = e; }
assert.strictEqual(thrown instanceof TypeError, false);
assert.strictEqual(thrown instanceof g.TypeError, true);
assert.strictEqual(thrown.message, 'boom');

assert.strictEqual(
  realm.evaluate(r, 'JSON.stringify({ a: [1, new Map([[1, 2]]).size], d: typeof Date.now(), m: Math.max(1, 2), re: /a(b)/.exec("ab")[1] })'),
  '{"a":[1,1],"d":"number","m":2,"re":"b"}'
);
assert.strictEqual(Object.getPrototypeOf([]), Array.prototype);

const realms = [];
for (let i = 0; i < 12; i++) {
  const ri = realm.create();
  realm.evaluate(ri, `var keep = []; for (let i = 0; i < 500; i++) keep.push({ i, s: 'v' + i, a: [i, i + 1] }); let lex = { tag: ${i} };`);
  realms.push(ri);
}
let junk = 0;
for (let i = 0; i < 400000; i++) { const o = { i, a: [i] }; junk += o.a[0] & 1; }
for (let i = 0; i < realms.length; i++) {
  assert.strictEqual(
    realm.evaluate(realms[i], 'keep.length + ":" + keep[499].s + ":" + keep[123].a[1] + ":" + lex.tag + ":" + (Object.getPrototypeOf(keep) === Array.prototype)'),
    `500:v499:124:${i}:true`
  );
  assert.strictEqual(realm.evaluate(realms[i], 'let t = 0; for (let k = 0; k < 3000; k++) { const o = { k, m: new Map([[k, k]]) }; t += o.m.get(k) & 1; } t'), 1500);
}
assert.ok(junk > 0);

console.log('realm basics: ok');
