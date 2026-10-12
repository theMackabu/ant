// primitives are never instances; their wrapper objects are
const assert = require('node:assert');

const check = () => {
  assert.deepStrictEqual(
    ['abc' instanceof String, 1 instanceof Number, true instanceof Boolean, 1n instanceof BigInt, Symbol() instanceof Symbol],
    [false, false, false, false, false],
    'primitives are not instances of their constructors'
  );
  assert.deepStrictEqual(['abc' instanceof Object, String('a') instanceof String], [false, false]);
  assert.deepStrictEqual(
    [new String('a') instanceof String, new Number(1) instanceof Number, new Boolean(false) instanceof Boolean, Object('a') instanceof Object],
    [true, true, true, true],
    'wrapper objects are'
  );
  class Sub extends String {}
  assert.deepStrictEqual([new Sub('x') instanceof String, 'x' instanceof Sub], [true, false]);
};

for (let i = 0; i < 300; i++) check();

const isString = { [Symbol.hasInstance]: v => typeof v === 'string' };
assert.deepStrictEqual(['abc' instanceof isString, 1 instanceof isString], [true, false], 'a plain object with Symbol.hasInstance works and sees primitives');
assert.throws(() => ({}) instanceof {}, TypeError, 'a plain object without Symbol.hasInstance is not callable');
assert.throws(() => ({}) instanceof { [Symbol.hasInstance]: 1 }, TypeError);

assert.deepStrictEqual([Object() instanceof Object, Object(undefined) instanceof Object, Object(null) instanceof Object], [true, true, true], 'Object() without new gets Object.prototype');

// Symbol.hasInstance is looked up through the constructor's own prototype chain
class Branded { static [Symbol.hasInstance](v) { return v === 'brand'; } }
class SubBranded extends Branded {}
const viaSub = v => v instanceof SubBranded;
for (let i = 0; i < 300; i++) assert.deepStrictEqual([viaSub('brand'), viaSub({})], [true, false], 'an inherited static hook answers');

class Plain {}
class Later extends Plain {}
const viaLater = v => v instanceof Later;
for (let i = 0; i < 300; i++) assert.strictEqual(viaLater(new Later()), true);
Object.defineProperty(Plain, Symbol.hasInstance, { value: () => false });
assert.strictEqual(viaLater(new Later()), false, 'a hook added to a parent constructor later is seen by a warm site');

class GateBase {}
class Gate extends GateBase {}
const viaGate = v => v instanceof Gate;
for (let i = 0; i < 300; i++) assert.deepStrictEqual([viaGate(new Gate()), viaGate('s'), viaGate(1)], [true, false, false]);
Object.defineProperty(GateBase, Symbol.hasInstance, { value: v => v === 's' });
assert.deepStrictEqual([viaGate('s'), viaGate(1)], [true, false], 'a warm site asks a hook added after primitives were answered');

// Function.prototype[Symbol.hasInstance] is the ordinary algorithm, fixed in place
const intrinsic = Function.prototype[Symbol.hasInstance];
assert.deepStrictEqual([typeof intrinsic, intrinsic.name, intrinsic.length], ['function', '[Symbol.hasInstance]', 1]);
assert.deepStrictEqual(Object.getOwnPropertyDescriptor(Function.prototype, Symbol.hasInstance),
  { value: intrinsic, writable: false, enumerable: false, configurable: false });
assert.strictEqual(Object.getOwnPropertyDescriptor(Function.prototype, 'call').value, Function.prototype.call, 'a builtin read through a descriptor keeps its identity');
class Ordinary {}
assert.deepStrictEqual([intrinsic.call(Ordinary, new Ordinary()), intrinsic.call(Ordinary, {}), intrinsic.call(Ordinary, 'x')], [true, false, false]);
assert.deepStrictEqual([intrinsic.call({}, {}), intrinsic.call(Object, Object.create(null))], [false, false], 'a non-callable or unrelated this answers false');
assert.strictEqual(intrinsic.call(Ordinary.bind(null), new Ordinary()), true, 'a bound function forwards to its target');
assert.strictEqual(intrinsic.call(new Proxy(Ordinary, {}), new Ordinary()), true, 'a callable proxy reads prototype through the proxy');
class Hooked { static [Symbol.hasInstance](v) { return v === 1 || Function.prototype[Symbol.hasInstance].call(this, v); } }
assert.deepStrictEqual([1 instanceof Hooked, new Hooked() instanceof Hooked, {} instanceof Hooked], [true, true, false], 'a hook can defer to the intrinsic');
assert.throws(() => { 'use strict'; Function.prototype[Symbol.hasInstance] = () => true; }, TypeError);
for (let i = 0; i < 300; i++) assert.strictEqual(viaLater({}), false);

function BoundBase() {}
const BoundCtor = BoundBase.bind(null);
BoundCtor.prototype = {};
const viaBound = v => v instanceof BoundCtor;
for (let i = 0; i < 300; i++) assert.strictEqual(viaBound(new BoundBase()), true, 'a bound function forwards to its target, not its own prototype');
assert.deepStrictEqual([Math.max instanceof Function, Math.max instanceof Object, Math.max instanceof Array], [true, true, false], 'a builtin is an ordinary function on the left');
assert.strictEqual(1 instanceof new Proxy({}, { get: (t, k) => k === Symbol.hasInstance ? () => true : undefined }), true, 'a proxy hook runs before the callability check');
class Warm {}
const viaWarm = v => v instanceof Warm;
for (let i = 0; i < 300; i++) assert.deepStrictEqual([viaWarm(1), viaWarm('s'), viaWarm(new Warm()), viaWarm({})], [false, false, true, false]);

function NonObjectProto() {}
NonObjectProto.prototype = 1;
assert.throws(() => ({}) instanceof NonObjectProto, TypeError, 'a non-object prototype throws for an object');
assert.strictEqual(1 instanceof NonObjectProto, false, 'a primitive is checked before the prototype');
assert.throws(() => ({}) instanceof (() => {}), TypeError, 'an arrow function has no prototype');
assert.throws(() => ({}) instanceof new Proxy(NonObjectProto, {}), TypeError, 'a proxied constructor is checked the same way');

console.log('instanceof primitives OK');
