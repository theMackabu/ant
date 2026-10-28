const assert = require('node:assert');

const hi = '\ud83d', lo = '\ude00', emoji = hi + lo;
const units = s => Array.from({ length: s.length }, (_, i) => s.charCodeAt(i).toString(16));
const same = (actual, expected, label) => assert.deepStrictEqual(units(actual), units(expected), label);

assert.strictEqual(emoji.indexOf(hi), 0);
assert.strictEqual(emoji.indexOf(lo), 1);
assert.strictEqual(emoji.lastIndexOf(lo), 1);
assert.strictEqual(emoji.lastIndexOf(hi), 0);
assert.strictEqual(('\udbff\udfff\udbff').lastIndexOf('\udbff'), 2);
assert.strictEqual(('\udbff\udfff\udbff').indexOf('\udbff'), 0);
assert.strictEqual(('\udbff\udfff\udbff').lastIndexOf('\udbff', 1), 0);
assert.strictEqual(('a' + emoji + 'b').indexOf(lo + 'b'), 2);
assert.strictEqual(('a' + emoji + 'b').indexOf('a' + hi), 0);
assert.strictEqual(('x' + emoji + emoji).lastIndexOf(lo + hi), 2);
assert.strictEqual((emoji + emoji).indexOf(lo, 2), 3);
assert.strictEqual((emoji + emoji).indexOf(emoji, 1), 2);
assert.strictEqual((emoji + 'a').indexOf('a', 1), 2);
assert.strictEqual((emoji + emoji).lastIndexOf(emoji, 1), 0);
assert.strictEqual((emoji + emoji).lastIndexOf(hi, 1), 0);
assert.strictEqual(emoji.indexOf('', 1), 1);
assert.strictEqual(emoji.lastIndexOf('', 1), 1);
assert.strictEqual(('☃' + emoji).lastIndexOf(lo, 2), 2);

assert.strictEqual(emoji.includes(lo), true);
assert.strictEqual(emoji.includes(hi), true);
assert.strictEqual(emoji.includes(lo, 1), true);
assert.strictEqual(emoji.includes(hi, 1), false);
assert.strictEqual('éa'.includes('a', 2), false);
assert.strictEqual('éa'.includes('a', 1), true);
assert.strictEqual(('\udfff☃').includes('☃', 3), false);
assert.strictEqual('undefined'.includes(), true);
assert.strictEqual(emoji.startsWith(lo, 1), true);
assert.strictEqual(emoji.endsWith(hi, 1), true);

assert.strictEqual('a1b1'.lastIndexOf(1), 3);
assert.strictEqual('a1b1'.lastIndexOf('1', '2'), 1);
assert.strictEqual('a1b1'.lastIndexOf('1', NaN), 3);
assert.strictEqual('a1b1'.lastIndexOf('1', { valueOf: () => 2 }), 1);
assert.strictEqual('xundefinedx'.lastIndexOf(), 1);

assert.deepStrictEqual(emoji.split(lo).map(units), [['d83d'], []]);
assert.deepStrictEqual(emoji.split(hi).map(units), [[], ['de00']]);
assert.deepStrictEqual(('b\ud800\ude00\udbff\udfff').split('\udfff').map(units), [['62', 'd800', 'de00', 'dbff'], []]);
assert.deepStrictEqual(('𐀀𐀀').split('\udc00\ud800').map(units), [['d800'], ['dc00']]);
assert.deepStrictEqual(('a' + emoji + 'a' + emoji).split(lo, 1).map(units), [['61', 'd83d']]);
assert.deepStrictEqual('a,b,,c'.split(','), ['a', 'b', '', 'c']);
assert.deepStrictEqual('é,é'.split(','), ['é', 'é']);

