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
  ["class C { static { var f = () => { var await = 3; return await }; C.r = f() } } return C.r", "value 3"],
  ["class C { static { C.f = () => await } } return typeof C.f", "value \"function\""],
  ["class C { static { C.f = () => { await } } } return 1", "value 1"],
  ["class C { static { C.f = () => { function await() {} } } } return 1", "value 1"],
  ["class C { static { C.f = () => { class await {} } } } return 1", "value 1"],
  ["class C { static { C.f = () => { var o = { await } } } } return 1", "value 1"],
  ["class C { static { C.f = (x = () => { var await }) => 1 } } return 1", "value 1"],
  ["class C { static { C.f = () => () => { var await = 1; return await } } } return C.f()()", "value 1"],
  ["class C { static { (await) => 1 } }", "compile SyntaxError"],
  ["class C { static { async await => 1 } }", "compile SyntaxError"],
  ["class C { static { await } }", "compile SyntaxError"],
  ["class C { static { var await } }", "compile SyntaxError"],
  ["class C { static { C.f = () => arguments } }", "compile SyntaxError"],
  ["class C { static x = () => { var await = 2; return await } } return C.x()", "value 2"],
  ["class C { static { function f() { var await = 4; return await } C.r = f() } } return C.r", "value 4"],
  ["function* g() { class C { x = yield 1 } return new C().x } var i = g(); i.next(); return i.next(5).value", "compile SyntaxError"],
  ["function* g() { class C { static x = yield } }", "compile SyntaxError"],
  ["function* g() { class C { x = function* () { yield 1 } } return [...new C().x()] } return g().next().value", "value [1]"],
  ["function* g() { class C { [yield 1] = 2 } return Object.keys(new C()) } var i = g(); i.next(); return i.next('k').value", "value [\"k\"]"],
  ["function* g() { class C { x = () => yield } }", "compile SyntaxError"],
  ["class C { x = yield }", "compile SyntaxError"],
  ["function* g() { class C { static { yield } } }", "compile SyntaxError"],
];

const failed = [];
for (const [src, want] of cases) {
  const got = outcome(src);
  if (got !== want) failed.push(`${src} => ${got}, want ${want}`);
}
assert.deepStrictEqual(failed, []);

console.log('ok');
