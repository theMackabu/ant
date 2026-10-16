const assert = require('node:assert');
const realm = require('ant:internal/realm');

const r = realm.create();
const g = r.global;

assert.notStrictEqual(g.Array.prototype.push, Array.prototype.push);
assert.notStrictEqual(g.JSON.parse, JSON.parse);
assert.notStrictEqual(g.Math.max, Math.max);
assert.notStrictEqual(g.parseInt, parseInt);
assert.strictEqual(g.Array.prototype.values, g.Array.prototype[Symbol.iterator]);
assert.strictEqual(realm.evaluate(r, 'Array.prototype.values === Array.prototype[Symbol.iterator]'), true);

assert.ok(g.JSON.parse('[]') instanceof g.Array);
assert.ok(g.Object.keys({ a: 1 }) instanceof g.Array);
assert.ok(g.Array.prototype.map.call([1], (x) => x) instanceof g.Array);
assert.ok([1].map.call(realm.evaluate(r, '[1]'), (x) => x) instanceof Array);
g.AJSON = JSON;
assert.ok(realm.evaluate(r, 'AJSON.parse("[]")') instanceof Array);
let hot;
for (let i = 0; i < 5000; i++) hot = g.JSON.parse('[1]');
assert.ok(hot instanceof g.Array);

assert.ok(g.eval('[]') instanceof g.Array);
assert.strictEqual(g.eval('this'), g);
assert.ok(new g.Function('return []')() instanceof g.Array);
assert.strictEqual(g.Function('return this')(), g);
assert.strictEqual(realm.evaluate(r, 'var q = 5; (function () { var q = 6; return eval("q"); })()'), 6);
assert.ok(g.Promise.resolve(1).then(() => 0) instanceof g.Promise);

assert.notStrictEqual(g.Date.prototype.getTime, Date.prototype.getTime);
assert.strictEqual(Object.getPrototypeOf(g.Date.prototype.getTime), g.Function.prototype);
assert.strictEqual(
  realm.evaluate(r, 'let res; new Promise((r) => { res = r; }); Object.getPrototypeOf(res) === Function.prototype'),
  true
);
realm.evaluate(r, 'let res2; var selfp = new Promise((r) => { res2 = r; }); res2(selfp);');
g.selfp.catch((e) => {
  assert.ok(e instanceof g.TypeError);
  console.log('realm self-resolution: ok');
});

g.Array.prototype.push.tag = 1;
assert.strictEqual(Array.prototype.push.tag, undefined);

assert.strictEqual(Object.getPrototypeOf(g.JSON.parse), g.Function.prototype);
assert.strictEqual(Object.getPrototypeOf(JSON.parse), Function.prototype);
assert.strictEqual(Object.getPrototypeOf(parseInt), Function.prototype);
assert.ok(parseInt instanceof Function);
assert.ok(g.parseInt instanceof g.Function);
assert.ok(!(g.parseInt instanceof Function));

const names = realm.evaluate(r, 'Object.getOwnPropertyNames(globalThis)');
for (const absent of ['Buffer', 'atob', 'btoa', 'AsyncIterator', 'InternalError', 'import', 'process', 'require', 'setTimeout'])
  assert.ok(!names.includes(absent), absent);
assert.deepStrictEqual(
  JSON.parse(realm.evaluate(r, 'console.log("hidden"); JSON.stringify([typeof console.log, Object.prototype.toString.call(console), console.createTask("t").run(() => 7), typeof console.context().warn, Object.getOwnPropertyDescriptor(globalThis, "console").enumerable])')),
  ['function', '[object console]', 7, 'function', false]
);

g.B = Buffer;
assert.strictEqual(realm.evaluate(r, 'B.from("x") instanceof B'), true);
assert.ok(Buffer.isBuffer(realm.evaluate(r, 'B.alloc(2)')));
assert.ok(Buffer.isBuffer(Buffer.from('hi')));

assert.ok(JSON.parse('[]') instanceof Array);
assert.ok([1].map((x) => x) instanceof Array);
assert.ok(eval('[]') instanceof Array);

console.log('realm builtins: ok');
