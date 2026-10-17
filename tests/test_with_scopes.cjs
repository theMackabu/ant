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
function hoisted() { var o = { x: 1 }; with (o) { var x = 2; } return [x, o.x]; }
assert.deepStrictEqual(hoisted(), [undefined, 2]);
function hoistedMiss() { with ({}) { var leak9 = 3; } return leak9; }
assert.strictEqual(hoistedMiss(), 3);
assert.strictEqual('leak9' in globalThis, false);
function hoistedFn() { with ({}) { function inner9() { return 4; } } return inner9(); }
assert.strictEqual(hoistedFn(), 4);
function forInVar() { var o = { k: 'o' }; with (o) { for (var k in { a: 1 }); } return [k, o.k]; }
assert.deepStrictEqual(forInVar(), [undefined, 'a']);
function forUpdate() { var oo = { i: 0 }, i = 10; with (oo) { for (i = 0; i < 3; i++); } return [i, oo.i]; }
assert.deepStrictEqual(forUpdate(), [10, 3]);
function addLocal() { var o = { n: 'a' }, n = 1; with (o) { n += 1; } return [n, o.n]; }
assert.deepStrictEqual(addLocal(), [1, 'a1']);
function inferred() { var y = 1; with ({ y: 'str' }) { return y + 1; } }
assert.strictEqual(inferred(), 'str1');
function inferredOuter() { var y = 1; with ({ y: 'str' }) { return (() => y + 1)(); } }
assert.strictEqual(inferredOuter(), 'str1');
var proto9 = { inh: 'inherited' };
with (Object.create(proto9)) { assert.strictEqual(inh, 'inherited'); }
var gets9 = 0;
var getter9 = { get gv() { gets9++; return 'got'; } };
with (getter9) { assert.strictEqual(gv, 'got'); }
assert.strictEqual(gets9, 1);
var setLog9 = [];
var setter9 = { set sv(v) { setLog9.push(v); }, get sv() { return 0; } };
with (setter9) { sv = 5; }
assert.deepStrictEqual(setLog9, [5]);
var protoWrite9 = Object.create({ pw: 1 });
with (protoWrite9) { pw = 2; }
assert.strictEqual(Object.getOwnPropertyDescriptor(protoWrite9, 'pw').value, 2);
var length = 'outer';
with (new String('abc')) { assert.strictEqual(length, 3); }
with ({ u9: 1, [Symbol.unscopables]: { u9: true } }) { assert.strictEqual(typeof u9, 'undefined'); }
var plain9 = { a9: 1 };
for (var n9 = 0; n9 < 3; n9++) with (plain9) { a9 += 1; }
assert.strictEqual(plain9.a9, 4);
function nestedWith9() {
  with ({ v: 'outer' }) {
    return (function () {
      with ({ w: 'inner' }) return () => [v, w, typeof v, delete w];
    })();
  }
}
assert.deepStrictEqual(nestedWith9()(), ['outer', 'inner', 'string', true]);
console.log('with scopes: ok');
