// Object.assign copies every kind of source with [[Set]], in [[OwnPropertyKeys]] order
const assert = require('node:assert');

const same = (a, b) => JSON.stringify(Reflect.ownKeys(a).map(String)) + JSON.stringify(a) === JSON.stringify(Reflect.ownKeys(b).map(String)) + JSON.stringify(b);

assert.ok(same(Object.assign({}, 'ab'), { 0: 'a', 1: 'b' }), 'string sources copy their characters');
assert.ok(same(Object.assign({}, new String('xy')), { 0: 'x', 1: 'y' }), 'String objects copy their characters');
assert.ok(same(Object.assign({}, null, undefined, 5, true, 'c'), { 0: 'c' }), 'other primitives copy nothing');
assert.ok(same(Object.assign({}, [7, , 9]), { 0: 7, 2: 9 }), 'array sources skip holes');
assert.ok(same(Object.assign({}, new Uint8Array([4, 5])), { 0: 4, 1: 5 }), 'typed array sources copy their elements');

const s = Symbol('s');
const log = [];
const src = { get b() { log.push('b'); return 1; }, get [s]() { log.push('s'); return 2; }, get 2() { log.push('2'); return 3; }, get a() { log.push('a'); return 4; } };
Object.assign({}, src);
assert.deepStrictEqual(log.splice(0), ['2', 'b', 'a', 's'], 'getters run in key order');
({ ...src });
assert.deepStrictEqual(log.splice(0), ['2', 'b', 'a', 's'], 'spread runs getters in key order too');

const setOrder = [];
const watched = new Proxy({}, { set(t, k, v) { setOrder.push(String(k)); t[k] = v; return true; } });
Object.assign(watched, { b: 1, [s]: 2, 10: 3, 2: 4, a: 5 });
assert.deepStrictEqual(setOrder, ['2', '10', 'b', 'a', 'Symbol(s)'], 'target writes run in key order');

let setterCalls = 0;
const withSetter = { set x(v) { setterCalls += v; } };
Object.assign(withSetter, { x: 2 }, { x: 3 });
assert.strictEqual(setterCalls, 5, 'own setters on the target run');
const inherits = Object.create({ set y(v) { this.got = v; } });
Object.assign(inherits, { y: 7 });
assert.deepStrictEqual([Object.hasOwn(inherits, 'y'), inherits.got], [false, 7], 'inherited setters run instead of adding a key');

const existing = { a: 1, b: 2 };
Object.assign(existing, { b: 20, c: 30 });
assert.ok(same(existing, { a: 1, b: 20, c: 30 }), 'existing keys update in place and new ones append');

assert.throws(() => Object.assign(Object.freeze({ a: 1 }), { a: 2 }), TypeError, 'a frozen target rejects writes');
assert.throws(() => Object.assign(Object.defineProperty({}, 'r', { value: 1 }), { r: 2 }), TypeError, 'a read-only key rejects writes');
assert.throws(() => Object.assign(Object.create(Object.freeze({ k: 1 })), { k: 2 }), TypeError, 'an inherited read-only key rejects writes');
assert.throws(() => Object.assign(new String('ab'), { 0: 'z' }), TypeError, 'String object indices are read-only');

const arr = [1, 2, 3];
Object.assign(arr, { 0: 'a', 5: 'f' });
assert.deepStrictEqual([arr.length, arr[0], arr[5]], [6, 'a', 'f'], 'array targets update elements and length');

const fn = function () {};
Object.assign(fn, { tag: 1 });
assert.strictEqual(fn.tag, 1, 'function targets take keys');

const proxied = new Proxy({ p: 1, [s]: 2 }, {});
const fromProxy = Object.assign({}, proxied);
assert.ok(fromProxy.p === 1 && fromProxy[s] === 2, 'proxy sources copy string and symbol keys');

const nonEnum = Object.defineProperty({ v: 1 }, 'hidden', { value: 2, enumerable: false });
assert.ok(same(Object.assign({}, nonEnum), { v: 1 }), 'non-enumerable keys are skipped');

const growing = [1, 2, 3];
const grower = { set 0(v) { growing.push(4); this.g0 = v; } };
Object.assign(grower, growing);
assert.deepStrictEqual([grower.g0, grower[1], grower[2], grower[3]], [1, 2, 3, undefined], 'elements added while copying are not visited');

