import { test, testThrows, testDeep, summary } from './helpers.js';

console.log('FormData constructor\n');

const fd0 = new FormData();
test('empty constructor', fd0.has('x'), false);

testThrows('requires new', () => FormData());
testThrows('rejects form element', () => new FormData(document));

console.log('\nappend / get / has / delete / set\n');

const fd = new FormData();

fd.append('name', 'Alice');
test('append then get', fd.get('name'), 'Alice');
test('has returns true', fd.has('name'), true);
test('has missing returns false', fd.has('other'), false);

fd.append('name', 'Bob');
const all = fd.getAll('name');
test('getAll length after two appends', all.length, 2);
test('getAll first', all[0], 'Alice');
test('getAll second', all[1], 'Bob');

fd.set('name', 'Carol');
test('set replaces all', fd.getAll('name').length, 1);
test('set value', fd.get('name'), 'Carol');

fd.delete('name');
test('delete removes all', fd.has('name'), false);
test('get after delete returns null', fd.get('name'), null);

console.log('\nBlob values\n');

const blob = new Blob(['hello'], { type: 'text/plain' });
const fdb = new FormData();
fdb.append('file', blob);

const got = fdb.get('file');
test('blob entry is File instance', got instanceof File, true);
test('blob entry filename default', got.name, 'blob');
test('blob entry type', got.type, 'text/plain');

fdb.append('file2', blob, 'custom.txt');
const got2 = fdb.get('file2');
test('blob with filename', got2.name, 'custom.txt');

console.log('\nFile values\n');

const file = new File(['world'], 'test.txt', { type: 'text/html' });
const fdf = new FormData();
fdf.append('upload', file);

const gotf = fdf.get('upload');
test('file entry is File instance', gotf instanceof File, true);
test('file entry name from File', gotf.name, 'test.txt');
test('file entry type', gotf.type, 'text/html');

fdf.append('upload2', file, 'override.txt');
test('file entry name overridden', fdf.get('upload2').name, 'override.txt');

console.log('\ngetAll with mixed types\n');

const fdm = new FormData();
fdm.append('x', 'str');
fdm.append('x', new Blob(['data']));
const mixed = fdm.getAll('x');
test('mixed getAll length', mixed.length, 2);
test('mixed first is string', mixed[0], 'str');
test('mixed second is File', mixed[1] instanceof File, true);

console.log('\nforEach\n');

const fdfe = new FormData();
fdfe.append('b', '2');
fdfe.append('a', '1');
const seen = [];
fdfe.forEach((val, name) => seen.push(`${name}=${val}`));
test('forEach visits all entries', seen.length, 2);
test('forEach insertion order first', seen[0], 'b=2');
test('forEach insertion order second', seen[1], 'a=1');

console.log('\nentries / keys / values\n');

const fdi = new FormData();
fdi.append('x', '1');
fdi.append('y', '2');
fdi.append('x', '3');

const keys = [...fdi.keys()];
test('keys length', keys.length, 3);
test('keys first', keys[0], 'x');
test('keys second', keys[1], 'y');
test('keys third', keys[2], 'x');

const vals = [...fdi.values()];
test('values first', vals[0], '1');
test('values second', vals[1], '2');
test('values third', vals[2], '3');

const entries = [...fdi.entries()];
test('entries length', entries.length, 3);
test('entries first name', entries[0][0], 'x');
test('entries first value', entries[0][1], '1');

console.log('\nSymbol.iterator\n');

const fds = new FormData();
fds.append('k', 'v');
const iter_result = [...fds];
test('Symbol.iterator yields entries', iter_result.length, 1);
test('Symbol.iterator entry name', iter_result[0][0], 'k');
test('Symbol.iterator entry value', iter_result[0][1], 'v');

console.log('\ntoStringTag\n');

test('toStringTag', Object.prototype.toString.call(new FormData()), '[object FormData]');

console.log('\nmultipart parameter parsing\n');

const boundary = 'spec-boundary';
const multipartBody =
  `--${boundary}\r\n` +
  `Content-Disposition: form-data; name="field"\r\n\r\n` +
  `value\r\n` +
  `--${boundary}--\r\n`;

const multipartReq = new Request('https://example.com/', {
  method: 'POST',
  headers: {
    'Content-Type': `multipart/form-data; boundaryx=wrong; boundary="${boundary}"`,
  },
  body: multipartBody,
});

const multipartFd = await multipartReq.formData();
test('multipart boundary lookup skips longer prefix match', multipartFd.get('field'), 'value');

summary();
