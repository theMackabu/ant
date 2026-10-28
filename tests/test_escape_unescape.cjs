'use strict';

const assert = require('node:assert');

function units(value) {
  const out = [];
  for (let i = 0; i < value.length; i++) out.push(value.charCodeAt(i).toString(16));
  return out.join(' ');
}

assert.strictEqual(escape('é☃😀'), '%E9%u2603%uD83D%uDE00');
assert.strictEqual(escape('ABCxyz019@*_+-./'), 'ABCxyz019@*_+-./');
assert.strictEqual(escape(' !"#$%&\'(),:;<=>?[\\]^`{|}~'), '%20%21%22%23%24%25%26%27%28%29%2C%3A%3B%3C%3D%3E%3F%5B%5C%5D%5E%60%7B%7C%7D%7E');
assert.strictEqual(escape('\x00\x7f\x80\xffĀ'), '%00%7F%80%FF%u0100');
assert.strictEqual(escape('a\ud800b'), 'a%uD800b');
assert.strictEqual(escape('\udc00\ud800'), '%uDC00%uD800');
assert.strictEqual(escape('\uffff\ufeff'), '%uFFFF%uFEFF');
assert.strictEqual(escape(), 'undefined');
assert.strictEqual(escape(undefined), 'undefined');
assert.strictEqual(escape(null), 'null');
assert.strictEqual(escape(12.5), '12.5');
assert.strictEqual(escape({ toString() { return 'é x'; } }), '%E9%20x');
assert.strictEqual(escape(''), '');
assert.throws(() => escape(Symbol('s')), TypeError);

assert.strictEqual(units(unescape('%uD83D%uDE00')), 'd83d de00');
assert.strictEqual(unescape('%uD83D%uDE00'), '😀');
assert.strictEqual(unescape('%uD83D%uDE00').length, 2);
assert.strictEqual(units(unescape('%uD800')), 'd800');
assert.strictEqual(units(unescape('x%uDC00y')), '78 dc00 79');
assert.strictEqual(units(unescape('%E9')), 'e9');
assert.strictEqual(unescape('%E9'), 'é');
assert.strictEqual(unescape('%41%42'), 'AB');
assert.strictEqual(unescape('%u0041%u00e9%u2603'), 'Aé☃');
assert.strictEqual(unescape('%zz%u12%4'), '%zz%u12%4');
assert.strictEqual(unescape('%u12345'), 'ሴ5');
assert.strictEqual(unescape('%41'), 'A');
assert.strictEqual(unescape('ab%'), 'ab%');
assert.strictEqual(unescape('é%20😀'), 'é 😀');
assert.strictEqual(unescape('%uD83D') + unescape('%uDE00'), '😀');
assert.strictEqual(unescape(), 'undefined');
assert.strictEqual(unescape(null), 'null');

const samples = ['', 'plain', 'é☃😀', 'a\ud800b\udc00', '\x00\xffĀ', '%u0041 %41'];
for (const sample of samples) {
  assert.strictEqual(unescape(escape(sample)), sample);
  assert.strictEqual(units(unescape(escape(sample))), units(sample));
}

console.log('ok');
