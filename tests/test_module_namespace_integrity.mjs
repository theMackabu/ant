import assert from 'node:assert';
import * as self from './test_module_namespace_integrity.mjs';

assert.strictEqual(Object.isExtensible(self), false);
assert.deepStrictEqual(Reflect.ownKeys(self), ['alpha', 'counter', 'default', 'zeta', Symbol.toStringTag]);
assert.throws(() => { self.added = 1; }, TypeError);
assert.strictEqual(Reflect.defineProperty(self, 'added', { value: 1 }), false);
assert.strictEqual(Reflect.deleteProperty(self, 'zeta'), false);
assert.strictEqual(Reflect.preventExtensions(self), true);

export function zeta() { return ++counter; }
export let counter = 1;
export const alpha = 'a';
export default 'd';

assert.strictEqual(Object.isSealed(self), true);
assert.strictEqual(Object.isFrozen(self), false);
assert.deepStrictEqual(Object.keys(self), ['alpha', 'counter', 'default', 'zeta']);
assert.throws(() => Object.freeze(self), TypeError);
assert.throws(() => { self.counter = 5; }, TypeError);
assert.throws(() => { self.alpha = 'b'; }, TypeError);
assert.throws(() => { self.zeta = 1; }, TypeError);
assert.strictEqual(Reflect.set(self, 'counter', 5), false);
assert.strictEqual(Reflect.set(self, 'missing', 5), false);
assert.strictEqual(counter, 1);
assert.strictEqual(self.counter, 1);
assert.strictEqual(self.zeta(), 2);
assert.strictEqual(self.counter, 2);

const source = 'export let x = 1; export function inc() { return ++x; } export const y = 2; export default 3; export { x as b, inc as a };';
const dep = await import('data:text/javascript,' + encodeURIComponent(source));

assert.deepStrictEqual(Object.keys(dep), ['a', 'b', 'default', 'inc', 'x', 'y']);
assert.deepStrictEqual(Reflect.ownKeys(dep), ['a', 'b', 'default', 'inc', 'x', 'y', Symbol.toStringTag]);
assert.deepStrictEqual(Object.getOwnPropertyDescriptor(dep, 'x'), { value: 1, writable: true, enumerable: true, configurable: false });
assert.deepStrictEqual(Object.getOwnPropertyDescriptor(dep, 'y'), { value: 2, writable: true, enumerable: true, configurable: false });
assert.deepStrictEqual(Object.getOwnPropertyDescriptor(dep, Symbol.toStringTag), { value: 'Module', writable: false, enumerable: false, configurable: false });
assert.deepStrictEqual(Object.getOwnPropertyDescriptors(dep).x, { value: 1, writable: true, enumerable: true, configurable: false });
assert.deepStrictEqual(Reflect.getOwnPropertyDescriptor(dep, 'x'), { value: 1, writable: true, enumerable: true, configurable: false });

assert.throws(() => { dep.x = 2; }, TypeError);
assert.throws(() => { dep.y = 5; }, TypeError);
assert.throws(() => { dep.inc = 1; }, TypeError);
assert.throws(() => { dep.default = 1; }, TypeError);
assert.throws(() => { dep['x'] = 2; }, TypeError);
assert.throws(() => Object.assign(dep, { x: 7 }), TypeError);
assert.strictEqual(Reflect.set(dep, 'x', 3), false);
assert.strictEqual(Reflect.set(dep, 'x', 3, {}), false);
assert.strictEqual(Reflect.set(dep, 'x', 1), false);
assert.strictEqual(dep.x, 1);

assert.strictEqual(Reflect.defineProperty(dep, 'x', { value: 1 }), true);
assert.strictEqual(Reflect.defineProperty(dep, 'x', { value: 1, writable: true, enumerable: true, configurable: false }), true);
assert.strictEqual(Reflect.defineProperty(dep, 'x', {}), true);
assert.strictEqual(Reflect.defineProperty(dep, 'x', { value: 99 }), false);
assert.strictEqual(Reflect.defineProperty(dep, 'x', { writable: false }), false);
assert.strictEqual(Reflect.defineProperty(dep, 'x', { enumerable: false }), false);
assert.strictEqual(Reflect.defineProperty(dep, 'x', { configurable: true }), false);
assert.strictEqual(Reflect.defineProperty(dep, 'x', { get() { return 1; } }), false);
assert.strictEqual(Reflect.defineProperty(dep, 'missing', { value: 1 }), false);
assert.throws(() => Object.defineProperty(dep, 'x', { value: 99 }), TypeError);
assert.throws(() => Object.defineProperty(dep, 'x', { writable: false }), TypeError);
assert.strictEqual(Object.defineProperty(dep, 'x', { value: 1 }), dep);
assert.strictEqual(dep.x, 1);
assert.deepStrictEqual(Object.getOwnPropertyDescriptor(dep, 'x'), { value: 1, writable: true, enumerable: true, configurable: false });

assert.strictEqual(Reflect.deleteProperty(dep, 'x'), false);
assert.strictEqual(Reflect.deleteProperty(dep, 'missing'), true);
assert.throws(() => { delete dep.x; }, TypeError);
assert.strictEqual(Reflect.deleteProperty(dep, Symbol.toStringTag), false);

assert.strictEqual(dep.inc(), 2);
assert.strictEqual(dep.x, 2);
assert.strictEqual(dep.b, 2);
assert.strictEqual(dep.a, dep.inc);
assert.strictEqual(Reflect.defineProperty(dep, 'x', { value: 2 }), true);
assert.strictEqual(Reflect.defineProperty(dep, 'x', { value: 1 }), false);

assert.strictEqual(Object.isFrozen(dep), false);
assert.strictEqual(Object.isSealed(dep), true);
assert.throws(() => Object.freeze(dep), TypeError);
assert.strictEqual(Object.seal(dep), dep);
assert.strictEqual(Object.getPrototypeOf(dep), null);

const starSource = 'export * from ' + JSON.stringify('data:text/javascript,' + encodeURIComponent('export const m = 1; export let c = 2; export const zz = 3; export default 9;')) + '; export const n = 4; export const b = 5; export const m = "local";';
const star = await import('data:text/javascript,' + encodeURIComponent(starSource));
assert.deepStrictEqual(Object.keys(star), ['b', 'c', 'm', 'n', 'zz']);
assert.deepStrictEqual([star.b, star.c, star.m, star.n, star.zz, star.default], [5, 2, 'local', 4, 3, undefined]);
assert.throws(() => { star.c = 1; }, TypeError);
assert.deepStrictEqual(Object.entries(star), [['b', 5], ['c', 2], ['m', 'local'], ['n', 4], ['zz', 3]]);
const forIn = [];
for (const key in star) forIn.push(key);
assert.deepStrictEqual(forIn, ['b', 'c', 'm', 'n', 'zz']);

function sum(ns) { return ns.b + ns.c + ns.n + ns.zz; }
let total = 0;
for (let i = 0; i < 20000; i++) total += sum(star);
assert.strictEqual(total, 14 * 20000);

function write(ns) { try { ns.x = 7; } catch (e) { return e instanceof TypeError; } return false; }
let refused = 0;
for (let i = 0; i < 20000; i++) refused += write(dep);
assert.strictEqual(refused, 20000);
assert.strictEqual(dep.x, 2);

const builtin = await import('node:path');
assert.throws(() => { builtin.join = 1; }, TypeError);
assert.strictEqual(Reflect.defineProperty(builtin, 'join', { value: 1 }), false);
assert.strictEqual(typeof builtin.join, 'function');

console.log('ok');
