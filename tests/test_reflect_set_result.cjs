const assert = require('node:assert');

// Reflect.set returns the [[Set]] answer: false where an ordinary Set is
// rejected (read-only or getter-only on the chain, no room for a new own
// property, a read-only array length), the trap's answer at a proxy on the
// chain, true after a store. Expected values come from Node.
const expected = [
  "plain new: [true,1]",
  "plain own: [true,1]",
  "own readonly: false",
  "frozen own: false",
  "sealed own: [true,1]",
  "nonext own: [true,1]",
  "getter only own: false",
  "setter own: [true,3]",
  "inherited setter nonext: [true,4]",
  "inherited writable nonext: false",
  "inherited writable ok: [true,5,1]",
  "sym own: [true,2]",
  "sym frozen: false",
  "sym inherited readonly: false",
  "array idx: [true,true,6]",
  "array nonext existing: [true,9]",
  "array frozen idx: false",
  "array sealed idx: [true,2,false]",
  "array readonly length grow: [false,true,5]",
  "array frozen length: false",
  "array length: [true,1]",
  "function target: [true,1]",
  "proxy on chain false: false",
  "proxy on chain true: [true,[true]]",
  "proxy no trap: [true,1]",
  "setter throws: RangeError",
  "strict caller nonext: false",
  "string key coerced: [true,[\"1.5\"]]",
  "Reflect.set nonext new key: false",
  "Reflect.set nonext new sym: false",
  "Reflect.set nonext existing: true",
  "Reflect.set sealed new: false",
  "Reflect.set inherited readonly: false",
  "Reflect.set array nonext idx: false",
  "setter past readonly length: [true,[9],1]",
  "index past readonly length: [false,1]",
  "length readonly: [false,false]",
  "within readonly length: [true,5]",
  "new plain prop: [true,1]",
  "new prop nonext: [false,false]",
  "existing nonext: [true,2]",
  "sealed existing: [true,2]",
  "sealed new: false",
  "frozen existing: false",
  "inherited readonly: false",
  "inherited writable: [true,2,1]",
  "function name: [false,\"f\"]",
  "function length: [false,2]",
  "function new prop: [true,1]",
  "sealed fn existing name: false",
  "array hole nonext: [false,false]",
  "array existing nonext: [true,5]",
  "sparse array own: true",
  "string wrapper index: false",
  "getter only: false",
  "sym new: [true,1]",
  "sym nonext: false",
  "array readonly length setter on chain: [[9],1]",
  "setter declines its own write: true",
  "compiled setter declines its own write: true",
  "trap declines its own write: true",
  "declined write after a setter: [true,false]",
  "no trap over frozen target: false",
  "no trap over frozen target strict: TypeError",
  "no trap over frozen target sloppy: \"ok\"",
  "no trap sym over nonext target: false"
];

