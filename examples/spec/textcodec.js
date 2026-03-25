import { test, testDeep, testThrows, summary } from './helpers.js';

console.log('TextEncoder/TextDecoder Tests\n');

const encoder = new TextEncoder();
const decoder = new TextDecoder();

const encoded = encoder.encode('hello');
test('TextEncoder type', encoded instanceof Uint8Array, true);
test('TextEncoder length', encoded.length, 5);
test('TextEncoder byte 0', encoded[0], 104);
test('TextEncoder byte 1', encoded[1], 101);

const decoded = decoder.decode(encoded);
test('TextDecoder', decoded, 'hello');

const utf8 = encoder.encode('日本語');
test('UTF-8 encode length', utf8.length, 9);
test('UTF-8 decode', decoder.decode(utf8), '日本語');

const emoji = encoder.encode('😀');
test('emoji encode length', emoji.length, 4);
test('emoji decode', decoder.decode(emoji), '😀');

const empty = encoder.encode('');
test('empty encode length', empty.length, 0);
test('empty decode', decoder.decode(empty), '');

const roundtrip = 'Hello, 世界! 🎉';
test('roundtrip', decoder.decode(encoder.encode(roundtrip)), roundtrip);

test('TextEncoder.encoding', encoder.encoding, 'utf-8');
test('TextEncoder requires new', typeof TextEncoder, 'function');
testThrows('TextEncoder without new throws', () => TextEncoder());

testDeep('encode lone high surrogate', [...encoder.encode('\uD800')], [0xef, 0xbf, 0xbd]);
testDeep('encode lone low surrogate', [...encoder.encode('\uDC00')], [0xef, 0xbf, 0xbd]);
testDeep('encode surrogate in string', [...encoder.encode('a\uD800b')], [0x61, 0xef, 0xbf, 0xbd, 0x62]);
testDeep('encode reversed surrogates', [...encoder.encode('\uDC00\uD800')], [0xef, 0xbf, 0xbd, 0xef, 0xbf, 0xbd]);
test('encode valid surrogate pair', encoder.encode('\uD834\uDD1E').length, 4); // U+1D11E 𝄞

const dest = new Uint8Array(10);
const result = encoder.encodeInto('hello', dest);
test('encodeInto read', result.read, 5);
test('encodeInto written', result.written, 5);
test('encodeInto data', decoder.decode(dest.subarray(0, result.written)), 'hello');

const small = new Uint8Array(2);
const partial = encoder.encodeInto('hello', small);
test('encodeInto partial written', partial.written, 2);
test('encodeInto partial read', partial.read, 2);

test('TextDecoder default encoding', new TextDecoder().encoding, 'utf-8');
test('TextDecoder utf8 alias', new TextDecoder('utf8').encoding, 'utf-8');
test('TextDecoder case insensitive', new TextDecoder('UTF-8').encoding, 'utf-8');
test('TextDecoder utf-16le label', new TextDecoder('utf-16le').encoding, 'utf-16le');
test('TextDecoder utf-16be label', new TextDecoder('utf-16be').encoding, 'utf-16be');
test('TextDecoder utf-16 alias', new TextDecoder('utf-16').encoding, 'utf-16le');
testThrows('TextDecoder invalid label', () => new TextDecoder('bogus'));
testThrows('TextDecoder without new throws', () => TextDecoder());

test('fatal defaults false', new TextDecoder().fatal, false);
test('fatal option true', new TextDecoder('utf-8', { fatal: true }).fatal, true);
test('ignoreBOM defaults false', new TextDecoder().ignoreBOM, false);
test('ignoreBOM option true', new TextDecoder('utf-8', { ignoreBOM: true }).ignoreBOM, true);

testThrows('fatal on invalid UTF-8', () => {
  new TextDecoder('utf-8', { fatal: true }).decode(new Uint8Array([0xff]));
});
testThrows('fatal on truncated sequence', () => {
  new TextDecoder('utf-8', { fatal: true }).decode(new Uint8Array([0xc0]));
});
testThrows('fatal on overlong', () => {
  new TextDecoder('utf-8', { fatal: true }).decode(new Uint8Array([0xc0, 0x80]));
});
test('non-fatal replacement', new TextDecoder().decode(new Uint8Array([0xff])), '\uFFFD');
test('non-fatal truncated', new TextDecoder().decode(new Uint8Array([0xc0])), '\uFFFD');

