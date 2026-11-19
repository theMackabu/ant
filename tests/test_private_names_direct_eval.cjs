const assert = require('node:assert');

const t = (name, f) => { let r; try { r = f() } catch (e) { r = e.constructor.name } return [name, r] };
const S = 'this.#x';
const cases = {
  fileLiteral() { class A { #x = 6; f() { return eval('this.#x') } } return new A().f() },
  fileDynamic() { class A { #x = 6; f(s) { return eval(s) } } return new A().f('this.#x') },
  indirectLiteral() { return (0, eval)("(function(){ class A { #x = 6; f() { return eval('this.#x') } } return new A().f() })")() },
  indirectDynamic() { return (0, eval)("(function(){ class A { #x = 6; f(s) { return eval(s) } } return new A().f('this.#x') })")() },
  newFunction() { return new Function("class A { #x = 6; f() { return eval('this.#x') } } return new A().f()")() },
  newFunctionDynamic() { return new Function("s", "class A { #x = 6; f() { return eval(s) } } return new A().f()")('this.#x') },
  nested() { class O { #o = 1; m(s) { const self = this; class I { #i = 2; f() { return eval(s) } } return new I().f.call(Object.assign(new I(), { self })) } } return [new O().m('this.#i'), new O().m('this.self.#o'), new O().m('this.#i + this.self.#o')] },
  nestedIndirect() { return (0, eval)("(function(s){ class O { #o = 1; m() { const self = this; class I { #i = 2; s = self; f() { return eval(s) } } return new I().f() } } return new O().m() })")('this.#i + this.s.#o') },
  arrow() { class A { #x = 6; f(s) { return (() => eval(s))() } } return new A().f('this.#x') },
  arrowNested() { class A { #x = 6; f(s) { return (() => () => eval(s))()() } } return new A().f('this.#x * 2') },
  fieldInit() { class A { #x = 6; y = eval('this.#x + 1'); } return new A().y },
  fieldInitDynamic() { const s = 'this.#x + 1'; class A { #x = 6; y = eval(s); } return new A().y },
  staticFieldInit() { const s = 'this.#x + 1'; class A { static #x = 6; static y = eval(s); } return A.y },
  staticBlock() { const s = 'A.#x + 2'; class A { static #x = 6; static { A.y = eval(s) } } return A.y },
  hasIn() { class A { #x = 6; static t(o, s) { return eval(s) } } return [A.t(new A(), '#x in o'), A.t({}, '#x in o')] },
  method() { class A { #m() { return 7 } f(s) { return eval(s) } } return new A().f('this.#m()') },
  getter() { class A { get #g() { return 8 } f(s) { return eval(s) } } return new A().f('this.#g') },
  setter() { class A { #v = 0; set #s(v) { this.#v = v } f(s) { eval(s); return this.#v } } return new A().f('this.#s = 9') },
  staticMethod() { class A { static #m() { return 10 } static f(s) { return eval(s) } } return A.f('A.#m()') },
  staticField() { class A { static #x = 11; static f(s) { return eval(s) } } return A.f('this.#x') },
  write() { class A { #x = 1; f(s) { eval(s); return this.#x } } return new A().f('this.#x = 12') },
  compound() { class A { #x = 1; f(s) { eval(s); return this.#x } } return new A().f('this.#x += 12; this.#x++') },
  optional() { class A { #x = 13; f(s, o) { return eval(s) } } return [new A().f('o?.#x', new A()), new A().f('o?.#x', null)] },
  ownClass() { class A { #x = 14; f(s) { return eval(s) } } return new A().f('(class B { #y = 1; #x = 2; g(a) { return [this.#y, this.#x, a.#x] } })').prototype.g.call(new (new A().f('(class B { #y = 1; #x = 2; static mk() { return new B() } g(a) { return [this.#y, this.#x] } })'))()) },
  ownClass2() { class A { #x = 14; f(s) { return eval(s) } } const a = new A(); return a.f('new (class B { #y = 1; g(a) { return [this.#y, a.#x] } })().g(this)') },
  evalInEval() { class A { #x = 15; f(s) { return eval(s) } } return new A().f("eval('this.#x + 1')") },
  evalFnInEval() { class A { #x = 15; f(s) { return eval(s) } } return new A().f("(() => this.#x + 2)()") },
  evalFnInEval2() { class A { #x = 15; f(s) { return eval(s) } } return new A().f("(function () { return this.#x + 3 }).call(this)") },
  undeclared() { class A { #x = 1; f(s) { return eval(s) } } return t('u', () => new A().f('this.#nope'))[1] },
  outsideClass() { return t('u', () => eval('({}).#x'))[1] },
  indirectNoSee() { class A { #x = 1; f() { return (0, eval)('this.#x') } } return t('u', () => new A().f())[1] },
  newFunctionNoSee() { class A { #x = 1; f() { return new Function('return this.#x').call(this) } } return t('u', () => new A().f())[1] },
  newFunctionInside() { class A { #x = 1; f() { return new Function('o', 'return #x in o')(this) } } return t('u', () => new A().f())[1] },
  varEval() { class A { #x = 16; f() { const e = eval; return t('u', () => e('this.#x'))[1] } } return new A().f() },
  wrongObject() { class A { #x = 1; f(s, o) { return eval(s) } } return t('u', () => new A().f('o.#x', {}))[1] },
  hot() { class A { #x = 3; f(s) { return eval(s) } } const a = new A(); let n = 0; for (let i = 0; i < 300; i++) n += a.f('this.#x'); return n },
  shadow() { class O { #x = 'outer'; m(s) { class I { #x = 'inner'; f(o) { return eval(s) } } return new I().f(this) } } return [new O().m('this.#x'), t('u', () => new O().m('o.#x'))[1]] },
  strictEval() { 'use strict'; class A { #x = 17; f(s) { return eval(s) } } return new A().f('this.#x') },
  generator() { class A { #x = 18; *f(s) { yield eval(s) } } return new A().f('this.#x').next().value },
  paramDefault() { class A { #x = 19; f(s, v = eval(s)) { return v } } return new A().f('this.#x') },
  innerFunction() { class A { #x = 21; f(s) { const self = this; return (function () { return eval(s) })() } } return new A().f('self.#x') },
  accessorPair() { class A { #v = 1; get #p() { return this.#v } set #p(v) { this.#v = v * 2 } f(s) { return eval(s) } } return new A().f('this.#p += 5, this.#p') },
  methodAssign() { class A { #m() {} f(s) { return eval(s) } } return t('u', () => new A().f('this.#m = 1'))[1] },
  methodIn() { class A { #m() {} static t(o, s) { return eval(s) } } return [A.t(new A(), '#m in o'), A.t({}, '#m in o')] },
  methodWrongReceiver() { class A { #m() { return 1 } f(s, o) { return eval(s) } } return t('u', () => new A().f('o.#m()', {}))[1] },
  methodValue() { class A { #m() { return 22 } f(s) { return eval(s) } } const a = new A(); return [a.f('this.#m') === a.f('this.#m'), a.f('this.#m').name, a.f('(0, this.#m)()')] },
  derived() { class P { #x = 23; px(s) { return eval(s) } } class D extends P { #y = 24; dy(s) { return eval(s) } } const d = new D(); return [d.px('this.#x'), d.dy('this.#y'), t('u', () => d.dy('this.#x'))[1]] },
  deepEval() { class A { #x = 25; f(s) { return eval(s) } } return new A().f("eval(\"eval('(() => this.#x)()')\")") },
  evalClassMethodEval() { class A { #x = 26; f(s) { return eval(s) } } return new A().f("new (class B { #y = 1; g(a, s) { return eval(s) } })().g(this, 'this.#y + a.#x')") },
  staticAccessor() { class A { static get #g() { return 27 } static f(s) { return eval(s) } } return A.f('A.#g') },
  twoClasses() { class A { #x = 1; f(s, o) { return eval(s) } } class B { #x = 2; f(s, o) { return eval(s) } } return [new A().f('this.#x'), new B().f('this.#x'), t('u', () => new A().f('o.#x', new B()))[1]] },
  literalUndeclared() { class A { #x = 1; f() { return eval('this.#zz') } } return t('u', () => new A().f())[1] },
  literalOutside() { return t('u', () => eval('#q in {}'))[1] },
  indirectLiteral2() { return (0, eval)("(function(){ class A { #m() { return 28 } static #s = 1; f() { return eval('this.#m() + A.#s') } } return new A().f() })")() },
  functionCtorNested() { return new Function("return class A { #x = 29; f() { return (() => eval('this.#x'))() } }")().prototype.f.call(new (new Function("return class A { #x = 29; static mk() { return new A() } f() { return (() => eval('this.#x'))() } }")())()) === undefined },
  functionCtorNested2() { const A = new Function("return class A { #x = 29; f(s) { return (() => eval(s))() } }")(); return new A().f('this.#x') },
  computedKeyNoSee() { return t('u', () => { class A { #x = 1; [eval('1, "k"')]() {} } return Object.getOwnPropertyNames(A.prototype) })[1] },
};
const asyncCases = {
  async asyncMethod() { class A { #x = 20; async f(s) { await null; return eval(s) } } return await new A().f('this.#x') },
};

const expected = {
  "fileLiteral": 6,
  "fileDynamic": 6,
  "indirectLiteral": 6,
  "indirectDynamic": 6,
  "newFunction": 6,
  "newFunctionDynamic": 6,
  "nested": [2, 1, 3],
  "nestedIndirect": 3,
  "arrow": 6,
  "arrowNested": 12,
  "fieldInit": 7,
  "fieldInitDynamic": 7,
  "staticFieldInit": 7,
  "staticBlock": 8,
  "hasIn": [true, false],
  "method": 7,
  "getter": 8,
  "setter": 9,
  "staticMethod": 10,
  "staticField": 11,
  "write": 12,
  "compound": 14,
  "optional": [13, null],
  "ownClass": "THROW TypeError",
  "ownClass2": [1, 14],
  "evalInEval": 16,
  "evalFnInEval": 17,
  "evalFnInEval2": 18,
  "undeclared": "SyntaxError",
  "outsideClass": "SyntaxError",
  "indirectNoSee": "SyntaxError",
  "newFunctionNoSee": "SyntaxError",
  "newFunctionInside": "SyntaxError",
  "varEval": "SyntaxError",
  "wrongObject": "TypeError",
  "hot": 900,
  "shadow": ["inner", "TypeError"],
  "strictEval": 17,
  "generator": 18,
  "paramDefault": 19,
  "innerFunction": 21,
  "accessorPair": 12,
  "methodAssign": "TypeError",
  "methodIn": [true, false],
  "methodWrongReceiver": "TypeError",
  "methodValue": [true, "#m", 22],
  "derived": [23, 24, "SyntaxError"],
  "deepEval": 25,
  "evalClassMethodEval": 27,
  "staticAccessor": 27,
  "twoClasses": [1, 2, "TypeError"],
  "literalUndeclared": "SyntaxError",
  "literalOutside": "SyntaxError",
  "indirectLiteral2": 29,
  "functionCtorNested": "THROW TypeError",
  "functionCtorNested2": 29,
  "computedKeyNoSee": ["constructor", "k"],
  "asyncMethod": 20,
};

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

const evalCases = [
  ["return (0, eval)(\"(function () { class O { #o = 1; m() { class I { f(x) { return x.#o } } return new I().f(this) } } return new O().m() })()\")", "value 1"],
  ["return (0, eval)(\"(function () { class O { #o = 1; static mk() { return class I { static f(x) { return eval('x.#o') } } } } return O.mk().f(new O()) })()\")", "value 1"],
];

const evalFailed = [];
for (const [src, want] of evalCases) {
  const got = outcome(src);
  if (got !== want) evalFailed.push(`${src} => ${got}, want ${want}`);
}
assert.deepStrictEqual(evalFailed, []);

class Probe {
  #x = 1;
  check(src) { try { return eval(src) } catch (e) { return e.constructor.name } }
  literal() {
    const out = [];
    const t = f => { try { out.push(f()) } catch (e) { out.push(e.constructor.name) } };
    t(() => eval('#x'));
    t(() => eval('#x in this'));
    t(() => eval('this.#x'));
    t(() => eval('(#x) in this'));
    t(() => eval('delete this.#x'));
    t(() => eval('this.#q'));
    return out;
  }
}
assert.deepStrictEqual(new Probe().literal(), ['SyntaxError', true, 1, 'SyntaxError', 'SyntaxError', 'SyntaxError']);
assert.deepStrictEqual(['#x in this', 'this.#x', 'delete this.#x'].map(src => new Probe().check(src)), [true, 1, 'SyntaxError']);

(async () => {
  const failed = [];
  const check = (k, v) => {
    const got = JSON.stringify(v);
    const want = JSON.stringify(expected[k]);
    if (got !== want) failed.push(`${k}: got ${got}, want ${want}`);
  };
  const run = async (f) => { try { return await f() } catch (e) { return 'THROW ' + e.constructor.name } };
  for (const k in cases) for (let i = 0; i < 3; i++) check(k, await run(cases[k]));
  for (const k in asyncCases) check(k, await run(asyncCases[k]));
  assert.deepStrictEqual(failed, []);
  assert.strictEqual(Object.keys(expected).length, Object.keys(cases).length + Object.keys(asyncCases).length);
  console.log('ok');
})();
