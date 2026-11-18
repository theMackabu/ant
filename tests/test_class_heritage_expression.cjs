const assert = require('node:assert');

function show(v) {
  if (typeof v === 'function') return 'function ' + v.name;
  if (typeof v === 'symbol' || typeof v === 'bigint') return String(v);
  if (v === undefined) return 'undefined';
  try { return JSON.stringify(v) } catch (e) { return 'unserializable' }
}

function outcome(src) {
  let fn;
  try { fn = new Function(src) } catch (e) { return 'compile ' + e.constructor.name }
  try { return 'value ' + show(fn()) } catch (e) { return 'throw ' + (e && e.constructor ? e.constructor.name : String(e)) }
}

const cases = [
  ["var a = 1, b = Object, c = Array; class X extends a ? b : c {}", "compile SyntaxError"],
  ["var b = Object; class X extends (1 ? b : 0) {} return 1", "value 1"],
  ["class X extends a = b {}", "compile SyntaxError"],
  ["var f = () => Object; class X extends f() {} return 1", "value 1"],
  ["class X extends () => 1 {}", "compile SyntaxError"],
  ["var a; class X extends a, Object {}", "compile SyntaxError"],
  ["class X extends new Object {}", "throw TypeError"],
  ["var o = { p: Object }; class X extends o.p {} return 1", "value 1"],
  ["class X extends async () => 1 {}", "compile SyntaxError"],
  ["class X extends 1 + 1 {}", "compile SyntaxError"],
  ["class X extends !Object {}", "compile SyntaxError"],
  ["class X extends void 0 {}", "compile SyntaxError"],
];

const failed = [];
for (const [src, want] of cases) {
  const got = outcome(src);
  if (got !== want) failed.push(`${src} => ${got}, want ${want}`);
}
assert.deepStrictEqual(failed, []);

console.log('ok');
