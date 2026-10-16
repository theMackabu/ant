const assert = require('node:assert');

var o = { x: 'from with' };
var x = 'global';
with (o) { var f = function () { return x; }; var g = () => x; }
assert.strictEqual(f(), 'from with'); assert.strictEqual(g(), 'from with');
o.x = 'changed'; assert.strictEqual(f(), 'changed');
delete o.x; assert.strictEqual(f(), 'global');
var a = { p: 'a' }, b = { q: 'b' };
var r;
with (a) { with (b) { r = q; } r += p; }
assert.strictEqual(r, 'ba');
with ([1, 2]) { assert.strictEqual(typeof push, 'function'); assert.strictEqual(push(3), 3); }
var obj = { v: 7, m() { return this.v; } };
with (obj) assert.strictEqual(m(), 7);
var w = { y: 1 }; var y = 0;
with (w) { y = 2; z9 = 3; }
assert.strictEqual(w.y, 2); assert.strictEqual(y, 0); assert.strictEqual(globalThis.z9, 3);
with ({ k: 'obj' }) { let k = 'local'; assert.strictEqual(k, 'local'); }
function h(k) { with ({ k: 'obj' }) return k; }
assert.strictEqual(h('param'), 'obj');
with ([]) { assert.strictEqual(typeof values, 'undefined'); }
var d = { t: 1 };
with (d) { assert.strictEqual(typeof t, 'number'); assert.strictEqual(delete t, true); assert.strictEqual(typeof t, 'undefined'); }
var fs = [];
for (var i = 0; i < 3; i++) with ({ i2: i }) fs.push(() => i2);
assert.deepStrictEqual(fs.map((fn) => fn()), [0, 1, 2]);
with ({ deep: 'd' }) { var dd = function () { return function () { return () => deep; }; }; }
assert.strictEqual(dd()()(), 'd');
assert.throws(() => { with (null) {} }, TypeError);
var seen = [];
with (new Proxy({}, { has(t, k) { seen.push(k); return false; } })) { typeof qq; }
assert.ok(seen.includes('qq'));
const cc = 1;
with ({ cc: 0 }) { cc = 5; }
assert.strictEqual(cc, 1);
console.log('with scopes: ok');
