const assert = require('node:assert');

const warm = 300;

function describe(value) {
  if (Object.is(value, -0)) return '-0';
  if (typeof value === 'bigint') return `${value}n`;
  if (typeof value === 'string') return JSON.stringify(value);
  if (typeof value === 'symbol') return value.toString();
  if (Array.isArray(value)) return `[${value.map(describe).join(',')}]`;
  return String(value);
}

function attempt(f) {
  try {
    return f();
  } catch (e) {
    return `threw ${e.constructor.name}${e.message === 'boom' ? ' boom' : ''}`;
  }
}

function check(label, run, expected) {
  const first = run().map(describe).join(' ');
  let last = first;
  for (let i = 0; i < warm; i++) last = run().map(describe).join(' ');
  const want = expected.map(describe).join(' ');
  console.log(label, last === want ? 'ok' : 'differs');
  assert.strictEqual(first, want, `${label} (first run)`);
  assert.strictEqual(last, want, `${label} (warm)`);
}

const thrower = { valueOf() { throw new Error('boom'); } };
const stringThrower = { toString() { throw new Error('boom'); } };
const primitiveThrower = { [Symbol.toPrimitive]() { throw new Error('boom'); } };
const trapThrower = new Proxy({}, { get() { throw new Error('boom'); } });

function looseEq(a, b) { return a == b; }
function looseNe(a, b) { return a != b; }
function eqInBranch(a, b) { if (a == b) return 'eq'; return 'ne'; }
function eqCaller(a, b) { return looseEq(a, b) ? 1 : 0; }

check('== propagates', () => [
  attempt(() => looseEq(thrower, 1)),
  attempt(() => looseEq(1, thrower)),
  attempt(() => looseEq('a', stringThrower)),
  attempt(() => looseEq(primitiveThrower, 1n)),
  attempt(() => looseEq(trapThrower, 1)),
  attempt(() => looseNe(thrower, 1)),
  attempt(() => eqInBranch(1, thrower)),
  attempt(() => eqCaller(thrower, 2)),
  looseEq(1, 1), looseNe(1, 2), eqInBranch(2, 2),
], ['threw Error boom', 'threw Error boom', 'threw Error boom', 'threw Error boom', 'threw Error boom',
  'threw Error boom', 'threw Error boom', 'threw Error boom', true, true, 'eq']);

let pendingAfterCatch = 'none';
try { looseEq(thrower, 1); } catch (e) { pendingAfterCatch = e.message; }
check('== leaves nothing pending', () => [pendingAfterCatch, looseEq({}, {}), looseEq(null, undefined)], ['boom', false, true]);

check('== of objects', () => {
  let calls = 0;
  const counted = { valueOf() { calls++; return 1; } };
  const fn = function () {};
  return [looseEq(counted, fn), calls, looseEq(counted, 1), calls, looseEq([], {}), looseEq(Math.max, Math.max),
    looseEq([1], 1), looseEq({ toString() { return 'x'; } }, 'x')];
}, [false, 0, true, 1, false, true, true, true]);

check('BigInt == string', () => [
  looseEq(1n, '1'), looseEq('1', 1n), looseNe(1n, '1'), looseEq(1n, 'x'), looseEq(2n, ' 2 '),
  looseEq(1n, '1.0'), looseEq(1n, '0x1'), looseEq(0n, ''), looseEq(1n, '1n'), looseEq(-1n, '-1'),
  looseEq(1n, '-0x1'), looseEq(15n, '0o17'), looseEq(5n, '0b101'), looseEq(0n, ' '),
  looseEq(12345678901234567890n, '12345678901234567890'), looseEq(1n, '+1'), looseEq(1n, '1_0'),
  looseEq(10n ** 300n, 1e300), looseEq(2n ** 1000n, 2 ** 1000),
], [true, true, false, false, true, false, true, true, false, true, false, true, true, true, true, true, false, false, true]);

