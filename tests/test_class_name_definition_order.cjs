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
  ["return (class A { static x = this.name; static name = 'n' }).x", "value \"A\""],
  ["return (class A { static x = this.name; static name = 'n' }).name", "value \"n\""],
  ["return (class A { static { A.x = this.name } static name = 'n' }).x", "value \"A\""],
  ["var C = class { static x = this.name; static name = 'n' }; return [C.x, C.name]", "value [\"C\",\"n\"]"],
  ["var C = class { static x = this.name }; return [C.x, C.name]", "value [\"C\",\"C\"]"],
  ["var C = class { static x = Object.getOwnPropertyNames(this) }; return C.x", "value [\"length\",\"name\",\"prototype\"]"],
  ["class A { static x = Object.getOwnPropertyNames(this); static y = 1 } return A.x", "value [\"length\",\"name\",\"prototype\"]"],
  ["class A { static x = Object.getOwnPropertyDescriptor(this, 'name') } return A.x", "value {\"value\":\"A\",\"writable\":false,\"enumerable\":false,\"configurable\":true}"],
  ["class A { static name() { return 'm' } static x = typeof this.name } return [A.x, typeof A.name]", "value [\"function\",\"function\"]"],
  ["class A { static get name() { return 'g' } static x = this.name } return [A.x, A.name]", "value [\"g\",\"g\"]"],
  ["class A { static x = this.name; static name } return [A.x, A.name, Object.getOwnPropertyDescriptor(A, 'name')]", "value [\"A\",null,{\"writable\":true,\"enumerable\":true,\"configurable\":true}]"],
  ["class A { static x = delete this.name; static y = this.name } return [A.x, A.y, 'name' in A, Object.getOwnPropertyNames(A)]", "value [true,\"\",true,[\"length\",\"prototype\",\"x\",\"y\"]]"],
  ["class A { static [(() => 'name')()] = 5 } return [A.name, Object.getOwnPropertyDescriptor(A, 'name')]", "value [5,{\"value\":5,\"writable\":true,\"enumerable\":true,\"configurable\":true}]"],
  ["class A { static ['name']() {} } return [typeof A.name, Object.getOwnPropertyDescriptor(A, 'name').writable]", "value [\"function\",true]"],
  ["return Reflect.ownKeys(class A { f = 1; static s = 2 })", "value [\"length\",\"name\",\"prototype\",\"s\"]"],
  ["return Reflect.ownKeys(class A { static s = 2 })", "value [\"length\",\"name\",\"prototype\",\"s\"]"],
  ["return Reflect.ownKeys(class A {})", "value [\"length\",\"name\",\"prototype\"]"],
  ["return Reflect.ownKeys(class {})", "value [\"length\",\"name\",\"prototype\"]"],
  ["return Reflect.ownKeys(class { f = 1 })", "value [\"length\",\"name\",\"prototype\"]"],
  ["return Reflect.ownKeys(class A { #p = 1; static s = 2; m() { return this.#p } })", "value [\"length\",\"name\",\"prototype\",\"s\"]"],
  ["return Reflect.ownKeys(class A { constructor() { this.a = 1 } static s = 2 })", "value [\"length\",\"name\",\"prototype\",\"s\"]"],
  ["return Reflect.ownKeys(class A extends Object { f = 1; static s = 2 })", "value [\"length\",\"name\",\"prototype\",\"s\"]"],
  ["return Reflect.ownKeys(class A extends Object { constructor() { super() } static s = 2 })", "value [\"length\",\"name\",\"prototype\",\"s\"]"],
  ["return Reflect.ownKeys(class A extends Object { static s = 2 })", "value [\"length\",\"name\",\"prototype\",\"s\"]"],
  ["return Reflect.ownKeys(class A { static m() {} static get g() { return 1 } static 1 = 2; static [Symbol.iterator]() {} }).map(String)", "value [\"1\",\"length\",\"name\",\"prototype\",\"m\",\"g\",\"Symbol(Symbol.iterator)\"]"],
  ["return Reflect.ownKeys(class A { static name = 'x'; static length = 9; static s = 1 })", "value [\"length\",\"name\",\"prototype\",\"s\"]"],
  ["return Reflect.ownKeys(class A { static s = 1; static name = 'x' })", "value [\"length\",\"name\",\"prototype\",\"s\"]"],
  ["return Reflect.ownKeys(class A { static name() {} static s = 1 })", "value [\"length\",\"name\",\"prototype\",\"s\"]"],
  ["return Reflect.ownKeys(class A { static #p = 1; static s = 2 })", "value [\"length\",\"name\",\"prototype\",\"s\"]"],
  ["return Reflect.ownKeys(class A { static { this.z = 1 } static s = 2 })", "value [\"length\",\"name\",\"prototype\",\"z\",\"s\"]"],
  ["return Reflect.ownKeys(function f() {})", "value [\"length\",\"name\",\"prototype\"]"],
  ["return Reflect.ownKeys(function () {})", "value [\"length\",\"name\",\"prototype\"]"],
  ["return Reflect.ownKeys(() => {})", "value [\"length\",\"name\"]"],
  ["return Reflect.ownKeys(async function f() {})", "value [\"length\",\"name\"]"],
  ["return Reflect.ownKeys(function* g() {})", "value [\"length\",\"name\",\"prototype\"]"],
  ["return Reflect.ownKeys({ m() {} }.m)", "value [\"length\",\"name\"]"],
  ["return Reflect.ownKeys(class A { m() {} }.prototype)", "value [\"constructor\",\"m\"]"],
  ["return Reflect.ownKeys((class A { f = 1; g = 2 }).prototype)", "value [\"constructor\"]"],
  ["return Object.keys(new (class A { f = 1; g = 2; #h = 3 }))", "value [\"f\",\"g\"]"],
  ["var d = Object.getOwnPropertyDescriptors(class A { f = 1; static s = 2 }); return Object.keys(d).map(k => k + ':' + ['writable', 'enumerable', 'configurable'].map(a => d[k][a] ? 1 : 0).join(''))", "value [\"length:001\",\"name:001\",\"prototype:000\",\"s:111\"]"],
  ["var d = Object.getOwnPropertyDescriptors(class {}); return Object.keys(d).map(k => k + ':' + ['writable', 'enumerable', 'configurable'].map(a => d[k][a] ? 1 : 0).join('') + ':' + JSON.stringify(d[k].value))", "value [\"length:001:0\",\"name:001:\\\"\\\"\",\"prototype:000:{}\"]"],
  ["class C { static ['k'] = class { static name() {} } } return typeof C.k.name", "value \"string\""],
  ["class C { static k = class { static name() {} } } return typeof C.k.name", "value \"function\""],
  ["class C { ['k'] = class { static name() {} } } return typeof new C().k.name", "value \"string\""],
  ["class C { k = class { static name() {} } } return typeof new C().k.name", "value \"function\""],
  ["var o = { ['k']: class { static name() {} } }; return typeof o.k.name", "value \"string\""],
  ["var o = { k: class { static name() {} } }; return typeof o.k.name", "value \"function\""],
  ["var k = class { static name() {} }; return typeof k.name", "value \"function\""],
  ["var k; k = class { static name() {} }; return typeof k.name", "value \"function\""],
  ["var [k = class { static name() {} }] = []; return typeof k.name", "value \"function\""],
  ["var { k = class { static name() {} } } = {}; return typeof k.name", "value \"function\""],
  ["function f(k = class { static name() {} }) { return typeof k.name } return f()", "value \"function\""],
  ["class C { static ['k'] = class { static name = 5 } } return C.k.name", "value 5"],
  ["class C { static ['k'] = class { static get name() { return 'g' } } } return C.k.name", "value \"k\""],
  ["class C { static ['k'] = class {} } return C.k.name", "value \"k\""],
  ["class C { static [Symbol('s')] = class {} } return Object.getOwnPropertySymbols(C).map(s => C[s].name)", "value [\"[s]\"]"],
  ["class C { static ['k'] = function () {} } return C.k.name", "value \"k\""],
  ["class C { static ['k'] = () => {} } return C.k.name", "value \"k\""],
  ["var o = { ['k']: class { static name() {} }, ['f']: function () {}, ['a']: () => {} }; return [typeof o.k.name, o.f.name, o.a.name]", "value [\"string\",\"f\",\"a\"]"],
  ["class C { static ['k'] = class { static name() {} } } return Object.getOwnPropertyDescriptor(C.k, 'name')", "value {\"value\":\"k\",\"writable\":false,\"enumerable\":false,\"configurable\":true}"],
  ["class C { static ['k'] = class { static name = 5 } } return Object.getOwnPropertyDescriptor(C.k, 'name')", "value {\"value\":5,\"writable\":true,\"enumerable\":true,\"configurable\":true}"],
  ["class C { static ['k'] = class { static get name() { return 'g' } } } return [Object.getOwnPropertyDescriptor(C.k, 'name'), Object.getOwnPropertyNames(C.k)]", "value [{\"value\":\"k\",\"writable\":false,\"enumerable\":false,\"configurable\":true},[\"length\",\"name\",\"prototype\"]]"],
  ["class C { static ['k'] = class { static name() {} static s = this.name } } return typeof C.k.s", "value \"string\""],
  ["class C { static ['k'] = class { static s = this.name } } return C.k.s", "value \"k\""],
  ["var o = { ['k']: class { static s = this.name; static name = 4 } }; return [o.k.s, o.k.name]", "value [\"k\",4]"],
  ["class C { static ['k'] = class Named { static name() {} } } return typeof C.k.name", "value \"function\""],
  ["class C { static ['k'] = (class { static name() {} }) } return typeof C.k.name", "value \"string\""],
  ["class C { static ['k'] = (0, class { static name() {} }) } return typeof C.k.name", "value \"function\""],
  ["var k = 'k'; var o = { [k]: class { static name() {} } }; return [typeof o.k.name, Object.getOwnPropertyNames(o.k)]", "value [\"string\",[\"length\",\"name\",\"prototype\"]]"],
  ["var n = 0; var key = { toString() { n++; return 'tk' } }; var o = { [key]: class { static s = this.name } }; return [o.tk.s, n, Object.keys(o)]", "value [\"tk\",1,[\"tk\"]]"],
];

const failed = [];
for (const [src, want] of cases) {
  const got = outcome(src);
  if (got !== want) failed.push(`${src} => ${got}, want ${want}`);
}
assert.deepStrictEqual(failed, []);

console.log('ok');
