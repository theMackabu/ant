import { test, testThrows, summary } from './helpers.js';

console.log('Blob constructor\n');

const empty = new Blob();
test('empty Blob size', empty.size, 0);
test('empty Blob type', empty.type, '');

const b1 = new Blob(['hello']);
test('string part size', b1.size, 5);

const b2 = new Blob(['hello', ' world']);
test('two string parts size', b2.size, 11);

const b3 = new Blob(['hello'], { type: 'text/plain' });
test('type set from options', b3.type, 'text/plain');

const b4 = new Blob(['hello'], { type: 'TEXT/PLAIN' });
test('type lowercased', b4.type, 'text/plain');

const b5 = new Blob(['x'], { type: 'te\x01xt' });
test('invalid type byte → empty string', b5.type, '');

testThrows('requires new', () => Blob(['x']));

console.log('\nBlob from typed parts\n');

const ab = new Uint8Array([1, 2, 3]).buffer;
const b6 = new Blob([ab]);
test('ArrayBuffer part size', b6.size, 3);

const ta = new Uint8Array([4, 5, 6]);
const b7 = new Blob([ta]);
test('Uint8Array part size', b7.size, 3);

const b8 = new Blob([b1, ' there']);
test('Blob part size', b8.size, 11);

console.log('\nBlob.text()\n');

const bt = await new Blob(['hello world']).text();
test('text() content', bt, 'hello world');

const be = await new Blob().text();
test('empty blob text()', be, '');

console.log('\nBlob.arrayBuffer()\n');

const bab = await new Blob([new Uint8Array([10, 20, 30])]).arrayBuffer();
const view = new Uint8Array(bab);
test('arrayBuffer() byte 0', view[0], 10);
test('arrayBuffer() byte 1', view[1], 20);
test('arrayBuffer() byte 2', view[2], 30);

console.log('\nBlob.bytes()\n');

const bytes = await new Blob([new Uint8Array([7, 8, 9])]).bytes();
test('bytes() is Uint8Array', bytes instanceof Uint8Array, true);
test('bytes() byte 0', bytes[0], 7);
test('bytes() byte 2', bytes[2], 9);

console.log('\nBlob.slice()\n');

const src = new Blob(['hello world']);
const s1 = src.slice(0, 5);
test('slice(0,5) size', s1.size, 5);
test('slice content', await s1.text(), 'hello');

const s2 = src.slice(6);
test('slice(6) size', s2.size, 5);
test('slice(6) content', await s2.text(), 'world');

const s3 = src.slice(-5);
test('slice(-5) size', s3.size, 5);
test('slice(-5) content', await s3.text(), 'world');

const s4 = src.slice(0, 5, 'text/plain');
test('slice with type', s4.type, 'text/plain');

const s5 = src.slice(5, 3);
test('slice end < start → empty', s5.size, 0);

console.log('\nFile constructor\n');

const f1 = new File(['content'], 'test.txt');
test('File name', f1.name, 'test.txt');
test('File size', f1.size, 7);
test('File type default', f1.type, '');
test('File lastModified is number', typeof f1.lastModified, 'number');
test('File lastModified > 0', f1.lastModified > 0, true);

const f2 = new File(['data'], 'file.txt', { type: 'text/plain', lastModified: 1000 });
test('File type from options', f2.type, 'text/plain');
test('File lastModified from options', f2.lastModified, 1000);

testThrows('File requires new', () => File(['x'], 'f.txt'));
testThrows('File requires 2 args', () => new File(['x']));

console.log('\nFile instanceof Blob\n');

test('File instanceof Blob', f1 instanceof Blob, true);
test('File instanceof File', f1 instanceof File, true);
test('Blob not instanceof File', b1 instanceof File, false);

const ft = await f1.text();
test('File.text() works', ft, 'content');

console.log('\nSymbol.toStringTag\n');

test('Blob toStringTag', Object.prototype.toString.call(b1), '[object Blob]');
test('File toStringTag', Object.prototype.toString.call(f1), '[object File]');

summary();
