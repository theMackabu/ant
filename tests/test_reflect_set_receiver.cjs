// Reflect.set with a receiver: the target's chain picks the property, the receiver takes the write
const assert = require('node:assert');

const target = { x: 1 };
const receiver = {};
assert.strictEqual(Reflect.set(target, 'x', 2, receiver), true);
assert.deepStrictEqual([target.x, receiver.x], [1, 2], 'a data write lands on the receiver');

let seenThis;
const withSetter = { set v(value) { seenThis = this; this._v = value; } };
const other = {};
assert.strictEqual(Reflect.set(withSetter, 'v', 5, other), true);
assert.strictEqual(seenThis, other, 'a setter runs with the receiver as this');
assert.strictEqual(other._v, 5);
assert.strictEqual(Reflect.set({ get g() { return 1; } }, 'g', 1, other), false, 'a getter-only property rejects the write');

const sym = Symbol('s');
const symTarget = { [sym]: 1 };
const symReceiver = {};
assert.strictEqual(Reflect.set(symTarget, sym, 9, symReceiver), true);
assert.deepStrictEqual([symTarget[sym], symReceiver[sym]], [1, 9], 'symbol keys write to the receiver too');

const readOnly = Object.defineProperty({}, 'r', { value: 1, writable: false });
assert.strictEqual(Reflect.set(readOnly, 'r', 2, {}), false, 'a read-only property on the target rejects the write');
const lockedReceiver = Object.defineProperty({}, 'k', { value: 1, writable: false });
assert.strictEqual(Reflect.set({}, 'k', 2, lockedReceiver), false, 'a read-only property on the receiver rejects the write');
const accessorReceiver = { get a() { return 1; } };
assert.strictEqual(Reflect.set({}, 'a', 2, accessorReceiver), false, 'an accessor on the receiver rejects the write');
assert.strictEqual(Reflect.set({}, 'n', 1, Object.preventExtensions({})), false, 'a non-extensible receiver rejects a new key');
assert.strictEqual(Reflect.set({}, 'p', 1, 5), false, 'a primitive receiver rejects the write');

const existing = { e: 1 };
assert.strictEqual(Reflect.set({}, 'e', 2, existing), true);
assert.deepStrictEqual(Object.getOwnPropertyDescriptor(existing, 'e'), { value: 2, writable: true, enumerable: true, configurable: true }, 'an existing receiver property keeps its attributes');

const proto = { set inherited(value) { this.got = value; } };
const child = Object.create(proto);
const sink = {};
assert.strictEqual(Reflect.set(child, 'inherited', 3, sink), true);
assert.strictEqual(sink.got, 3, 'an inherited setter runs with the receiver as this');

const traps = [];
const proxy = new Proxy({}, { set(t, key, value, recv) { traps.push([key, value, recv === sink]); return true; } });
assert.strictEqual(Reflect.set(proxy, 'q', 4, sink), true);
assert.strictEqual(Reflect.set(Object.create(proxy), 'w', 6, sink), true);
assert.deepStrictEqual(traps, [['q', 4, true], ['w', 6, true]], 'a proxy set trap sees the receiver');

const plainProxy = new Proxy({}, {});
const plainSink = {};
assert.strictEqual(Reflect.set(plainProxy, 'z', 7, plainSink), true);
assert.strictEqual(plainSink.z, 7, 'a proxy without a set trap writes to the receiver');

const reactiveRaw = { count: 0 };
const reactive = new Proxy(reactiveRaw, { set(t, k, v, r) { return Reflect.set(t, k, v, r); } });
for (let i = 0; i < 3000; i++) reactive.count = i;
reactive.added = 'new';
assert.deepStrictEqual([reactiveRaw.count, reactiveRaw.added], [2999, 'new'], 'a trap-less receiver proxy writes through to its target');

const defineLog = [];
const definingRaw = { d: 1 };
const defining = new Proxy(definingRaw, {
  set(t, k, v, r) { return Reflect.set(t, k, v, r); },
  defineProperty(t, k, desc) { defineLog.push([k, desc.value]); return Reflect.defineProperty(t, k, desc); },
});
defining.d = 2;
defining.fresh = 3;
assert.deepStrictEqual(defineLog, [['d', 2], ['fresh', 3]], 'a defineProperty trap on the receiver runs');
assert.deepStrictEqual([definingRaw.d, definingRaw.fresh], [2, 3]);

const describeLog = [];
const describing = new Proxy({ x: 1 }, {
  getOwnPropertyDescriptor(t, k) { describeLog.push(k); return Reflect.getOwnPropertyDescriptor(t, k); },
});
assert.strictEqual(Reflect.set({ x: 0 }, 'x', 5, describing), true);
assert.deepStrictEqual(describeLog, ['x'], 'a getOwnPropertyDescriptor trap on the receiver runs');

const frozenReceiver = Object.freeze({ f: 1 });
assert.strictEqual(Reflect.set({ f: 0 }, 'f', 2, frozenReceiver), false, 'a frozen receiver rejects the write');
assert.strictEqual(Reflect.set({}, 'brandNew', 2, Object.freeze({})), false, 'a frozen receiver rejects a new key');

let proxiedThis;
const accessorRaw = Object.create({ set acc(v) { proxiedThis = this; } });
const accessorProxy = new Proxy(accessorRaw, { set(t, k, v, r) { return Reflect.set(t, k, v, r); } });
accessorProxy.acc = 1;
assert.strictEqual(proxiedThis, accessorProxy, 'an inherited setter sees the proxy as this');