const out = []; const sym = Symbol('s');
const t = (n, f) => { try { out.push(n + ': ' + JSON.stringify(f())); } catch (e) { out.push(n + ': ' + (e.code || e.constructor.name)); } };
t('plain new', () => { const o = {}; return [Reflect.set(o, 'a', 1), o.a]; });
t('plain own', () => { const o = { a: 0 }; return [Reflect.set(o, 'a', 1), o.a]; });
t('own readonly', () => Reflect.set(Object.defineProperty({}, 'a', { value: 0 }), 'a', 1));
t('frozen own', () => Reflect.set(Object.freeze({ a: 0 }), 'a', 1));
t('sealed own', () => { const o = Object.seal({ a: 0 }); return [Reflect.set(o, 'a', 1), o.a]; });
t('nonext own', () => { const o = Object.preventExtensions({ a: 0 }); return [Reflect.set(o, 'a', 1), o.a]; });
t('getter only own', () => Reflect.set({ get a() { return 1; } }, 'a', 2));
t('setter own', () => { let v; const o = { set a(x) { v = x; } }; return [Reflect.set(o, 'a', 3), v]; });
t('inherited setter nonext', () => { let v; const o = Object.preventExtensions(Object.create({ set a(x) { v = x; } })); return [Reflect.set(o, 'a', 4), v]; });
t('inherited writable nonext', () => Reflect.set(Object.preventExtensions(Object.create({ a: 1 })), 'a', 5));
t('inherited writable ok', () => { const p = { a: 1 }; const o = Object.create(p); return [Reflect.set(o, 'a', 5), o.a, p.a]; });
t('sym own', () => { const o = { [sym]: 1 }; return [Reflect.set(o, sym, 2), o[sym]]; });
t('sym frozen', () => Reflect.set(Object.freeze({ [sym]: 1 }), sym, 2));
t('sym inherited readonly', () => Reflect.set(Object.create(Object.defineProperty({}, sym, { value: 1 })), sym, 2));
t('array idx', () => { const a = [1, 2]; return [Reflect.set(a, 0, 9), Reflect.set(a, 5, 1), a.length]; });
t('array nonext existing', () => { const a = Object.preventExtensions([1, 2]); return [Reflect.set(a, 1, 9), a[1]]; });
t('array frozen idx', () => Reflect.set(Object.freeze([1]), 0, 2));
t('array sealed idx', () => { const a = Object.seal([1]); return [Reflect.set(a, 0, 2), a[0], Reflect.set(a, 1, 3)]; });
t('array readonly length grow', () => { const a = [1]; Object.defineProperty(a, 'length', { writable: false }); return [Reflect.set(a, 1, 2), Reflect.set(a, 0, 5), a[0]]; });
t('array frozen length', () => Reflect.set(Object.freeze([]), 'length', 0));
t('array length', () => { const a = [1, 2, 3]; return [Reflect.set(a, 'length', 1), a.length]; });
t('function target', () => { function f() {} return [Reflect.set(f, 'x', 1), f.x]; });
t('proxy on chain false', () => Reflect.set(Object.create(new Proxy({}, { set() { return false; } })), 'a', 1));
t('proxy on chain true', () => { const log = []; const o = Object.create(new Proxy({}, { set(t, k, v, r) { log.push(r === o); return true; } })); return [Reflect.set(o, 'a', 1), log]; });
t('proxy no trap', () => { const tg = {}; return [Reflect.set(new Proxy(tg, {}), 'a', 1), tg.a]; });
t('setter throws', () => Reflect.set({ set a(x) { throw new RangeError('x'); } }, 'a', 1));
t('strict caller nonext', () => { 'use strict'; return Reflect.set(Object.preventExtensions({}), 'z', 1); });
t('string key coerced', () => { const o = {}; return [Reflect.set(o, 1.5, 1), Object.keys(o)]; });
t('Reflect.set nonext new key', () => Reflect.set(Object.preventExtensions({}), 'x', 1));
t('Reflect.set nonext new sym', () => Reflect.set(Object.preventExtensions({}), sym, 1));
t('Reflect.set nonext existing', () => Reflect.set(Object.preventExtensions({ x: 0 }), 'x', 1));
t('Reflect.set sealed new', () => Reflect.set(Object.seal({}), 'y', 1));
t('Reflect.set inherited readonly', () => { const p = {}; Object.defineProperty(p, 'z', { value: 1 }); return Reflect.set(Object.create(p), 'z', 2); });
t('Reflect.set array nonext idx', () => Reflect.set(Object.preventExtensions([1]), 5, 1));

