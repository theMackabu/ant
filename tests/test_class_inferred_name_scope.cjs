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
  ["class C { static a = class extends (class {}) { } } return [C.a.name, Object.getPrototypeOf(C.a).name]", "value [\"a\",\"\"]"],
  ["class C { static a = class { [(() => {}).name + 'k'] = 1 } } return Object.keys(new C.a())", "value [\"k\"]"],
  ["var a = class extends (class {}) {}; return [a.name, Object.getPrototypeOf(a).name]", "value [\"a\",\"\"]"],
  ["var o = { a: class extends (class {}) {} }; return [o.a.name, Object.getPrototypeOf(o.a).name]", "value [\"a\",\"\"]"],
  ["var a = class { static [(() => {}).name + 'k'] = 1; [(class {}).name + 'm']() {} }; return [Object.keys(a), Object.getOwnPropertyNames(a.prototype), a.name]", "value [[\"k\"],[\"constructor\",\"m\"],\"a\"]"],
  ["var a = class { static b = class {}; c = () => {}; static d = function () {} }; return [a.name, a.b.name, new a().c.name, a.d.name]", "value [\"a\",\"b\",\"c\",\"d\"]"],
  ["var a = class { constructor() {} }; var b = class { static name = 'x' }; var c = class { static name() {} }; return [a.name, b.name, typeof c.name]", "value [\"a\",\"x\",\"function\"]"],
  ["var a = class { m() { return class {} } static s = (() => class {})() }; return [new a().m().name, a.s.name]", "value [\"\",\"\"]"],
  ["var a = class { static x = (class {}, class {}) }; return a.x.name", "value \"\""],
  ["class C { x = class extends (class {}) {} } var c = new C(); return [c.x.name, Object.getPrototypeOf(c.x).name]", "value [\"x\",\"\"]"],
];

const failed = [];
for (const [src, want] of cases) {
  const got = outcome(src);
  if (got !== want) failed.push(`${src} => ${got}, want ${want}`);
}
assert.deepStrictEqual(failed, []);

console.log('ok');
