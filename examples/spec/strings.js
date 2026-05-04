import { test, testDeep, summary } from './helpers.js';

console.log('String Tests\n');

let str = 'hello world';
test('indexOf found', str.indexOf('world'), 6);
test('indexOf first occurrence', str.indexOf('o'), 4);
test('indexOf not found', str.indexOf('xyz'), -1);
test('indexOf empty string', str.indexOf(''), 0);

let js = 'JavaScript';
test('substring start end', js.substring(0, 4), 'Java');
test('substring start only', js.substring(4), 'Script');
test('substring swaps if end < start', js.substring(10, 4), 'Script');

let fox = 'The quick brown fox';
test('slice start end', fox.slice(0, 3), 'The');
test('slice start only', fox.slice(10), 'brown fox');
test('slice negative start', fox.slice(-3), 'fox');
test('slice negative end', fox.slice(0, -4), 'The quick brown');

let csv = 'apple,banana,cherry';
let parts = csv.split(',');
test('split length', parts.length, 3);
test('split first', parts[0], 'apple');
test('split last', parts[2], 'cherry');

let chars = 'hello'.split('');
test('split empty', chars.length, 5);
test('split empty first', chars[0], 'h');

test('includes true', 'quick brown fox'.includes('quick'), true);
test('includes false', 'quick brown fox'.includes('cat'), false);
test('includes empty', 'hello'.includes(''), true);

test('startsWith true', 'Hello, World!'.startsWith('Hello'), true);
test('startsWith false', 'Hello, World!'.startsWith('World'), false);

test('endsWith true', 'index.html'.endsWith('.html'), true);
test('endsWith false', 'index.html'.endsWith('.js'), false);

test('toUpperCase', 'hello'.toUpperCase(), 'HELLO');
test('toLowerCase', 'HELLO'.toLowerCase(), 'hello');

test('trim', '  hello  '.trim(), 'hello');
test('trimStart', '  hello'.trimStart(), 'hello');
test('trimEnd', 'hello  '.trimEnd(), 'hello');

test('padStart', '5'.padStart(3, '0'), '005');
test('padEnd', '5'.padEnd(3, '0'), '500');

test('repeat', 'ab'.repeat(3), 'ababab');

test('charAt', 'hello'.charAt(1), 'e');
test('charAt default index', 'hello'.charAt(), 'h');
test('charAt coercion', 'hello'.charAt('1'), 'e');
test('charCodeAt', 'ABC'.charCodeAt(0), 65);
test('charCodeAt default index', 'ABC'.charCodeAt(), 65);
test('charCodeAt coercion', 'ABC'.charCodeAt('1'), 66);
test('codePointAt ascii', 'ABC'.codePointAt(0), 65);
test('codePointAt default index', 'ABC'.codePointAt(), 65);
test('codePointAt coercion', 'ABC'.codePointAt('1'), 66);
test('codePointAt out of bounds', 'ABC'.codePointAt(10), undefined);
test('codePointAt utf8 2-byte', 'é'.codePointAt(0), 233);
test('codePointAt utf8 3-byte', '中'.codePointAt(0), 20013);
test('codePointAt utf8 4-byte', '😀'.codePointAt(0), 128512);
test('charAt astral leading surrogate', '💙'.charAt(0).charCodeAt(0), 0xD83D);
test('charAt astral trailing surrogate', '💙'.charAt(1).charCodeAt(0), 0xDC99);

test('replace', 'hello world'.replace('world', 'there'), 'hello there');
test('replaceAll', 'a-b-c'.replaceAll('-', '_'), 'a_b_c');

test(
  'template missing placeholders stay empty',
  'Hello, {{name}}. Missing: {{missing}}.'.template({ name: 'Ant' }),
  'Hello, Ant. Missing: .'
);
test(
  'template placeholder values use normal string coercion',
  '{{nil}}/{{undef}}/{{obj}}/{{arr}}/{{ok}}'.template({
    nil: null,
    undef: undefined,
    obj: { toString() { return 'custom'; } },
    arr: [1, 2],
    ok: false
  }),
  'null/undefined/custom/1,2/false'
);
test(
  'template unterminated placeholders remain literal',
  'before {{name after'.template({ name: 'ignored' }),
  'before {{name after'
);

testDeep('match', 'test123'.match(/\d+/), ['123']);

test('at positive', 'hello'.at(1), 'e');
test('at negative', 'hello'.at(-1), 'o');

test('length', 'hello'.length, 5);
test('bracket access', 'hello'[0], 'h');
test('bracket access astral leading surrogate', '💙'[0].charCodeAt(0), 0xD83D);
test('bracket access astral trailing surrogate', '💙'[1].charCodeAt(0), 0xDC99);

test('concat', 'hello'.concat(' ', 'world'), 'hello world');

test('lastIndexOf', 'hello world world'.lastIndexOf('world'), 12);

test('String.fromCharCode', String.fromCharCode(65, 66, 67), 'ABC');
test('String.raw tagged template', String.raw`line1\nline2`, 'line1\\nline2');
test('String.raw substitutions', String.raw({ raw: ['a', 'b', 'c'] }, 1, 2), 'a1b2c');

test('empty string length', ''.length, 0);
test('empty indexOf', ''.indexOf('x'), -1);
test('empty includes empty', ''.includes(''), true);
test('empty startsWith empty', ''.startsWith(''), true);

let nfc = '\u0041\u006d\u0065\u0301\u006c\u0069\u0065';
test('normalize default (NFC)', nfc.normalize(), 'Am\u00E9lie');
test('normalize NFC explicit', nfc.normalize('NFC'), 'Am\u00E9lie');

let composed = 'Am\u00E9lie';
test('normalize NFD', composed.normalize('NFD'), 'Ame\u0301lie');

test('normalize NFKC fi ligature', '\uFB01'.normalize('NFKC'), 'fi');
test('normalize NFKC fullwidth', '\uFF21'.normalize('NFKC'), 'A');

test('normalize NFKD fi ligature', '\uFB01'.normalize('NFKD'), 'fi');
test('normalize NFKD fullwidth', '\uFF21'.normalize('NFKD'), 'A');

test('normalize empty string', ''.normalize(), '');
test('normalize ascii passthrough', 'hello'.normalize(), 'hello');
test('normalize no arg same as NFC', '\u00E9'.normalize(), '\u00E9');

let template = `Value: ${1 + 2}`;
test('template literal', template, 'Value: 3');

let multi = `line1
line2`;
test('multiline template', multi.includes('\n'), true);

summary();
