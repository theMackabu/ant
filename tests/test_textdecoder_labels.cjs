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

const legacy = [
  ['l9', [0xa4, 0x41], '\u20acA'],
  ['koi8-r', [0xc1, 0xc2], '\u0430\u0431'],
  ['windows-1251', [0xc0, 0xff], '\u0410\u044f'],
  ['ibm866', [0x80, 0xaf], '\u0410\u043f'],
  ['shift_jis', [0x82, 0xa0, 0x93, 0xfa, 0x96, 0x7b], '\u3042\u65e5\u672c'],
  ['euc-jp', [0xa4, 0xa2, 0xc6, 0xfc, 0xcb, 0xdc], '\u3042\u65e5\u672c'],
  ['iso-2022-jp', [0x1b, 0x24, 0x42, 0x24, 0x22, 0x46, 0x7c, 0x1b, 0x28, 0x42], '\u3042\u65e5'],
  ['gbk', [0xc4, 0xe3, 0xba, 0xc3], '\u4f60\u597d'],
  ['big5', [0xa7, 0x41, 0xa6, 0x6e], '\u4f60\u597d'],
  ['euc-kr', [0xbe, 0xc8, 0xb3, 0xe7], '\uc548\ub155'],
  ['utf-16be', [0xfe, 0xff, 0x00, 0x41], 'A'],
  ['utf-16le', [0xff, 0xfe, 0x41, 0x00, 0x3d, 0xd8], 'A\ufffd'],
];
for (const [label, bytes, text] of legacy) assert.strictEqual(new TextDecoder(label).decode(Uint8Array.from(bytes)), text, label);

const sjis = new TextDecoder('shift_jis');
assert.strictEqual(sjis.decode(Uint8Array.of(0x82), { stream: true }) + sjis.decode(Uint8Array.of(0xa0)), '\u3042');
assert.strictEqual(new TextDecoder('shift_jis').decode(Uint8Array.of(0x82)), '\ufffd');
assert.throws(() => new TextDecoder('shift_jis', { fatal: true }).decode(Uint8Array.of(0x82)), TypeError);
assert.strictEqual(new TextDecoder('utf-16le', { ignoreBOM: true }).decode(Uint8Array.of(0xff, 0xfe, 0x41, 0x00)), '\ufeffA');

const { StringDecoder } = require('node:string_decoder');
const sd = new StringDecoder('utf16le');
assert.strictEqual(sd.write(Buffer.from([0x3d, 0xd8])), '');
assert.strictEqual(sd.write(Buffer.from([0x00, 0xde])), '\u{1f600}');
assert.strictEqual(sd.end(Buffer.from([0x41, 0x00])), 'A');
const odd = new StringDecoder('utf16le');
assert.strictEqual(odd.write(Buffer.from([0x3d, 0xd8, 0x00])), '\ud83d');
assert.strictEqual(odd.write(Buffer.from([0xde, 0x41])), '\ude00');
assert.strictEqual(odd.end(), '');
assert.strictEqual(new StringDecoder('utf16le').end(Buffer.from([0x3d, 0xd8, 0x41, 0x00])), '\ud83dA');
const held = new StringDecoder('utf16le');
assert.strictEqual(held.write(Buffer.from([0x3d, 0xd8])), '');
assert.strictEqual(held.end(), '\ud83d');

console.log('ok');
