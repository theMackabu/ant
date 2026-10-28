const assert = require('node:assert');

const warm = 300;

function describe(value) {
  if (Object.is(value, -0)) return '-0';
  if (typeof value === 'bigint') return `${value}n`;
  if (typeof value === 'string') return JSON.stringify(value);
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

const codes = (s) => Array.from(s, (c) => c.codePointAt(0).toString(16)).join(' ');

check('joining cyclic arrays', () => {
  const a = [1, 2];
  a.push(a);
  const b = [1, [2, 3]];
  b[1].push(b);
  const c = [1, { toString() { return c.join('+'); } }, 3];
  const sparse = [1, 2];
  sparse[5] = sparse;
  return [String(a), a.join('-'), a.toString(), a.toLocaleString(), `${a}`, String(b), String(b[1]), c.join('|'), sparse.join()];
}, ['1,2,', '1-2-', '1,2,', '1,2,', '1,2,', '1,2,3,', '2,3,1,', '1||3', '1,2,,,,']);

check('Object called as a function', () => {
  const o = { a: 1 };
  class Sub extends Object { constructor(v) { super(v); this.k = 1; } }
  const sub = new Sub(5);
  const holder = { Object };
  return [
    Object.getPrototypeOf(Object()) === Object.prototype, Object.getPrototypeOf(Object(null)) === Object.prototype,
    Object.getPrototypeOf(Object(undefined)) === Object.prototype, Object.getPrototypeOf(new Object()) === Object.prototype,
    Object(o) === o, new Object(o) === o, typeof Object(1), Object('s') instanceof String,
    sub instanceof Sub, sub.k, holder.Object() === holder,
  ];
}, [true, true, true, true, true, true, 'object', true, true, 1, false]);

function maxOf(...values) { return Math.max(...values); }
function minOf(...values) { return Math.min(...values); }

check('Math.max and Math.min coercion', () => {
  const order = [];
  const value = (name, v) => ({ valueOf() { order.push(name); return v; } });
  const r = [maxOf(NaN, value('a', 4)), minOf(NaN, value('b', 4)), maxOf(value('c', NaN), value('d', 4), value('e', 1))];
  r.push(order.join(''));
  r.push(attempt(() => maxOf(1, { valueOf() { throw new Error('boom'); } }, value('f', 1))), order.join(''));
  r.push(maxOf(), minOf(), maxOf(-0, 0), minOf(0, -0), maxOf('3', 2), minOf(true, 2), Math.max(1, 2, 3), Math.min(3, 1, 2));
  return r;
}, [NaN, NaN, NaN, 'abcde', 'threw Error boom', 'abcde', -Infinity, Infinity, 0, -0, 3, 1, 3, 1]);

check('this of plain builtin calls', () => {
  const up = String.prototype.toUpperCase;
  const ts = Object.prototype.toString;
  const valueOf = Object.prototype.valueOf;
  const join = Array.prototype.join;
  const call = (f, ...args) => f(...args);
  return [
    attempt(() => up('a')), ts(), (function () { 'use strict'; return ts(); })(), call(ts), ts.call(null),
    attempt(() => valueOf()), attempt(() => join()), attempt(() => call(up)), parseInt('12'), Math.max(1, 2),
  ];
}, ['threw TypeError', '[object Undefined]', '[object Undefined]', '[object Undefined]', '[object Null]',
  'threw TypeError', 'threw TypeError', 'threw TypeError', 12, 2]);

const stringMethods = [
  ['charCodeAt', [0]], ['charAt', [0]], ['codePointAt', [0]], ['at', [0]], ['toUpperCase', []], ['toLowerCase', []],
  ['toLocaleUpperCase', []], ['trim', []], ['trimStart', []], ['trimEnd', []], ['slice', [0, 3]], ['substring', [0, 3]],
  ['substr', [0, 3]], ['indexOf', ['x']], ['lastIndexOf', ['x']], ['includes', ['x']], ['startsWith', ['x']],
  ['endsWith', ['x']], ['split', [',']], ['repeat', [1]], ['padStart', [3]], ['padEnd', [3]], ['concat', ['x']],
  ['normalize', []], ['localeCompare', ['x']], ['search', ['x']], ['match', ['x']], ['replace', ['x', 'y']],
  ['replaceAll', ['x', 'y']], ['anchor', ['x']],
];

check('String.prototype methods on null and undefined', () => {
  const r = [];
  for (const [name, args] of stringMethods)
    for (const receiver of [null, undefined])
      if (attempt(() => String.prototype[name].apply(receiver, args)) !== 'threw TypeError') r.push(`${name}(${receiver})`);
  r.push(String.prototype.toUpperCase.call(12), String.prototype.slice.call(true, 1), String.prototype.charAt.call(45, 1));
  return r;
}, ['12', 'rue', '5']);

check('full case mapping', () => [
  codes('ß'.toUpperCase()), codes('ﬁ'.toUpperCase()), codes('İ'.toLowerCase()), codes('Σ'.toLowerCase()),
  codes('ΑΣ'.toLowerCase()), codes('ΑΣ Β'.toLowerCase()), codes('ΑΣ.'.toLowerCase()), codes('aΣb'.toLowerCase()),
  codes('ᾳ'.toUpperCase()), codes('ŉ'.toUpperCase()), codes('straße'.toLocaleUpperCase()), codes('ΌΣΟΣ'.toLowerCase()),
  codes('ÁΣ'.toLowerCase()), 'ABC'.toLowerCase(), 'abc'.toUpperCase(), 'ǅ'.toUpperCase(), 'ǅ'.toLowerCase(),
], ['53 53', '46 49', '69 307', '3c3', '3b1 3c2', '3b1 3c2 20 3b2', '3b1 3c2 2e', '61 3c3 62', '391 399', '2bc 4e',
  '53 54 52 41 53 53 45', '3cc 3c3 3bf 3c2', '61 301 3c2', 'abc', 'ABC', 'Ǆ', 'ǆ']);

check('trimming whitespace', () => [
  '\t\n ﻿x '.trim(), ' 　 x    '.trim(), '᠎x'.trim(), '​x'.trim(), ' x '.trimStart(),
  ' x '.trimEnd(), '\v\fx\v\f'.trim(), 'ठ'.trim(), '     y '.trim(), ''.trim(),
], ['x', 'x', '᠎x', '​x', 'x ', ' x', 'x', 'ठ', 'y', '']);

check('typed array canonical numeric keys', () => {
  const r = [];
  for (const key of [-1, 3, 0.5, '-0', NaN, '1.5', 'Infinity', '-Infinity', '1e3', '01', -0, '1', 1, '12345678901234567890']) {
    const a = new Uint32Array(3);
    a[key] = 4294967295;
    r.push(`${String(key)}:${a[key]}:${Object.getOwnPropertyNames(a).filter((k) => !['0', '1', '2', 'BYTES_PER_ELEMENT', 'buffer'].includes(k)).join('|')}:${a[0]}${a[1]}`);
  }
  let calls = 0;
  const b = new Int8Array(3);
  b[5] = { valueOf() { calls++; return 1; } };
  r.push(calls);
  const c = new Uint8ClampedArray(2);
  r.push(attempt(() => { c[0] = 1n; return 'stored'; }), attempt(() => { c[7] = 1n; return 'stored'; }));
  const d = new BigInt64Array(2);
  r.push(attempt(() => { d[0] = 1; return 'stored'; }), attempt(() => { d[9] = 1; return 'stored'; }));
  d[1] = 5n;
  r.push(d[1]);
  return r;
}, ['-1:undefined::00', '3:undefined::00', '0.5:undefined::00', '-0:undefined::00', 'NaN:undefined::00',
  '1.5:undefined::00', 'Infinity:undefined::00', '-Infinity:undefined::00', '1e3:4294967295:1e3:00',
  '01:4294967295:01:00', '0:4294967295::42949672950', '1:4294967295::04294967295', '1:4294967295::04294967295',
  '12345678901234567890:4294967295:12345678901234567890:00', 1, 'threw TypeError', 'threw TypeError', 'threw TypeError', 'threw TypeError', 5n]);

const slice = [].slice;
function argsSlice(start) { return slice.call(arguments, start); }

check('slice argument conversion', () => {
  const order = [];
  const value = (name, v) => ({ valueOf() { order.push(name); return v; } });
  return [
    argsSlice('1', 2), argsSlice(1.5, 2), argsSlice(10, 1), [0, 1, 2].slice('1'), [0, 1, 2].slice(1.5),
    slice.call({ 0: 0, 1: 1, length: 2 }, '1'), [0, 1, 2].slice(-1.5), [0, 1, 2].slice(0, -1.5), [0, 1, 2].slice(NaN, '2'),
    [0, 1, 2].slice(undefined, undefined), [0, 1, 2].slice(null, Infinity), [0, 1, 2].slice(value('s', 1), value('e', 2)),
    order.join(''), attempt(() => [1].slice(Symbol('s'))), attempt(() => [1].slice(1n)), 'abcdef'.slice('1', '3'),
    'abcdef'.slice(-2.5), 'abcdéf'.slice(value('a', 1)), 'abc'.slice(1.9, 2.9), order.join(''),
  ];
}, [[2], [2], [], [1, 2], [1, 2], [1], [2], [0, 1], [0, 1], [0, 1, 2], [0, 1, 2], [1], 'se', 'threw TypeError',
  'threw TypeError', 'bc', 'ef', 'bcdéf', 'b', 'sea']);

check('RegExp lastIndex conversion', () => {
  const r = [];
  const g = /a/g;
  let calls = 0;
  g.lastIndex = { valueOf() { calls++; return 1; } };
  const m = g.exec('aa');
  r.push(m && m.index, calls, g.lastIndex);
  const plain = /a/;
  let plainCalls = 0;
  plain.lastIndex = { valueOf() { plainCalls++; return 5; } };
  r.push(plain.exec('ba').index, plainCalls);
  const negative = /a/g;
  negative.lastIndex = -3;
  r.push(negative.exec('ab').index, negative.lastIndex);
  const past = /a/g;
  past.lastIndex = 10;
  r.push(past.exec('ab'), past.lastIndex);
  const fraction = /a/g;
  fraction.lastIndex = 1.7;
  r.push(fraction.exec('aaa').index, fraction.lastIndex);
  const sticky = /a/y;
  sticky.lastIndex = '1';
  r.push(sticky.test('ba'), sticky.lastIndex);
  const throwing = /a/g;
  throwing.lastIndex = { valueOf() { throw new Error('boom'); } };
  r.push(attempt(() => throwing.exec('a')));
  return r;
}, [1, 1, 2, 1, 1, 0, 1, null, 0, 1, 2, true, 2, 'threw Error boom']);

function put(array, index, value) { array[index] = value; return array[index]; }
function strictPut(array, index, value) { 'use strict'; array[index] = value; return array[index]; }

check('writes to non-writable elements', () => {
  const r = [];
  const hole = [];
  Object.defineProperty(hole, 1, { value: 9, writable: false, enumerable: false, configurable: false });
  r.push(put(hole, 1, 100), attempt(() => strictPut(hole, 1, 5)), hole.length);
  const first = [];
  Object.defineProperty(first, 0, { value: 9, writable: false });
  r.push(put(first, 0, 100));
  const big = new Array(1024);
  Object.defineProperty(big, 769, { value: 9, writable: false });
  r.push(put(big, 769, 100));
  const filled = new Array(1024);
  Object.defineProperty(filled, 769, { value: 9, writable: false });
  for (let i = 0; i < 769; ++i) filled[i] = i;
  r.push(put(filled, 769, 100), Object.keys(filled).includes('769'), filled[768]);
  const dense = [1, 2, 3];
  Object.defineProperty(dense, 1, { value: 9, writable: false });
  r.push(put(dense, 1, 100), put(dense, 2, 7));
  const proto = {};
  Object.defineProperty(proto, 'x', { value: 1, writable: false });
  const child = Object.create(proto);
  r.push(put(child, 'x', 2), Object.hasOwn(child, 'x'), attempt(() => strictPut(child, 'x', 3)));
  const arrayProto = [];
  Object.defineProperty(arrayProto, 0, { value: 7, writable: false });
  const arrayChild = Object.create(arrayProto);
  r.push(put(arrayChild, 0, 8), Object.hasOwn(arrayChild, '0'));
  return r;
}, [9, 'threw TypeError', 2, 9, 9, 9, false, 768, 9, 7, 1, false, 'threw TypeError', 7, false]);

console.log('PASS spec builtin semantics');