function preInc(v) { return ++v; }
function preDec(v) { return --v; }
function postInc(v) { const old = v++; return [old, v]; }
function postDec(v) { const old = v--; return [old, v]; }
function fieldUpdate(o) { const a = o.p++; const b = ++o.p; const c = o.q--; const d = --o.q; return [a, b, c, d, o.p, o.q]; }
function elementUpdate(a) { const x = a[0]++; const y = --a[1]; return [x, y, a[0], a[1]]; }
class Priv { #v; constructor(v) { this.#v = v; } bump() { const o = this.#v++; return [o, ++this.#v]; } }

check('prefix ++/--', () => [
  preInc('19'), preDec('19'), preInc(5n), preDec(5n), preInc(true), preInc(null), preInc(undefined),
  preInc({ valueOf() { return 5; } }), preInc({ valueOf() { return 2n; } }), preInc(' 7 '), preInc(1.5),
  attempt(() => preInc(Symbol('s'))), attempt(() => preDec(thrower)),
], [20, 18, 6n, 4n, 2, 1, NaN, 6, 3n, 8, 2.5, 'threw TypeError', 'threw Error boom']);

check('postfix ++/--', () => [
  postInc('7'), postDec('7'), postInc(5n), postDec(5n), postInc(null), postInc({ valueOf() { return 4; } }),
  postInc({ valueOf() { return 4n; } }), postInc(-0), attempt(() => postInc(thrower)),
], [[7, 8], [7, 6], [5n, 6n], [5n, 4n], [0, 1], [4, 5], [4n, 5n], [-0, 1], 'threw Error boom']);

check('updates of properties', () => [
  fieldUpdate({ p: '3', q: 2n }), elementUpdate(['9', 2n]), new Priv('1').bump(), new Priv(1n).bump(),
], [[3, 5, 2n, 0n, 5, 0n], [9, 1n, 10, 1n], [1, 3], [1n, 3n]]);

function bigintLoop(n) { let x = 0n; for (let i = 0; i < n; i++) ++x; let y = 10n; for (let i = 0; i < n; i++) y--; return [x, y]; }
check('BigInt update loops', () => [bigintLoop(3), bigintLoop(0)], [[3n, 7n], [0n, 10n]]);

function G() {}
class GlobalClass {}
class Derived extends GlobalClass {}
const boundG = G.bind(null);
function isG(v) { return v instanceof G; }
function isClass(v) { return v instanceof GlobalClass; }
function isBound(v) { return v instanceof boundG; }
function isString(v) { return v instanceof String; }
function isObject(v) { return v instanceof Object; }
function isFunction(v) { return v instanceof Function; }

check('instanceof primitives', () => [
  isString('abc'), isString(new String('abc')), 1 instanceof Number, true instanceof Boolean, isObject('x'),
  isG(1), isG('s'), isG(null), isG(undefined), isBound(new G()), isClass(new Derived()),
  isFunction(Math.max), isObject(Math.max), isObject(() => 1),
], [false, true, false, false, false, false, false, false, false, true, true, true, true, true]);

check('instanceof through proxies', () => {
  const proxy = new Proxy(new G(), {});
  const trapped = new Proxy({}, { getPrototypeOf: () => G.prototype });
  const throwing = new Proxy({}, { getPrototypeOf() { throw new Error('boom'); } });
  return [isG(proxy), isG(Object.create(proxy)), isG(trapped), isG(Object.create(trapped)),
    attempt(() => isG(throwing)), isG(new Proxy({}, {}))];
}, [true, true, true, true, 'threw Error boom', false]);

check('instanceof with non-object prototypes', () => {
  function H() {}
  const instance = new H();
  const original = H.prototype;
  H.prototype = 1;
  const r = [attempt(() => instance instanceof H), attempt(() => 1 instanceof H), attempt(() => ({}) instanceof (() => 1)),
    attempt(() => 1 instanceof (() => 1)), attempt(() => ({}) instanceof {}), attempt(() => ({}) instanceof 1)];
  H.prototype = original;
  r.push(instance instanceof H);
  return r;
}, ['threw TypeError', false, 'threw TypeError', false, 'threw TypeError', 'threw TypeError', true]);

check('Symbol.hasInstance', () => {
  class C {}
  const instance = new C();
  const r = [instance instanceof C];
  Object.defineProperty(C, Symbol.hasInstance, { value: (v) => v === 42, configurable: true });
  r.push(instance instanceof C, 42 instanceof C);
  delete C[Symbol.hasInstance];
  r.push(instance instanceof C);
  class Parent { static [Symbol.hasInstance](v) { return v === 'p'; } }
  class Child extends Parent {}
  r.push('p' instanceof Child, new Child() instanceof Child);
  const d = Object.getOwnPropertyDescriptor(Function.prototype, Symbol.hasInstance);
  r.push(typeof d.value, d.writable, d.enumerable, d.configurable);
  Function.prototype[Symbol.hasInstance] = () => true;
  r.push(({}) instanceof C, Function.prototype[Symbol.hasInstance].call(C, instance), Function.prototype[Symbol.hasInstance].call({}, instance));
  r.push(attempt(() => ({}) instanceof { [Symbol.hasInstance]: 1 }), ({}) instanceof { [Symbol.hasInstance]: () => 1 });
  return r;
}, [true, false, true, true, true, false, 'function', false, false, false, false, true, false, 'threw TypeError', true]);

class Base { constructor() { this.b = 1; } }
class ReturnsPrimitive extends Base { constructor(v) { super(); if (v === 0) return 1; if (v === 1) return null; if (v === 2) return { o: 1 }; if (v === 3) return undefined; if (v === 4) { try { return 's'; } catch (e) { return { caught: 1 }; } } } }
function construct(v) { return attempt(() => JSON.stringify(new ReturnsPrimitive(v))); }
function F() { return 1; }

check('derived constructors returning primitives', () => [
  construct(0), construct(1), construct(2), construct(3), construct(4), construct(5),
  attempt(() => Reflect.construct(ReturnsPrimitive, [0])), JSON.stringify(new F()),
], ['threw TypeError', 'threw TypeError', '{"o":1}', '{"b":1}', 'threw TypeError', '{"b":1}', 'threw TypeError', '{}']);

const homeBase = { greet() { return 'base:' + this.n; }, get v() { return 'bv' + this.n; } };
const home = {
  n: 1,
  greet() { return super.greet() + '!'; },
  get v() { return 'o' + super.v; },
  ['c' + 1]() { return super.greet(); },
  arrow() { return (() => super.greet())(); },
};
Object.setPrototypeOf(home, homeBase);
const literalProto = { __proto__: homeBase, n: 2, greet() { return super.greet() + '?'; } };
const late = { m() { return typeof super.greet; } };

check('super in object literal methods', () => {
  const before = late.m();
  Object.setPrototypeOf(late, homeBase);
  const after = late.m();
  Object.setPrototypeOf(late, Object.prototype);
  return [home.greet(), home.v, home.c1(), home.arrow(), literalProto.greet(), before, after];
}, ['base:1!', 'obv1', 'base:1', 'base:1', 'base:2?', 'undefined', 'function']);

function switchHoist(v) {
  switch (v) {
    case 1: return f();
    case 2: function f() { return 'f' + v; } return f();
    default: return typeof f;
  }
}
function switchAnnexB(v) { switch (v) { case 1: { return typeof g; } case 2: function g() {} } return typeof g; }
function switchStrict(v) { 'use strict'; switch (v) { case 1: return h(); case 2: function h() { return 'h'; } } return typeof h; }

check('function declarations in switch cases', () => [
  switchHoist(1), switchHoist(2), switchHoist(3), switchAnnexB(1), switchAnnexB(2), switchAnnexB(5), switchStrict(1), switchStrict(3),
], ['f1', 'f2', 'function', 'function', 'function', 'undefined', 'h', 'undefined']);

function target(a, b) { return [this === undefined ? 'u' : typeof this, a, b, arguments.length]; }
function strictTarget() { 'use strict'; return this; }
function sloppyThis() { return this === globalThis; }

check('Function.prototype.apply argument lists', () => [
  attempt(() => target.apply(null, 1)), attempt(() => target.apply(null, 'ab')), attempt(() => target.apply(null, true)),
  attempt(() => target.apply(null, Symbol('s'))), target.apply(null, null).length, target.apply(null, undefined)[3],
  target.apply(null, { length: 2, 0: 'x' })[1], target.apply(null, function (p, q) {})[3], strictTarget.apply(undefined, []),
], ['threw TypeError', 'threw TypeError', 'threw TypeError', 'threw TypeError', 4, 0, 'x', 2, undefined]);

check('this of spread calls', () => [sloppyThis(...[1]), strictTarget(...[1]), Object.prototype.toString(...[])],
  [true, undefined, '[object Object]']);

console.log('PASS spec operator semantics');
