const assert = require('node:assert');

// Integer keys on ordinary (non-array) objects are read and written straight
// from the object's shape when nothing on the prototype chain can hold or
// intercept them; accessors, read-only properties, frozen and exotic objects
// and index-bearing prototypes take the generic path. Expected values come
// from Node.

// writes
{
  const r = [];
  const t = (n, f) => { try { r.push(n + ': ' + JSON.stringify(f())); } catch (e) { r.push(n + ': ' + e.constructor.name); } };
  const loopSet = (o, n) => { for (let i = 0; i < n; i++) o[i] = i * 2; return o; };
  t('plain create+update', () => { const o = {}; loopSet(o, 5); loopSet(o, 5); return o; });
  t('existing data', () => { const o = { 0: 'a', 1: 'b' }; o[1] = 'z'; return o; });
  t('own readonly sloppy', () => { const o = {}; Object.defineProperty(o, '0', { value: 1, writable: false }); o[0] = 2; return o[0]; });
  t('own readonly strict', () => { 'use strict'; const o = {}; Object.defineProperty(o, '0', { value: 1, writable: false }); o[0] = 2; return o[0]; });
  t('own setter', () => { const log = []; const o = { set 3(v) { log.push(v); } }; o[3] = 9; return log; });
  t('frozen sloppy', () => { const o = Object.freeze({ 0: 1 }); o[0] = 2; o[1] = 3; return o; });
  t('frozen strict update', () => { 'use strict'; const o = Object.freeze({ 0: 1 }); o[0] = 2; });
  t('frozen strict add', () => { 'use strict'; const o = Object.freeze({ 0: 1 }); o[5] = 2; });
  t('sealed update', () => { const o = Object.seal({ 0: 1 }); o[0] = 2; o[1] = 3; return o; });
  t('nonext strict add', () => { 'use strict'; const o = Object.preventExtensions({}); o[0] = 1; });
  t('inherited setter', () => { const log = []; const p = { set 2(v) { log.push(v); } }; const o = Object.create(p); o[2] = 7; return [log, Object.keys(o)]; });
  t('inherited readonly', () => { const p = {}; Object.defineProperty(p, '1', { value: 0 }); const o = Object.create(p); o[1] = 5; return [o[1], Object.keys(o)]; });
  t('Object.prototype setter', () => { const log = []; Object.defineProperty(Object.prototype, '7', { set(v) { log.push(v); }, configurable: true }); const o = {}; o[7] = 1; delete Object.prototype[7]; return [log, Object.keys(o)]; });
  t('Object.prototype readonly', () => { Object.defineProperty(Object.prototype, '8', { value: 0, configurable: true }); const o = {}; o[8] = 1; delete Object.prototype[8]; return Object.keys(o); });
  t('proto later gets setter', () => { const p = {}; const o = Object.create(p); loopSet(o, 3); Object.defineProperty(p, '9', { set(v) { this.saw = v; } }); o[9] = 4; return [o.saw, Object.keys(o)]; });
  t('null proto', () => loopSet(Object.create(null), 3));
  t('proxy proto', () => { const log = []; const o = Object.create(new Proxy({}, { set(t, k, v) { log.push(k); return true; } })); o[4] = 1; return [log, Object.keys(o)]; });
  t('typed array', () => { const a = new Uint8Array(4); loopSet(a, 6); return Array.from(a); });
  t('buffer', () => { const b = Buffer.alloc(3); loopSet(b, 3); return [...b]; });
  t('function', () => { function f() {} loopSet(f, 3); return [f[0], f[2]]; });
  t('class instance', () => { class C { constructor() { this.x = 1; } } const c = new C(); loopSet(c, 3); return Object.keys(c); });
  t('large index', () => { const o = {}; o[4294967294] = 1; o[4294967295] = 2; return Object.keys(o); });
  t('key order', () => { const o = { b: 1 }; o[2] = 0; o[1] = 0; o.a = 2; return Object.keys(o); });
  t('hot loop', () => { const o = {}; for (let r = 0; r < 300; r++) for (let i = 0; i < 64; i++) o[i] = r; return [o[0], o[63], Object.keys(o).length]; });
  assert.deepStrictEqual(r, [
    "plain create+update: {\"0\":0,\"1\":2,\"2\":4,\"3\":6,\"4\":8}",
    "existing data: {\"0\":\"a\",\"1\":\"z\"}",
    "own readonly sloppy: 1",
    "own readonly strict: TypeError",
    "own setter: [9]",
    "frozen sloppy: {\"0\":1}",
    "frozen strict update: TypeError",
    "frozen strict add: TypeError",
    "sealed update: {\"0\":2}",
    "nonext strict add: TypeError",
    "inherited setter: [[7],[]]",
    "inherited readonly: [0,[]]",
    "Object.prototype setter: [[1],[]]",
    "Object.prototype readonly: []",
    "proto later gets setter: [4,[\"0\",\"1\",\"2\",\"saw\"]]",
    "null proto: {\"0\":0,\"1\":2,\"2\":4}",
    "proxy proto: [[\"4\"],[]]",
    "typed array: [0,2,4,6]",
    "buffer: [0,2,4]",
    "function: [0,4]",
    "class instance: [\"0\",\"1\",\"2\",\"x\"]",
    "large index: [\"4294967294\",\"4294967295\"]",
    "key order: [\"1\",\"2\",\"b\",\"a\"]",
    "hot loop: [299,299,64]"
  ]);
}

