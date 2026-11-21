const assert = require('node:assert');

const indirectEval = eval;
function outcome(source) {
  let result;
  try {
    result = indirectEval('(function(){' + source + '})')();
    result = typeof result === 'function' ? 'fn:' + result.name : JSON.stringify(result);
  } catch (error) {
    result = error.name;
  }
  return result === undefined ? 'undefined' : result;
}

const cases = [
  ["call","class A { #m() { return 7 } f() { return this.#m() } } return new A().f()","7"],
  ["in","class A { #m() {} static has(o) { return #m in o } } return [A.has(new A), A.has({}), A.has(A)]","[true,false,false]"],
  ["in non-object","class A { #m() {} static has(o) { return #m in o } } return A.has(1)","TypeError"],
  ["foreign","class A { #m() { return 1 } static call(o) { return o.#m() } } return A.call({})","TypeError"],
  ["foreign read","class A { #m() { return 1 } static get(o) { return o.#m } } return A.get({})","TypeError"],
  ["primitive read","class A { #m() { return 1 } static get(o) { return o.#m } } return A.get(5)","TypeError"],
  ["null read","class A { #m() { return 1 } static get(o) { return o.#m } } return A.get(null)","TypeError"],
  ["optional","class A { #m() { return 1 } static get(o) { return o?.#m } } return [A.get(null), A.get(undefined), typeof A.get(new A)]","[null,null,\"function\"]"],
  ["optional foreign","class A { #m() { return 1 } static get(o) { return o?.#m } } return A.get({})","TypeError"],
  ["optional call","class A { #m() { return this === undefined ? 'u' : 1 } static get(o) { return o?.#m() } } return [A.get(null), A.get(new A)]","[null,1]"],
  ["identity","class A { #m() {} static get(o) { return o.#m } } return A.get(new A) === A.get(new A)","true"],
  ["name","class A { #m() {} static get(o) { return o.#m } } return A.get(new A).name","\"#m\""],
  ["write","class A { #m() {} static set(o) { o.#m = 1 } } A.set(new A)","TypeError"],
  ["write foreign","class A { #m() {} static set(o) { o.#m = 1 } } A.set({})","TypeError"],
  ["write order","const log = []; class A { #m() {} static set(o) { try { o.#m = (log.push('rhs'), 1) } catch (e) { log.push(e.name) } } } A.set(new A); return log","[\"rhs\",\"TypeError\"]"],
  ["compound","class A { #m() {} static set(o) { o.#m += 1 } } A.set(new A)","TypeError"],
  ["update","class A { #m() {} static set(o) { o.#m++ } } A.set(new A)","TypeError"],
  ["logical assign","class A { #m() {} static set(o) { return typeof (o.#m ||= 1) } } return A.set(new A)","\"function\""],
  ["nullish assign","class A { #m() {} static set(o) { return typeof (o.#m ??= 1) } } return A.set(new A)","\"function\""],
  ["and assign","class A { #m() {} static set(o) { o.#m &&= 1 } } A.set(new A)","TypeError"],
  ["destructure","class A { #m() {} static set(o) { [o.#m] = [1] } } A.set(new A)","TypeError"],
  ["delete-ish this","class A { #m() { return this } f() { return this.#m() === this } } return new A().f()","true"],
  ["before fields","class A { x = this.#m(); #m() { return 3 } } return new A().x","3"],
  ["before ctor body","class A { constructor() { this.x = this.#m() } #m() { return 3 } } return new A().x","3"],
  ["derived before super","class B {} class A extends B { constructor() { let r; try { A.probe(null) } catch (e) { r = e.name } super(); this.r = [r, this.#m()] } #m() { return 3 } static probe(o) { return o.#m } } return new A().r","[\"TypeError\",3]"],
  ["base ctor sees no brand","let seen; class B { constructor() { seen = A.has(this) } } class A extends B { #m() {} static has(o) { return #m in o } } const a = new A; return [seen, A.has(a)]","[false,true]"],
  ["returned foreign gets brand","class B { constructor(o) { return o } } class A extends B { #m() { return 9 } static call(o) { return o.#m() } } const o = {}; new A(o); return A.call(o)","9"],
  ["double install","class B { constructor(o) { return o } } class A extends B { #m() {} } const o = {}; new A(o); new A(o)","TypeError"],
  ["double install fields","class B { constructor(o) { return o } } class A extends B { #f = 1 } const o = {}; new A(o); new A(o)","TypeError"],
  ["double install accessor","class B { constructor(o) { return o } } class A extends B { get #g() { return 1 } } const o = {}; new A(o); new A(o)","TypeError"],
  ["two evaluations","const mk = () => class A { #m() { return 1 } static call(o) { return o.#m() } static get(o) { return o.#m } }; const X = mk(), Y = mk(); let r; try { X.call(new Y) } catch (e) { r = e.name } return [r, X.call(new X), X.get(new X) === Y.get(new Y)]","[\"TypeError\",1,false]"],
  ["static method","class A { static #s() { return 5 } static f() { return A.#s() } static has(o) { return #s in o } } return [A.f(), A.has(A), A.has(new A)]","[5,true,false]"],
  ["static on subclass","class A { static #s() { return 5 } static f() { return this.#s() } } class B extends A {} return B.f()","TypeError"],
  ["getter setter","class A { #v = 1; get #g() { return this.#v } set #g(x) { this.#v = x } f() { this.#g = 5; this.#g += 2; return this.#g } } return new A().f()","7"],
  ["getter only write","class A { get #g() { return 1 } f() { this.#g = 5 } } new A().f()","TypeError"],
  ["setter only read","class A { set #g(v) {} f() { return this.#g } } new A().f()","TypeError"],
  ["super in private","class B { hi() { return 'B' } } class A extends B { #m() { return super.hi() + 'A' } f() { return this.#m() } } return new A().f()","\"BA\""],
  ["proxy","class A { #m() { return 1 } static call(o) { return o.#m() } static has(o) { return #m in o } } const p = new Proxy(new A, {}); let r; try { A.call(p) } catch (e) { r = e.name } return [r, A.has(p)]","[\"TypeError\",false]"],
  ["many methods","class A { #a() { return 1 } #b() { return 2 } #c() { return 3 } #d() { return 4 } #x = 10; f() { return this.#a() + this.#b() + this.#c() + this.#d() + this.#x } } return new A().f()","20"],
  ["inherit chain","class A { #m() { return 'a' } fa() { return this.#m() } } class B extends A { #m() { return 'b' } fb() { return this.#m() } } class C extends B { #n() { return 'c' } fc() { return this.#n() } } const c = new C; return c.fa() + c.fb() + c.fc()","\"abc\""],
  ["nested class access","class A { #m() { return 1 } make() { const self = this; return class I { #m() { return 2 } both() { return [self.#m === undefined, this.#m()] } outer(o) { return o.#m() } } } } const I = new A().make(); let r; try { new I().outer(new A) } catch (e) { r = e.name } return [new I().both(), r]","TypeError"],
  ["nested reaches outer","class A { #m() { return 1 } make() { return class I { get(o) { return o.#m() } } } } const a = new A; return new (a.make())().get(a)","1"],
  ["closure in key before def","let f; class A { [(f = (o) => o.#m(), 'k')]() {} #m() { return 6 } } return f(new A)","6"],
  ["arrow in method","class A { #m() { return 6 } f() { return [1, 2].map(() => this.#m()) } } return new A().f()","[6,6]"],
  ["generator async private","class A { *#g() { yield 1; yield 2 } f() { return [...this.#g()] } } return new A().f()","[1,2]"],
  ["tagged","class A { #m(s) { return s.raw[0] + (this instanceof A) } f() { return this.#m`x` } } return new A().f()","\"xtrue\""],
  ["new private","class A { #M = class K {}; #m() { return 1 } f() { return [new this.#M().constructor.name, typeof this.#m] } } return new A().f()","[\"K\",\"function\"]"],
  ["new method","class A { #m() { } f() { return new this.#m() } } return new A().f()","TypeError"],
  ["spread call","class A { #m(...a) { return a.length } f() { return this.#m(...[1, 2, 3]) } } return new A().f()","3"],
  ["field named like method class","class A { #f = () => 2; #m() { return 1 } f() { return this.#f() + this.#m() } } return new A().f()","3"],
  ["field only class","class A { #x = 1; static has(o) { return #x in o } f() { return this.#x } } return [new A().f(), A.has({})]","[1,false]"],
  ["typeof in loop","class A { #m(i) { return i + 1 } run() { let t = 0; for (let i = 0; i < 20000; i++) t = this.#m(t); return t } } return new A().run()","20000"],
  ["loop foreign late","class A { #m(i) { return i + 1 } static run(o, n) { let t = 0; for (let i = 0; i < n; i++) t = o.#m(t); return t } } A.run(new A, 20000); let r; try { A.run({}, 5) } catch (e) { r = e.name } return [r, A.run(new A, 3)]","[\"TypeError\",3]"],
  ["class in loop","const out = []; let prev; for (let i = 0; i < 3; i++) { class A { #m() { return i } static call(o) { return o.#m() } } let r = 'none'; if (prev) { try { A.call(prev) } catch (e) { r = e.name } } out.push(A.call(new A), r); prev = new A } return out","[0,\"none\",1,\"TypeError\",2,\"TypeError\"]"],
];
for (let round = 0; round < 2; round++)
  for (const [label, source, expected] of cases) assert.strictEqual(outcome(source), expected, label);

