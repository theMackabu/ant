const assert = require('node:assert');

const indirectEval = eval;
function outcome(source) {
  try {
    return String(indirectEval('(function(){' + source + '})')());
  } catch (error) {
    return error.name;
  }
}

const cases = [
  ["decl key Inner.name","class Inner { [Inner.name]() {} } return 'no throw'","ReferenceError"],
  ["decl key typeof","class Inner { [typeof Inner]() {} } return 'no throw'","ReferenceError"],
  ["expr key Inner","const C = class Inner { [Inner]() {} }; return 'no throw'","ReferenceError"],
  ["expr key typeof","const C = class Inner { [typeof Inner]() {} }; return 'no throw'","ReferenceError"],
  ["extends self decl","class A extends (A, Object) {} return 'no throw'","ReferenceError"],
  ["extends self expr","const C = class A extends (A, Object) {}; return 'no throw'","ReferenceError"],
  ["extends shadows outer","let A = Object; const C = class A extends A {}; return 'no throw'","ReferenceError"],
  ["static field key","class A { static [A.name] = 1 } return 'no throw'","ReferenceError"],
  ["instance field key","class A { [A.name] = 1 } return 'no throw'","ReferenceError"],
  ["getter key","class A { get [A.name]() { return 1 } } return 'no throw'","ReferenceError"],
  ["static method key","class A { static [A.name]() {} } return 'no throw'","ReferenceError"],
  ["key closure called now","class A { [(() => A)()]() {} } return 'no throw'","ReferenceError"],
  ["key closure called later","let f; class A { [(f = () => A, 'k')]() {} } return f() === A","true"],
  ["key typeof closure later","let f; class A { [(f = () => typeof A, 'k')]() {} } return f()","function"],
  ["outer TDZ before decl","{ const g = () => A; try { g() } catch (e) { return e.name } class A {} }","ReferenceError"],
  ["method sees class","class A { m() { return A } } return new A().m() === A","true"],
  ["inner immutable vs outer","class A { m() { return A } } const B = A; A = null; return new B().m() === B","true"],
  ["assign inner in method","class A { m() { A = 1 } } new A().m(); return 'no throw'","TypeError"],
  ["assign inner in static block","class A { static { A = 1 } } return 'no throw'","TypeError"],
  ["assign inner in key","class A { [(A = 1)]() {} } return 'no throw'","ReferenceError"],
  ["expr name invisible outside","const C = class Inner {}; return typeof Inner","undefined"],
  ["expr inner in method","const C = class Inner { m() { return Inner } }; return new C().m() === C","true"],
  ["static field sees class","class A { static me = A } return A.me === A","true"],
  ["static block sees class","let s; class A { static { s = A } } return s === A","true"],
  ["field init sees class","class A { me = A } return new A().me === A","true"],
  ["ctor sees class","class A { constructor() { this.me = A } } return new A().me === A","true"],
  ["ctor arrow sees class","class A { constructor() { this.f = () => A } } return new A().f() === A","true"],
  ["extends closure later","let f; class A extends (f = () => A, Object) {} return f() === A","true"],
  ["nested class key outer ok","class A { static make() { return class B { [A.name]() { return 1 } } } } return new (A.make())().A()","1"],
  ["default export style","const o = { C: class Q { static [typeof Q] = 1 } }; return 'no throw'","ReferenceError"],
  ["outer static in inner key","class O { static #k = \"q\"; static make() { return class I { [O.#k]() { return 7 } } } } return new (O.make())().q()","7"],
  ["arrow key no private","class A { #x = 1; [(() => \"k\")()]() {} ; static f(o) { return o.#x } } return A.f(new A)","1"],
  ["in check hits TDZ","class A { static #p = 1; [((o) => #p in o)(A) ]() {} } return \"no throw\"","ReferenceError"],
  ["own private on {} in key","class A { #x; [ (function(o){ try { return o.#x } catch (e) { return e.name } })({}) ]() { return 1 } } return Object.getOwnPropertyNames(A.prototype).join()","constructor,TypeError"],
  ["own private direct on {}","class A { #x; [({}).#x]() {} } return \"no throw\"","TypeError"],
  ["own #x in {} key","class A { #x; [String(#x in {})]() { return 5 } } return new A().false()","5"],
  ["own static in key this","class A { static #s = 1; static [this === undefined ? \"u\" : \"t\"] = 2 } return Object.keys(A).join()","t"],
  ["this.#p in key","class A { #p = 1; [this.#p]() {} } return \"no throw\"","TypeError"],
  ["inner key outer instance priv","class O { #v = \"name\"; make() { const self = this; return class I { [self.#v]() { return 3 } } } } return new (new O().make())().name()","3"],
  ["inner key own priv closure","let f; class I { #y = 9; [(f = (o) => o.#y, \"k\")]() {} } return f(new I)","9"],
  ["inner key own method closure","let f; class I { #y() { return 4 } [(f = (o) => o.#y(), \"k\")]() {} } return f(new I)","4"],
  ["inner key own priv in closure","let f; class I { #y = 9; [(f = (o) => #y in o, \"k\")]() {} } return f(new I) + \",\" + f({})","true,false"],
  ["static field key own static","class A { static #k = \"q\"; static [A.#k] = 1 } return \"no throw\"","ReferenceError"],
  ["nested inner own in key","class O { static make() { return class I { static #z = 1; [I.#z]() {} } } } O.make(); return \"no throw\"","ReferenceError"],
  ["nested inner key outer priv fn","class O { static #k() { return \"mm\" } static make() { return class I { [O.#k()]() { return 8 } } } } return new (O.make())().mm()","8"],
  ["key uses outer+inner same name","class O { static #k = \"outer\"; static make() { return class I { static #k = \"inner\"; static [(() => { try { return O.#k } catch (e) { return e.name } })()] = 1 } } } return Object.keys(O.make()).join()","TypeError"],
  ["extends uses outer private","class O { static #B = class Base { b() { return 2 } }; static make() { return class I extends O.#B {} } } return new (O.make())().b()","2"],
  ["extends own private","class I extends (class { static x = 1 }, Object) { #q } return \"ok\"","ok"],
  ["extends own private access","try { eval(\"class I extends (o => o.#q) { #q }\"); return \"accepted\" } catch (e) { return e.name }","SyntaxError"],
  ["undeclared in key","try { eval(\"class I { [o.#nope]() {} }\"); return \"accepted\" } catch (e) { return e.name }","SyntaxError"],
  ["getter key own priv","let f; class I { #y = 2; get [(f = (o) => o.#y, \"g\")]() { return 1 } } return f(new I)","2"],
  ["field key own priv","let f; class I { #y = 2; [(f = (o) => o.#y * 2, \"g\")] = 5 } return f(new I) + \",\" + new I().g","4,5"],
  ["static method key priv","let f; class I { static #y = 2; static [(f = () => I.#y, \"g\")]() {} } return f()","2"],
];
for (let round = 0; round < 2; round++)
  for (const [label, source, expected] of cases) assert.strictEqual(outcome(source), expected, label);

function hot() {
  class Counter {
    static total = 0;
    self = Counter;
    constructor() { Counter.total++; }
    read() { return Counter.total; }
    same() { return this.self === Counter; }
  }
  const Alias = Counter;
  let sum = 0;
  for (let i = 0; i < 20000; i++) {
    const c = new Alias();
    sum += c.read() + (c.same() ? 1 : 0);
  }
  return sum;
}
assert.strictEqual(hot(), 20000 * 20001 / 2 + 20000);

function early(flag) {
  let captured;
  try {
    class Late { [(captured = () => Late, flag ? Late.name : 'k')]() { return 1; } }
    return typeof captured() + new Late().k();
  } catch (error) {
    return error.name;
  }
}
for (let i = 0; i < 300; i++) {
  assert.strictEqual(early(false), 'function1');
  assert.strictEqual(early(true), 'ReferenceError');
}

console.log('ok');