test('UTF-8 BOM stripped by default', new TextDecoder().decode(new Uint8Array([0xef, 0xbb, 0xbf, 0x41])), 'A');
test('UTF-8 BOM kept with ignoreBOM', new TextDecoder('utf-8', { ignoreBOM: true }).decode(new Uint8Array([0xef, 0xbb, 0xbf, 0x41])), '\uFEFFA');

{
  const sd = new TextDecoder();
  let out = '';
  out += sd.decode(new Uint8Array([0xf0, 0x9f, 0x92]), { stream: true });
  out += sd.decode(new Uint8Array([0xa9]));
  test('streaming UTF-8 multi-byte', out, '\u{1F4A9}');
}

{
  const sd = new TextDecoder();
  let out = '';
  out += sd.decode(new Uint8Array([0xf0]), { stream: true });
  out += sd.decode(new Uint8Array([0x9f]), { stream: true });
  out += sd.decode(new Uint8Array([0x92]), { stream: true });
  out += sd.decode(new Uint8Array([0xa9]));
  test('streaming UTF-8 byte-at-a-time', out, '\u{1F4A9}');
}

{
  const sd = new TextDecoder();
  let out = '';
  out += sd.decode(new Uint8Array([0xf0, 0x9f]), { stream: true });
  out += sd.decode();
  test('streaming flush incomplete sequence', out, '\uFFFD');
}

test('UTF-16LE basic', new TextDecoder('utf-16le').decode(new Uint8Array([0x41, 0x00, 0x42, 0x00])), 'AB');
test('UTF-16LE surrogate pair', new TextDecoder('utf-16le').decode(new Uint8Array([0x34, 0xd8, 0x1e, 0xdd])), '\uD834\uDD1E');
test('UTF-16LE BOM stripped', new TextDecoder('utf-16le').decode(new Uint8Array([0xff, 0xfe, 0x41, 0x00])), 'A');

test(
  'UTF-16LE BOM kept with ignoreBOM',
  new TextDecoder('utf-16le', { ignoreBOM: true }).decode(new Uint8Array([0xff, 0xfe, 0x41, 0x00])),
  '\uFEFFA'
);

testThrows('UTF-16LE fatal on odd byte', () => {
  new TextDecoder('utf-16le', { fatal: true }).decode(new Uint8Array([0x00]));
});

test('UTF-16LE non-fatal odd byte', new TextDecoder('utf-16le').decode(new Uint8Array([0x00])), '\uFFFD');

{
  const sd = new TextDecoder('utf-16le');
  let out = '';
  out += sd.decode(new Uint8Array([0x41]), { stream: true });
  out += sd.decode(new Uint8Array([0x00]));
  test('UTF-16LE streaming split code unit', out, 'A');
}

{
  const sd = new TextDecoder('utf-16le');
  let out = '';
  out += sd.decode(new Uint8Array([0x34, 0xd8]), { stream: true });
  out += sd.decode(new Uint8Array([0x1e, 0xdd]));
  test('UTF-16LE streaming split surrogate pair', out, '\uD834\uDD1E');
}

test('UTF-16BE basic', new TextDecoder('utf-16be').decode(new Uint8Array([0x00, 0x41, 0x00, 0x42])), 'AB');
test('UTF-16BE surrogate pair', new TextDecoder('utf-16be').decode(new Uint8Array([0xd8, 0x34, 0xdd, 0x1e])), '\uD834\uDD1E');
test('UTF-16BE BOM stripped', new TextDecoder('utf-16be').decode(new Uint8Array([0xfe, 0xff, 0x00, 0x41])), 'A');

{
  const buf = new Uint8Array([0x68, 0x69]).buffer;
  test('decode ArrayBuffer', new TextDecoder().decode(buf), 'hi');
}

{
  const d = new TextDecoder();
  d.decode(new Uint8Array([0xf0, 0x9f]), { stream: true });
  const fresh = d.decode(new Uint8Array([0x41]));
  test('decoder reuse resets', fresh, '\uFFFDA');
}

testDeep('encode undefined', [...encoder.encode(undefined)], []);
testDeep('encode no args', [...encoder.encode()], []);
test('decode no args', decoder.decode(), '');

summary();