const same = { s: 1 };
assert.strictEqual(Reflect.set(same, 's', 2, same), true);
assert.strictEqual(same.s, 2, 'the target as its own receiver behaves like a plain set');

class Base { set field(value) { this.stored = value; } }
class Derived extends Base { set field(value) { Reflect.set(Base.prototype, 'field', value * 2, this); } }
const d = new Derived();
d.field = 5;
assert.strictEqual(d.stored, 10, 'super-style forwarding through Reflect.set keeps this');

// the receiver's own [[DefineOwnProperty]] decides: array length validates, typed arrays refuse bad numeric keys
const shrink = [1, 2, 3];
assert.deepStrictEqual([Reflect.set({}, 'length', 1, shrink), shrink], [true, [1]], 'an array receiver truncates');
const coerced = [1, 2];
assert.deepStrictEqual([Reflect.set({}, 'length', '1', coerced), coerced.length], [true, 1], 'a string length converts');
for (const bad of [-1, 1.5, 2 ** 32]) assert.throws(() => Reflect.set({}, 'length', bad, [1]), RangeError, `length ${bad} is a RangeError`);
assert.deepStrictEqual([Reflect.set({}, 'length', 0, Object.freeze([1]))], [false], 'a frozen array receiver refuses');

const taReceiver = () => new Uint8Array(2);
for (const key of ['1.5', '-0', '5', 'NaN']) {
  const r = taReceiver();
  assert.deepStrictEqual([Reflect.set({}, key, 7, r), Object.keys(r)], [false, ['0', '1']], `a typed array receiver refuses ${key}`);
}
const inRange = taReceiver();
assert.deepStrictEqual([Reflect.set({}, '1', 7, inRange), inRange[1]], [true, 7], 'a valid index on a typed array receiver writes');

for (const key of ['1.5', '9', 'Infinity']) {
  const sink2 = {};
  assert.deepStrictEqual([Reflect.set(new Uint8Array(2), key, 7, sink2), Object.keys(sink2)], [true, []], `a typed array target swallows ${key}`);
}
const forwarded = {};
assert.deepStrictEqual([Reflect.set(new Uint8Array(2), '1', 7, forwarded), forwarded[1]], [true, 7], 'a valid index forwards to the receiver');

// a builtin receiver keeps its identity as this and takes writes like any function
let seenSelf;
const call = Function.prototype.call;
assert.strictEqual(Reflect.set({ set x(v) { seenSelf = this; } }, 'x', 1, call), true);
assert.strictEqual(seenSelf, call, 'a setter sees the builtin receiver itself');
assert.deepStrictEqual([Reflect.set({}, 'tagged', 7, call), call.tagged], [true, 7], 'a data write lands on the builtin');
delete call.tagged;

Object.defineProperty(Function.prototype, 'hooked', { set(v) { seenSelf = this; }, configurable: true });
assert.strictEqual(Reflect.set(Math.max, 'hooked', 1), true);
assert.strictEqual(seenSelf, Math.max, 'a builtin target is its own default receiver');
delete Function.prototype.hooked;
assert.deepStrictEqual([Reflect.set(Math.max, 'own', 2), Math.max.own], [true, 2], 'a builtin target takes its own write');
assert.strictEqual(Reflect.set(Math.max, 'name', 'x'), false, 'a builtin read-only name still refuses');
delete Math.max.own;

// user code in a define (traps, descriptor getters, key conversion) throws through; only a refusal is false
const boom = () => { throw new TypeError('boom'); };
const throwsBoom = f => assert.throws(f, e => e instanceof TypeError && e.message === 'boom');
const nullAccess = () => null.x;
throwsBoom(() => Reflect.set({}, 'x', 1, new Proxy({}, { defineProperty: boom })));
assert.throws(() => Reflect.set({}, 'x', 1, new Proxy({}, { defineProperty: nullAccess })), TypeError, 'an engine error in a trap throws through');
assert.throws(() => Reflect.set({}, 'x', 1, new Proxy({}, { defineProperty() { throw new RangeError('r'); } })), RangeError);
assert.strictEqual(Reflect.set({}, 'x', 1, new Proxy({}, { defineProperty: () => false })), false, 'a falsy trap refuses');
throwsBoom(() => Reflect.defineProperty(new Proxy({}, { defineProperty: boom }), 'x', { value: 1 }));
assert.throws(() => Reflect.defineProperty({}, 'x', { get value() { return null.x; } }), TypeError, 'a throwing descriptor getter throws through');
assert.throws(() => Reflect.defineProperty({}, { toString: nullAccess }, { value: 1 }), TypeError, 'a throwing key conversion throws through');
assert.throws(() => Reflect.defineProperty({}, 'x', 1), TypeError, 'a non-object descriptor throws');
assert.throws(() => Reflect.defineProperty({}, 'x', { get: 1 }), TypeError, 'a non-callable getter throws');
assert.strictEqual(Reflect.defineProperty(Object.freeze({}), 'x', { value: 1 }), false);
assert.strictEqual(Reflect.defineProperty(Object.defineProperty({}, 'x', { value: 1 }), 'x', { value: 2 }), false);

console.log('reflect set receiver OK');
