'use strict';

const assert = require('node:assert');
const qs = require('node:querystring');

const plain = value => (value && typeof value === 'object' ? { ...value } : value);

assert.deepStrictEqual(Object.keys(qs).sort(), ['decode', 'encode', 'escape', 'parse', 'stringify', 'unescape', 'unescapeBuffer']);
assert.strictEqual(qs.decode, qs.parse);
assert.strictEqual(qs.encode, qs.stringify);

assert.strictEqual(qs.unescape('a+b'), 'a+b');
assert.strictEqual(qs.unescape('a%20b+c'), 'a b+c');
assert.strictEqual(qs.unescape('a+b%'), 'a+b%');
assert.strictEqual(qs.unescape('a+b%', true), 'a b%');
assert.strictEqual(qs.unescape('%zz%41'), '%zzA');
assert.strictEqual(qs.unescape('%E2%82'), '�');
assert.strictEqual(qs.unescape('%E2%82%AC'), '€');
assert.strictEqual(qs.unescape('é%'), '�%');
assert.strictEqual(qs.unescape('%%41'), '%A');
assert.deepStrictEqual([...qs.unescapeBuffer('a%2Bb+%zz')], [0x61, 0x2b, 0x62, 0x2b, 0x25, 0x7a, 0x7a]);
assert.deepStrictEqual([...qs.unescapeBuffer('a+b', true)], [0x61, 0x20, 0x62]);

assert.deepStrictEqual(plain(qs.parse('a+b=c+d&e=%2B')), { 'a b': 'c d', e: '+' });
assert.deepStrictEqual(plain(qs.parse('a=1&a=2&a=3&b')), { a: ['1', '2', '3'], b: '' });
assert.deepStrictEqual(plain(qs.parse('a=%zz&b=%E2%82&c=%')), { a: '%zz', b: '�', c: '%' });
assert.deepStrictEqual(plain(qs.parse('a:1;b:2', ';', ':')), { a: '1', b: '2' });
assert.deepStrictEqual(plain(qs.parse('a==1&&b', '&&', '==')), { a: '1', b: '' });
assert.deepStrictEqual(plain(qs.parse('a=1&b=2&c=3', null, null, { maxKeys: 2 })), { a: '1', b: '2' });
assert.deepStrictEqual(plain(qs.parse('a=1&b=2&c=3', null, null, { maxKeys: 0 })), { a: '1', b: '2', c: '3' });
assert.deepStrictEqual(plain(qs.parse('a+b=c', null, null, { decodeURIComponent: v => `<${v}>` })), { '<a%20b>': '<c>' });
assert.deepStrictEqual(plain(qs.parse('a%41=b+%2', null, null, { decodeURIComponent() { throw new Error('x'); } })), { aA: 'b %2' });
assert.deepStrictEqual(plain(qs.parse(123)), {});
assert.deepStrictEqual(plain(qs.parse('')), {});
assert.strictEqual(Object.getPrototypeOf(qs.parse('a=1')), null);

assert.strictEqual(qs.escape('a b+c/é'), 'a%20b%2Bc%2F%C3%A9');
assert.strictEqual(qs.escape(1), '1');
assert.strictEqual(qs.escape(null), 'null');
assert.strictEqual(qs.escape('\uD800a'), '%F0%90%81%A1');
assert.strictEqual(qs.escape('\uDC00\uD800'), '%F0%90%80%80');
assert.strictEqual(qs.escape('😀'), '%F0%9F%98%80');
assert.throws(() => qs.escape('a\uD800'), err => err instanceof URIError && err.code === 'ERR_INVALID_URI' && err.message === 'URI malformed');
assert.throws(() => qs.escape(Symbol('s')), TypeError);

assert.strictEqual(
  qs.stringify({ a: 'b c', n: 1, big: 1e21, nan: NaN, t: true, o: {}, u: undefined, z: null, arr: ['x', 2], empty: [], bi: 10n }),
  'a=b%20c&n=1&big=1e%2B21&nan=&t=true&o=&u=&z=&arr=x&arr=2&bi=10'
);
assert.strictEqual(qs.stringify({ a: 1, b: 2 }, ';', ':'), 'a:1;b:2');
assert.strictEqual(qs.stringify({ a: 'x y' }, null, null, { encodeURIComponent: v => `[${v}]` }), '[a]=[x y]');
assert.strictEqual(qs.stringify('abc'), '');
assert.strictEqual(qs.stringify(null), '');

const origUnescape = qs.unescape;
qs.unescape = v => `U(${v})`;
assert.deepStrictEqual(plain(qs.parse('a=%20')), { 'U(a)': 'U(%20)' });
qs.unescape = origUnescape;
const origEscape = qs.escape;
qs.escape = v => `E(${v})`;
assert.strictEqual(qs.stringify({ a: 'b' }), 'E(a)=E(b)');
qs.escape = origEscape;

console.log('querystring node compat tests passed');
