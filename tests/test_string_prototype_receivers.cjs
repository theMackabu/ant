const assert = require('node:assert');

const methods = [
  'anchor', 'at', 'big', 'blink', 'bold', 'charAt', 'charCodeAt', 'codePointAt', 'concat', 'endsWith',
  'fixed', 'fontcolor', 'fontsize', 'includes', 'indexOf', 'isWellFormed', 'italics', 'lastIndexOf', 'link',
  'localeCompare', 'match', 'matchAll', 'normalize', 'padEnd', 'padStart', 'repeat', 'replace', 'replaceAll',
  'search', 'slice', 'small', 'split', 'startsWith', 'strike', 'sub', 'substr', 'substring', 'sup',
  'toLocaleLowerCase', 'toLocaleUpperCase', 'toLowerCase', 'toUpperCase', 'toWellFormed', 'trim',
  'trimEnd', 'trimStart',
];

for (const name of methods) {
  const fn = String.prototype[name];
  assert.strictEqual(typeof fn, 'function', name);
  for (const receiver of [null, undefined]) {
    assert.throws(() => fn.call(receiver, 'a', 'b'), (e) =>
      e instanceof TypeError && e.message === `String.prototype.${name} called on null or undefined`, name);
  }
}

assert.strictEqual(String.prototype.trimLeft, String.prototype.trimStart);
assert.strictEqual(String.prototype.trimRight, String.prototype.trimEnd);
assert.throws(() => String.prototype.trim.call(Symbol()), { name: 'TypeError', message: 'Cannot convert a Symbol value to a string' });

assert.strictEqual('x'.blink(), '<blink>x</blink>');
assert.strictEqual('x'.anchor('a"b'), '<a name="a&quot;b">x</a>');
assert.strictEqual('x'.link(), '<a href="undefined">x</a>');
for (const [name, length] of [['blink', 0], ['anchor', 1], ['fontcolor', 1], ['isWellFormed', 0], ['toWellFormed', 0], ['toLocaleLowerCase', 0]]) {
  assert.strictEqual(String.prototype[name].name, name);
  assert.strictEqual(String.prototype[name].length, length);
  assert.strictEqual(Object.getOwnPropertyDescriptor(String.prototype, name).enumerable, false);
}

assert.strictEqual('ab'.repeat('2'), 'abab');
assert.strictEqual('ab'.repeat(), '');
assert.strictEqual('ab'.repeat(null), '');
assert.strictEqual('ab'.repeat(2.7), 'abab');
assert.strictEqual('ab'.repeat('x'), '');
assert.throws(() => 'ab'.repeat(-1), { name: 'RangeError', message: 'Invalid count value: -1' });
assert.throws(() => 'ab'.repeat(Infinity), { name: 'RangeError', message: 'Invalid count value: Infinity' });
assert.throws(() => 'ab'.repeat(2 ** 40), { name: 'RangeError', message: 'Invalid string length' });
assert.strictEqual('a'.padStart('3'), '  a');
assert.strictEqual('a'.padStart('3', 'xy'), 'xya');
assert.strictEqual('a'.padEnd({ valueOf() { return 4; } }, 0), 'a000');
assert.strictEqual('a'.padEnd(-5), 'a');
assert.strictEqual('a'.padEnd(2 ** 31, ''), 'a');
assert.throws(() => 'a'.padEnd(3n), { name: 'TypeError', message: 'Cannot convert a BigInt value to a number' });
assert.throws(() => 'a'.padStart(2 ** 31), { name: 'RangeError', message: 'Invalid string length' });

const hi = '\uD83D', lo = '\uDE00';
const wellFormed = [
  ['', true, ''], ['abc', true, 'abc'], ['hé', true, 'hé'], ['\u{1F600}', true, '\u{1F600}'],
  [hi, false, '�'], [lo, false, '�'], ['a' + hi, false, 'a�'], [lo + 'b', false, '�b'],
  [hi + hi, false, '��'], [lo + hi, false, '��'], ['x' + hi + lo + 'y', true, 'x\u{1F600}y'],
  ['\u{1F600}'.slice(0, 1) + 'z' + '\u{1F600}'.slice(1), false, '�z�'],
  [String.fromCharCode(0xD83D, 0xDE00), true, '\u{1F600}'], [[hi, lo].join(''), true, '\u{1F600}'],
];
for (const [input, expected, fixed] of wellFormed) {
  assert.strictEqual(input.isWellFormed(), expected, JSON.stringify(input));
  assert.strictEqual(input.toWellFormed(), fixed, JSON.stringify(input));
  assert.strictEqual(input.toWellFormed().isWellFormed(), true);
}
assert.strictEqual(String.prototype.isWellFormed.call(123), true);
assert.strictEqual(String.prototype.toWellFormed.call({ toString() { return 'o' + hi; } }), 'o�');

const re = /a/;
re.exec = () => ({ 0: 'a', index: Symbol(), length: 1 });
assert.throws(() => 'xa'.replace(re, 'b'), { name: 'TypeError', message: 'Cannot convert a Symbol value to a number' });
re.exec = () => ({ 0: 'a', index: 1n, length: 1 });
assert.throws(() => 'xa'.replace(re, 'b'), { name: 'TypeError', message: 'Cannot convert a BigInt value to a number' });

console.log('PASS String.prototype receivers');
