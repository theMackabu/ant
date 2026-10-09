const assert = require('node:assert');

// Symbol-keyed sets walk the prototype chain through the shape of each
// ordinary object: inherited setters, getters and read-only properties
// decide the outcome, and a miss defines an own property on the receiver.
// Expected values come from Node.
const expected = [
  "own data: 2",
  "new prop: [3,1]",
  "inherited setter: [[4],0]",
  "inherited getter only sloppy: 1",
  "inherited getter only strict: TypeError",
  "inherited non-writable sloppy: [1,0]",
  "inherited non-writable strict: TypeError",
  "own non-writable strict: TypeError",
  "inherited writable: [7,1]",
  "array: [8,1]",
  "function: 9",
  "frozen sloppy: 0",
  "frozen strict: TypeError",
  "null proto: 10",
  "setPrototypeOf later: 11",
  "iterator: [1]",
  "toStringTag: \"[object X]\"",
  "class proto setter: 12",
  "array proto symbol: 1"
];

const s = Symbol('s'); const r = [];
const t = (n, f) => { try { r.push(n + ': ' + JSON.stringify(f())); } catch (e) { r.push(n + ': ' + e.constructor.name); } };
t('own data', () => { const o = { [s]: 1 }; o[s] = 2; return o[s]; });
t('new prop', () => { const o = {}; o[s] = 3; return [o[s], Object.getOwnPropertySymbols(o).length]; });
t('inherited setter', () => { const log = []; const p = { set [s](v) { log.push(v); } }; const o = Object.create(p); o[s] = 4; return [log, Object.getOwnPropertySymbols(o).length]; });
t('inherited getter only sloppy', () => { const p = { get [s]() { return 1; } }; const o = Object.create(p); o[s] = 5; return o[s]; });
t('inherited getter only strict', () => { 'use strict'; const p = { get [s]() { return 1; } }; const o = Object.create(p); o[s] = 5; return o[s]; });
t('inherited non-writable sloppy', () => { const p = {}; Object.defineProperty(p, s, { value: 1 }); const o = Object.create(p); o[s] = 6; return [o[s], Object.getOwnPropertySymbols(o).length]; });
t('inherited non-writable strict', () => { 'use strict'; const p = {}; Object.defineProperty(p, s, { value: 1 }); const o = Object.create(p); o[s] = 6; return o[s]; });
t('own non-writable strict', () => { 'use strict'; const o = {}; Object.defineProperty(o, s, { value: 1 }); o[s] = 2; return o[s]; });
t('inherited writable', () => { const p = { [s]: 1 }; const o = Object.create(p); o[s] = 7; return [o[s], p[s]]; });
t('array', () => { const a = [1]; a[s] = 8; return [a[s], a.length]; });
t('function', () => { function f() {} f[s] = 9; return f[s]; });
t('frozen sloppy', () => { const o = Object.freeze({}); o[s] = 1; return Object.getOwnPropertySymbols(o).length; });
t('frozen strict', () => { 'use strict'; const o = Object.freeze({}); o[s] = 1; return 1; });
t('null proto', () => { const o = Object.create(null); o[s] = 10; return o[s]; });
t('setPrototypeOf later', () => { const o = {}; Object.setPrototypeOf(o, { set [s](v) { this.got = v; } }); o[s] = 11; return o.got; });
t('iterator', () => { const o = {}; o[Symbol.iterator] = function* () { yield 1; }; return [...o]; });
t('toStringTag', () => { const o = {}; o[Symbol.toStringTag] = 'X'; return String(o); });
t('class proto setter', () => { class A { set [s](v) { this.v = v; } } const a = new A(); a[s] = 12; return a.v; });
t('array proto symbol', () => { Array.prototype[s] = 0; const a = []; a[s] = 13; const own = Object.getOwnPropertySymbols(a).length; delete Array.prototype[s]; return own; });

assert.deepStrictEqual(r, expected);
console.log('symbol set semantics: ok');
