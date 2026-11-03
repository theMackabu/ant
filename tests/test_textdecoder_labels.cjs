const assert = require('node:assert');

const supported = {
  'utf-8': ['unicode-1-1-utf-8', 'unicode11utf8', 'unicode20utf8', 'utf-8', 'utf8', 'x-unicode20utf8'],
  'iso-8859-2': ['csisolatin2', 'iso-8859-2', 'iso-ir-101', 'iso8859-2', 'iso88592', 'iso_8859-2', 'iso_8859-2:1987', 'l2', 'latin2'],
  'windows-1252': [
    'ansi_x3.4-1968', 'ascii', 'cp1252', 'cp819', 'csisolatin1', 'ibm819', 'iso-8859-1', 'iso-ir-100', 'iso8859-1',
    'iso88591', 'iso_8859-1', 'iso_8859-1:1987', 'l1', 'latin1', 'us-ascii', 'windows-1252', 'x-cp1252',
  ],
  'utf-16be': ['unicodefffe', 'utf-16be'],
  'utf-16le': ['csunicode', 'iso-10646-ucs-2', 'ucs-2', 'unicode', 'unicodefeff', 'utf-16', 'utf-16le'],
};

for (const Ctor of [TextDecoder, TextDecoderStream]) {
  for (const [name, labels] of Object.entries(supported)) {
    for (const label of labels) {
      assert.strictEqual(new Ctor(label).encoding, name, `${Ctor.name} ${label}`);
      assert.strictEqual(new Ctor(label.toUpperCase()).encoding, name, `${Ctor.name} ${label} upper`);
      assert.strictEqual(new Ctor(` \t\n${label}\f\r `).encoding, name, `${Ctor.name} ${label} padded`);
    }
  }

  for (const label of ['csiso2022kr', 'hz-gb-2312', 'iso-2022-cn', 'iso-2022-cn-ext', 'iso-2022-kr', 'replacement', 'bogus', '', 'utf-8\u0000', ' utf-8'])
    assert.throws(() => new Ctor(label), RangeError, `${Ctor.name} ${JSON.stringify(label)}`);

  assert.strictEqual(new Ctor().encoding, 'utf-8');
  assert.strictEqual(new Ctor(undefined).encoding, 'utf-8');
  assert.strictEqual(new Ctor({ toString: () => 'latin1' }).encoding, 'windows-1252');
}

assert.strictEqual(new TextDecoder('latin2').decode(Uint8Array.of(0xa1, 0xb1)), 'Ąą');
assert.strictEqual(new TextDecoder('l1').decode(Uint8Array.of(0x80, 0xe9)), '€é');

console.log('ok');
