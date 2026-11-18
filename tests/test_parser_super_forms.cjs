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
  ["class A { m() { super?.() } }", "compile SyntaxError"],
  ["class A extends Object { constructor() { super?.() } }", "compile SyntaxError"],
  ["class A { m() { super?.x } }", "compile SyntaxError"],
  ["class A { m() { delete super } }", "compile SyntaxError"],
  ["class A { x = delete super }", "compile SyntaxError"],
  ["class A { m() { delete super.x } } try { new A().m() } catch (e) { return e.constructor.name }", "value \"ReferenceError\""],
  ["class A { m() { super => 1 } }", "compile SyntaxError"],
  ["class A { m() { (super) => 1 } }", "compile SyntaxError"],
  ["class A { m() { (super).x } }", "compile SyntaxError"],
  ["class A { m() { return (super.x) } } return new A().m()", "value undefined"],
  ["class A { m() { super } }", "compile SyntaxError"],
  ["class A { m() { super`x` } }", "compile SyntaxError"],
  ["class A { m() { new super } }", "compile SyntaxError"],
  ["class A { m() { new super.x } } try { new A().m() } catch (e) { return e.constructor.name }", "value \"TypeError\""],
  ["class A { m() { super = 1 } }", "compile SyntaxError"],
  ["class A { m() { super++ } }", "compile SyntaxError"],
  ["class A { m() { typeof super } }", "compile SyntaxError"],
  ["class A { m() { [super] } }", "compile SyntaxError"],
  ["class A { m() { ({ super }) } }", "compile SyntaxError"],
  ["class A { m() { super, 1 } }", "compile SyntaxError"],
  ["class A { m() { (a = super) => 1 } }", "compile SyntaxError"],
  ["class A { m() { super.x => 1 } }", "compile SyntaxError"],
  ["class A { m() { return super.toString === Object.prototype.toString } } return new A().m()", "value true"],
  ["var o = { m() { return super.toString === Object.prototype.toString } }; return o.m()", "value true"],
  ["var o = { m() { super?.() } }", "compile SyntaxError"],
  ["var o = { m() { delete super } }", "compile SyntaxError"],
  ["function f() { super.x }", "compile SyntaxError"],
  ["function f() { delete super }", "compile SyntaxError"],
  ["class A { static { delete super } }", "compile SyntaxError"],
  ["class A extends Object { constructor() { (super)() } }", "compile SyntaxError"],
  ["class A { m() { async (super) => 1 } }", "compile SyntaxError"],
];

const failed = [];
for (const [src, want] of cases) {
  const got = outcome(src);
  if (got !== want) failed.push(`${src} => ${got}, want ${want}`);
}
assert.deepStrictEqual(failed, []);

console.log('ok');
