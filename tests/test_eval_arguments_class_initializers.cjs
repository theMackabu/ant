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
  ["class C { static { eval('C.r = typeof arguments') } } return C.r", "throw SyntaxError"],
  ["class C { static x = eval('typeof arguments') } return C.x", "throw SyntaxError"],
  ["class C { x = eval('typeof arguments') } return new C().x", "throw SyntaxError"],
  ["class C { x = eval('(() => arguments)') } return typeof new C().x", "throw SyntaxError"],
  ["class C { x = eval('(function () { return arguments.length })(1, 2)') } return new C().x", "value 2"],
  ["class C { m() { return eval('arguments.length') } } return new C().m(1, 2)", "value 2"],
  ["class C { static { C.r = eval('(function () { return typeof arguments })()') } } return C.r", "value \"object\""],
  ["class C { x = () => eval('arguments') } return typeof new C().x", "value \"function\""],
  ["class C { static x = eval('var arguments = 1; arguments') } return C.x", "throw SyntaxError"],
  ["function f() { class C { x = eval('arguments') } return new C() } return typeof f(1)", "throw SyntaxError"],
  ["class C { static { (() => eval('arguments'))() } }", "throw SyntaxError"],
  ["class C { x = eval('eval(\"arguments\")') } return typeof new C()", "throw SyntaxError"],
];

const failed = [];
for (const [src, want] of cases) {
  const got = outcome(src);
  if (got !== want) failed.push(`${src} => ${got}, want ${want}`);
}
assert.deepStrictEqual(failed, []);
console.log('ok');