const shrinking = [1, 2, 3];
const shrinker = { set 0(v) { shrinking.length = 1; this.s0 = v; } };
Object.assign(shrinker, shrinking);
assert.deepStrictEqual([shrinker.s0, 1 in shrinker, 2 in shrinker], [1, false, false], 'elements removed while copying are skipped');

const target = {};
for (let i = 0; i < 3000; i++) Object.assign(target, { a: i, b: i + 1 }, 'k');
assert.ok(same(target, { 0: 'k', a: 2999, b: 3000 }), 'compiled callers keep the same results');

// named keys are read before index setters on the target run, as OwnPropertyKeys is taken once
for (const make of [() => ['a'], () => new String('a')]) {
  const src = make();
  const seen = Object.assign({ set 0(v) { src.late = 1; } }, src);
  assert.ok(!('late' in seen) && src.late === 1, `a key added mid-copy is not copied from ${typeof src}`);
}
const dropped = ['a', 'b'];
dropped.gone = 1;
const afterDelete = Object.assign({ set 0(v) { delete dropped.gone; } }, dropped);
assert.ok(!('gone' in afterDelete), 'a key deleted mid-copy is skipped');

// an empty target can take the source layout whole only when [[Set]] would add every key plainly
const protoTarget = {};
const heir = Object.create(protoTarget);
const readP = o => o.p;
const hasQ = o => 'q' in o;
for (let i = 0; i < 3000; i++) assert.ok(readP(heir) === undefined && !hasQ(heir));
Object.assign(protoTarget, { p: 1, q: 2 });
assert.ok(readP(heir) === 1 && hasQ(heir), 'a cached absence on an heir sees keys assigned to its prototype');

const protoKeyed = Object.assign({}, JSON.parse('{"__proto__": {"inherited": 1}, "own": 2}'));
assert.ok(protoKeyed.inherited === 1 && !Object.hasOwn(protoKeyed, '__proto__') && protoKeyed.own === 2, 'an own __proto__ key runs the inherited setter');

let protoSetterCalls = 0;
const withSetterProto = Object.create({ set s(v) { protoSetterCalls++; } });
Object.assign(withSetterProto, { s: 1, t: 2 });
assert.ok(protoSetterCalls === 1 && !Object.hasOwn(withSetterProto, 's') && withSetterProto.t === 2, 'an inherited setter runs instead of adding the key');

const readOnlyProto = Object.create(Object.defineProperty({}, 'r', { value: 0, writable: false }));
assert.throws(() => Object.assign(readOnlyProto, { r: 1 }), TypeError, 'an inherited read-only key rejects the write');
assert.throws(() => Object.assign(Object.freeze({}), { f: 1 }), TypeError, 'a frozen empty target rejects new keys');

const bare = Object.assign(Object.create(null), { x: 1, y: 2 });
assert.ok(Object.getPrototypeOf(bare) === null && bare.x === 1 && bare.y === 2, 'a null-prototype target keeps its prototype');

const trapLog = [];
const behindProxy = Object.create(new Proxy({}, { set(t, k, v, r) { trapLog.push(k); return Reflect.set(t, k, v, r); } }));
Object.assign(behindProxy, { m: 1 });
assert.deepStrictEqual(trapLog, ['m'], 'a proxy on the target prototype chain sees the set');

const reshaped = [1, 2, 3];
const reshapedOut = Object.assign({ set 0(v) { Object.defineProperty(reshaped, 1, { get: () => 'g', enumerable: true }); reshaped[50] = 'far'; } }, reshaped);
assert.deepStrictEqual([reshapedOut[1], reshapedOut[2], 50 in reshapedOut], ['g', 3, false], 'a source made sparse mid-copy still yields its remaining indices');
const holey = [1, , 3];
const holeyOut = Object.assign({ set 0(v) { holey[1] = 'filled'; } }, holey);
assert.deepStrictEqual([1 in holeyOut, holeyOut[2]], [false, 3], 'a hole filled mid-copy was not an own key at the start');
const hidden = [1, 2, 3];
const hiddenOut = Object.assign({ set 0(v) { Object.defineProperty(hidden, 2, { enumerable: false }); } }, hidden);
assert.deepStrictEqual([hiddenOut[1], 2 in hiddenOut], [2, false], 'an index made non-enumerable mid-copy is skipped');

console.log('object assign sources OK');