// reads
{
  const r = [];
  const t = (n, f) => { try { r.push(n + ': ' + JSON.stringify(f())); } catch (e) { r.push(n + ': ' + e.constructor.name); } };
  const readAll = (o, n) => { const out = []; for (let i = 0; i < n; i++) out.push(o[i]); return out; };
  const hot = (o, n) => { let last; for (let r = 0; r < 400; r++) last = readAll(o, n); return last; };
  t('own data', () => hot({ 0: 'a', 1: 'b', 3: 'd' }, 5));
  t('own getter', () => hot({ get 1() { return 'g'; } }, 3));
  t('inherited data', () => hot(Object.create({ 2: 'p' }), 4));
  t('inherited getter', () => hot(Object.create({ get 0() { return 'pg'; } }), 2));
  t('Object.prototype data', () => { Object.prototype[5] = 'OP'; const v = hot({}, 7); delete Object.prototype[5]; return v; });
  t('Object.prototype getter', () => { Object.defineProperty(Object.prototype, '1', { get() { return 'OPG'; }, configurable: true }); const v = hot({}, 3); delete Object.prototype[1]; return v; });
  t('proto added later', () => { const p = {}; const o = Object.create(p); hot(o, 3); p[1] = 'late'; return hot(o, 3); });
  t('null proto', () => hot(Object.create(null), 3));
  t('proxy proto', () => hot(Object.create(new Proxy({}, { get(t, k) { return 'px' + String(k); } })), 2));
  t('typed array', () => hot(new Uint8Array([7, 8]), 3));
  t('buffer', () => hot(Buffer.from([1, 2]), 3));
  t('string wrapper', () => hot(new String('ab'), 3));
  t('function', () => { function f() {} f[1] = 'f1'; return hot(f, 2); });
  t('class instance', () => { class C {} const c = new C(); c[0] = 'c0'; return hot(c, 2); });
  t('frozen', () => hot(Object.freeze({ 0: 'z' }), 2));
  t('after delete', () => { const o = { 0: 1, 1: 2 }; hot(o, 2); delete o[0]; return hot(o, 2); });
  t('negative/fraction', () => { const o = { '-1': 'm', '1.5': 'h' }; return [o[-1], o[1.5]]; });
  t('top-level interp', () => readAll({ 0: 'x', 2: 'y' }, 3));
  assert.deepStrictEqual(r, [
    "own data: [\"a\",\"b\",null,\"d\",null]",
    "own getter: [null,\"g\",null]",
    "inherited data: [null,null,\"p\",null]",
    "inherited getter: [\"pg\",null]",
    "Object.prototype data: [null,null,null,null,null,\"OP\",null]",
    "Object.prototype getter: TypeError",
    "proto added later: TypeError",
    "null proto: TypeError",
    "proxy proto: TypeError",
    "typed array: TypeError",
    "buffer: TypeError",
    "string wrapper: TypeError",
    "function: TypeError",
    "class instance: TypeError",
    "frozen: TypeError",
    "after delete: TypeError",
    "negative/fraction: [\"m\",\"h\"]",
    "top-level interp: TypeError"
  ]);
}

console.log('plain object index access: ok');