t('setter past readonly length', () => { const log = []; const proto = Object.create(Array.prototype); Object.defineProperty(proto, '5', { set(v) { log.push(v); }, configurable: true }); const a = [1]; Object.setPrototypeOf(a, proto); Object.defineProperty(a, 'length', { writable: false }); return [Reflect.set(a, '5', 9), log, a.length]; });
t('index past readonly length', () => { const a = [1]; Object.defineProperty(a, 'length', { writable: false }); return [Reflect.set(a, 3, 9), a.length]; });
t('length readonly', () => { const a = [1]; Object.defineProperty(a, 'length', { writable: false }); return [Reflect.set(a, 'length', 1), Reflect.set(a, 'length', 2)]; });
t('within readonly length', () => { const a = [1, 2]; Object.defineProperty(a, 'length', { writable: false }); return [Reflect.set(a, 0, 5), a[0]]; });
t('new plain prop', () => { const o = {}; return [Reflect.set(o, 'x', 1), o.x]; });
t('new prop nonext', () => { const o = Object.preventExtensions({}); return [Reflect.set(o, 'x', 1), 'x' in o]; });
t('existing nonext', () => { const o = Object.preventExtensions({ x: 1 }); return [Reflect.set(o, 'x', 2), o.x]; });
t('sealed existing', () => { const o = Object.seal({ x: 1 }); return [Reflect.set(o, 'x', 2), o.x]; });
t('sealed new', () => { const o = Object.seal({}); return Reflect.set(o, 'y', 2); });
t('frozen existing', () => { const o = Object.freeze({ x: 1 }); return Reflect.set(o, 'x', 2); });
t('inherited readonly', () => { const p = Object.defineProperty({}, 'x', { value: 1 }); return Reflect.set(Object.create(p), 'x', 2); });
t('inherited writable', () => { const p = { x: 1 }; const o = Object.create(p); return [Reflect.set(o, 'x', 2), o.x, p.x]; });
t('function name', () => { function f() {} return [Reflect.set(f, 'name', 'g'), f.name]; });
t('function length', () => { function f(a, b) {} return [Reflect.set(f, 'length', 9), f.length]; });
t('function new prop', () => { function f() {} return [Reflect.set(f, 'z', 1), f.z]; });
t('sealed fn existing name', () => { function f() {} Object.seal(f); return Reflect.set(f, 'name', 'q'); });
t('array hole nonext', () => { const a = [1, , 3]; Object.preventExtensions(a); return [Reflect.set(a, 1, 2), 1 in a]; });
t('array existing nonext', () => { const a = [1, 2]; Object.preventExtensions(a); return [Reflect.set(a, 1, 5), a[1]]; });
t('sparse array own', () => { const a = []; a[1000000] = 1; Object.preventExtensions(a); return Reflect.set(a, 1000000, 2); });
t('string wrapper index', () => Reflect.set(new String('ab'), 0, 'x'));
t('getter only', () => Reflect.set({ get x() { return 1; } }, 'x', 2));
t('sym new', () => { const s = Symbol(); const o = {}; return [Reflect.set(o, s, 1), o[s]]; });
t('sym nonext', () => { const s = Symbol(); return Reflect.set(Object.preventExtensions({}), s, 1); });
t('array readonly length setter on chain', () => { const log = []; const proto = Object.create(Array.prototype); Object.defineProperty(proto, '5', { set(v) { log.push(v); }, configurable: true }); const a = [1]; Object.setPrototypeOf(a, proto); Object.defineProperty(a, 'length', { writable: false }); a[5] = 9; return [log, a.length]; });

t('setter declines its own write', () => { const frozen = Object.freeze({}); return Reflect.set({ set a(v) { frozen.x = v; } }, 'a', 1); });
t('compiled setter declines its own write', () => { const frozen = Object.freeze({}); const o = { set a(v) { frozen.x = v; } }; let all = true; for (let i = 0; i < 500; i++) all = Reflect.set(o, 'a', i) && all; return all; });
t('trap declines its own write', () => { const frozen = Object.freeze({}); const p = new Proxy({}, { set(t, k, v) { frozen.x = v; return true; } }); return Reflect.set(p, 'a', 1); });
t('declined write after a setter', () => { const o = Object.preventExtensions({ set a(v) {} }); return [Reflect.set(o, 'a', 1), Reflect.set(o, 'b', 1)]; });

t('no trap over frozen target', () => Reflect.set(new Proxy(Object.freeze({ x: 1 }), {}), 'x', 2));
t('no trap over frozen target strict', () => { 'use strict'; const p = new Proxy(Object.freeze({}), {}); p.y = 1; return 'stored'; });
t('no trap over frozen target sloppy', () => { const p = new Proxy(Object.freeze({}), {}); p.y = 1; return 'ok'; });
t('no trap sym over nonext target', () => Reflect.set(new Proxy(Object.preventExtensions({}), {}), Symbol('s'), 1));

assert.deepStrictEqual(out, expected);
console.log('reflect set result: ok');
