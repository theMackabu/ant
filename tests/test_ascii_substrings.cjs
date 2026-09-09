const assert = require('node:assert');

function check(text, expected) {
  assert.strictEqual(text, expected);
  assert.strictEqual(text.length, expected.length);
  assert.deepStrictEqual(Array.from(text, c => c.codePointAt(0)), Array.from(expected, c => c.codePointAt(0)));
  assert.strictEqual(Buffer.from(text).toString('hex'), Buffer.from(expected).toString('hex'));
}

for (const body of ['alpha\0beta', 'éalpha😀beta', 'ascii']) {
  const parent = ' \t' + body + '\r\n ';
  check(parent.trim(), body);
  check(parent.trimStart(), body + '\r\n ');
  check(parent.trimEnd(), ' \t' + body);
  const pieces = (body + '|' + body + '|').split('|');
  assert.deepStrictEqual(pieces, [body, body, '']);
  for (const piece of pieces) check(piece, piece === '' ? '' : body);
}

for (const parent of ['alpha-beta', 'éalpha-beta😀']) {
  const captures = /(alpha)-(beta)(z)?/.exec(parent);
  check(captures[0], 'alpha-beta');
  check(captures[1], 'alpha');
  check(captures[2], 'beta');
  assert.strictEqual(captures[3], undefined);
  check(RegExp.$1, 'alpha');
  check(RegExp.lastMatch, 'alpha-beta');
  assert.deepStrictEqual(parent.match(/alpha|beta/g), ['alpha', 'beta']);
  let callbackMatch;
  parent.replace(/(alpha)/, match => { callbackMatch = match; return 'A'; });
  check(callbackMatch, 'alpha');
}

check(/(é😀)/u.exec('beforeé😀after')[1], 'é😀');
const named = /(?<part>ascii)/.exec('éasciiz');
check(named.groups.part, 'ascii');
assert.deepStrictEqual('a\0b,c'.split(/(,)/), ['a\0b', ',', 'c']);
assert.deepStrictEqual('é|ascii|😀'.split(/(\|)/u), ['é', '|', 'ascii', '|', '😀']);
assert.deepStrictEqual('ab'.split(/(z)?b/), ['a', undefined, '']);
check(/()/.exec('ascii')[1], '');
check(' \t\n'.trim(), '');

// Larger materialized/rope parents and enough copies to exercise collection.
const long = 'x'.repeat(8192);
for (let i = 0; i < 1000; i++) {
  const parent = ' ' + long + ' ';
  check(parent.trim(), long);
  check(/(x+)/.exec(parent)[1], long);
  assert.strictEqual((long + ',' + long).split(',')[1].length, long.length);
}
console.log('PASS ASCII substring metadata, mixed Unicode, captures, trim and split');