class Base {
  constructor(target) { if (target) return target; }
}
class Branded extends Base {
  #count = 0;
  #step() { return ++this.#count; }
  #twice() { return this.#step() + this.#step(); }
  get #total() { return this.#count; }
  static bump(o) { return o.#twice(); }
  static total(o) { return o.#total; }
  static is(o) { return #step in o && #twice in o && #count in o; }
  static method(o) { return o.#step; }
}

const instances = [];
for (let i = 0; i < 5000; i++) instances.push(new Branded());
let sum = 0;
for (let round = 0; round < 4; round++)
  for (const instance of instances) sum += Branded.bump(instance);
assert.strictEqual(sum, 5000 * (3 + 7 + 11 + 15));
assert.strictEqual(Branded.total(instances[0]), 8);
assert.strictEqual(Branded.method(instances[0]), Branded.method(instances[4999]));

const adopted = {};
new Branded(adopted);
assert.strictEqual(Branded.is(adopted), true);
assert.strictEqual(Branded.bump(adopted), 3);
assert.throws(() => new Branded(adopted), TypeError);
assert.strictEqual(Branded.bump(adopted), 7);

for (const foreign of [{}, [], function () {}, Object.create(Branded.prototype), new Proxy(new Branded(), {})]) {
  assert.strictEqual(Branded.is(foreign), false);
  assert.throws(() => Branded.bump(foreign), TypeError);
  assert.throws(() => Branded.method(foreign), TypeError);
}
for (const primitive of [1, 'x', null, undefined, true, Symbol.iterator]) {
  assert.throws(() => Branded.bump(primitive), TypeError);
  assert.throws(() => Branded.is(primitive), TypeError);
}
assert.strictEqual(Branded.bump(instances[1]), 19);

class Evaluated {
  #m() { return 6; }
  viaEval() { return eval('this.#m()'); }
  hasViaEval(o) { return eval('#m in o'); }
}
assert.strictEqual(new Evaluated().viaEval(), 6);
assert.strictEqual(new Evaluated().hasViaEval({}), false);

console.log('ok');
