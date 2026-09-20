const assert = require('node:assert');
const { domainToASCII } = require('node:url');

assert.strictEqual(typeof domainToASCII, 'function');
assert.strictEqual(domainToASCII.length, 1);
assert.strictEqual(require('url').domainToASCII, domainToASCII);

for (const [input, expected] of [
  ['', ''],
  ['ExAmPlE.COM', 'example.com'],
  ['münich.com', 'xn--mnich-kva.com'],
  ['MÜNICH.com', 'xn--mnich-kva.com'],
  ['日本.jp', 'xn--wgv71a.jp'],
  ['faß.de', 'xn--fa-hia.de'],
  ['xn--mnich-kva.com', 'xn--mnich-kva.com'],
  ['ＡＢ.com', 'ab.com'],
  ['example。com.', 'example.com.'],
  ['%65xample.com', 'example.com'],
  ['m%C3%BCnich.com', 'xn--mnich-kva.com'],
  ['example.com/path', 'example.com'],
  ['example.com?query', 'example.com'],
  ['example.com#hash', 'example.com'],
  ['example.com\\path', 'example.com'],
  ['exa\tmple.com\r\n', 'example.com'],
  ['127.1', '127.0.0.1'],
  ['0x7f.1', '127.0.0.1'],
  ['[0:0:0:0:0:0:0:1]', '[::1]'],
  ['a'.repeat(64) + '.com', 'a'.repeat(64) + '.com'],
  ['-example_.com', '-example_.com'],
  [undefined, 'undefined'],
  [null, 'null'],
  [true, 'true'],
  [123, '0.0.0.123'],
  [123n, '0.0.0.123'],
  [new String('münich.com'), 'xn--mnich-kva.com'],
  [{ toString() { return 'münich.com'; } }, 'xn--mnich-kva.com'],
]) {
  assert.strictEqual(domainToASCII(input), expected);
}

for (const input of [
  'a\u200cb.com', 'xn--iñvalid.com', 'exa mple.com', 'example.com:80',
  'user@example.com', '1.2.3.999', '[::invalid]', '%', '%FF',
  'example.com\0suffix', '\ud800', '\udc00',
]) {
  assert.strictEqual(domainToASCII(input), '', JSON.stringify(input));
}

assert.throws(() => domainToASCII(), {
  name: 'TypeError',
  code: 'ERR_MISSING_ARGS',
  message: 'The "domain" argument must be specified',
});
for (const input of [
  Symbol('domain'), Object(Symbol('domain')),
  { [Symbol.toPrimitive]() { return Symbol('domain'); } },
  { toString() { return {}; }, valueOf() { return {}; } },
]) {
  assert.throws(() => domainToASCII(input), TypeError);
}

let conversions = 0;
assert.strictEqual(domainToASCII({
  [Symbol.toPrimitive](hint) {
    assert.strictEqual(hint, 'string');
    conversions++;
    return 'münich.com';
  },
}), 'xn--mnich-kva.com');
assert.strictEqual(conversions, 1);

for (const reason of [undefined, null, new Error('domain coercion')]) {
  for (const input of [
    { toString() { throw reason; } },
    { [Symbol.toPrimitive]() { throw reason; } },
  ]) {
    let caught = false;
    try {
      domainToASCII(input);
    } catch (error) {
      caught = true;
      assert.strictEqual(error, reason);
    }
    assert.ok(caught, 'domainToASCII must propagate coercion errors');
  }
}

assert.strictEqual(domainToASCII('münich.com', Symbol('ignored')), 'xn--mnich-kva.com');
console.log('url.domainToASCII tests passed');