same(emoji.replace(lo, '[$&]'), hi + '[' + lo + ']');
same(emoji.replace(hi, '[$&]'), '[' + hi + ']' + lo);
same(emoji.replace(lo, lo), emoji);
same(emoji.replaceAll(lo, '<$`>'), hi + '<' + hi + '>');
same((emoji + 'x' + emoji).replaceAll(hi, '$\''), lo + 'x' + emoji + lo + 'x' + lo + lo);
same((emoji + emoji).replaceAll(lo + hi, (m, i) => '{' + i + '}'), hi + '{1}' + lo);
same('\udbff\udfff\udbff'.replace('\udbff', '[$&]'), '[\udbff]\udfff\udbff');
same(emoji.replace('\udc00', '!'), emoji);
assert.strictEqual(emoji.replace(lo, ''), hi);
assert.strictEqual(emoji.replaceAll(hi, '').replace('', hi), emoji);
same('abcb'.replaceAll('b', '<$`>'), 'a<a>c<abc>');

assert.strictEqual('abc'.at(), 'a');
assert.strictEqual('abc'.at(NaN), 'a');
assert.strictEqual('abc'.at(-1), 'c');
assert.strictEqual('abc'.at(Infinity), undefined);
assert.strictEqual('abc'.at('1'), 'b');
same(emoji.at(1), lo);
same(emoji.at(-2), hi);

const lone = 'a' + lo + 'b' + hi;
const forOf = [];
for (const ch of lone) forOf.push(ch);
assert.deepStrictEqual(forOf.map(units), [['61'], ['de00'], ['62'], ['d83d']]);
assert.deepStrictEqual([...lone].map(units), [['61'], ['de00'], ['62'], ['d83d']]);
assert.deepStrictEqual(Array.from(lone + emoji).map(units), [['61'], ['de00'], ['62'], ['d83d'], ['d83d', 'de00']]);
assert.deepStrictEqual([...lo + hi].map(units), [['de00'], ['d83d']]);

assert.strictEqual('\ude00' < '😀', false);
assert.strictEqual('😀' < '\ude00', true);
assert.strictEqual('\uffff' < emoji, false);
assert.strictEqual(emoji < '\uffff', true);
assert.strictEqual('\ue000' > '𐀀', true);
assert.strictEqual(hi + 'z' < emoji, true);
assert.strictEqual(hi + '\uffff' < emoji, false);
assert.strictEqual(hi < emoji, true);
assert.strictEqual('😀' <= '😁', true);
assert.strictEqual('é' < '☃', true);
assert.strictEqual('ab' < 'b', true);
assert.deepStrictEqual(['\uffff', emoji, lo, 'a', '\ue000'].sort().map(units),
  [['61'], ['d83d', 'de00'], ['de00'], ['e000'], ['ffff']]);

same(''.padEnd(1, emoji + '☃'), hi);
same(''.padStart(3, '𐀀\ud800'), '𐀀\ud800');
same(''.padEnd(5, '𐀀\ud800'), '𐀀\ud800𐀀');
same('ab☃'.padStart(7, lo + 'aé' + emoji), lo + 'aé' + hi + 'ab☃');
same('x'.padEnd('3', 'y'), 'xyy');
same(lo.padStart(2, emoji), emoji);
assert.strictEqual('abc'.padEnd(2, 'x'), 'abc');
assert.throws(() => 'a'.padEnd(Infinity, 'b'), RangeError);
assert.strictEqual('a'.padEnd(Infinity, ''), 'a');

assert.strictEqual(JSON.stringify(lo + 'a' + hi), '"\\ude00a\\ud83d"');
assert.strictEqual(JSON.stringify(hi + lo), '"' + emoji + '"');

let built = '';
for (const part of [hi, lo, 'x', hi]) built += part;
assert.strictEqual(built.indexOf(lo), 1);
assert.strictEqual(built.lastIndexOf(hi), 3);
assert.strictEqual(built.lastIndexOf(hi, 2), 0);
assert.strictEqual(new Set([built.slice(0, 2), emoji]).size, 1);

const big = ('é' + emoji + 'ab').repeat(200);
assert.strictEqual(big.lastIndexOf(lo), big.length - 3);
assert.strictEqual(big.indexOf(lo, 600), 602);
assert.strictEqual(big.indexOf('ab', 600), 603);
assert.strictEqual(big.split(lo).length, 201);
assert.strictEqual(big.replaceAll(hi, '').length, big.length - 200);

console.log('ok');
