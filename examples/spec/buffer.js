import { test, summary } from './helpers.js';

console.log('Buffer Tests\n');

const ab = new ArrayBuffer(16);
test('ArrayBuffer byteLength', ab.byteLength, 16);

const ab2 = ab.slice(4, 12);
test('ArrayBuffer slice byteLength', ab2.byteLength, 8);

const u8 = new Uint8Array(8);
test('Uint8Array length', u8.length, 8);
test('Uint8Array byteLength', u8.byteLength, 8);
test('Uint8Array BYTES_PER_ELEMENT', u8.BYTES_PER_ELEMENT, 1);

const i16 = new Int16Array(4);
test('Int16Array length', i16.length, 4);
test('Int16Array byteLength', i16.byteLength, 8);
test('Int16Array BYTES_PER_ELEMENT', i16.BYTES_PER_ELEMENT, 2);

const i32 = new Int32Array(2);
test('Int32Array length', i32.length, 2);
test('Int32Array byteLength', i32.byteLength, 8);
test('Int32Array BYTES_PER_ELEMENT', i32.BYTES_PER_ELEMENT, 4);

const f32 = new Float32Array(4);
test('Float32Array length', f32.length, 4);
test('Float32Array byteLength', f32.byteLength, 16);
test('Float32Array BYTES_PER_ELEMENT', f32.BYTES_PER_ELEMENT, 4);

const f64 = new Float64Array(2);
test('Float64Array length', f64.length, 2);
test('Float64Array byteLength', f64.byteLength, 16);
test('Float64Array BYTES_PER_ELEMENT', f64.BYTES_PER_ELEMENT, 8);

const u16 = new Uint16Array(4);
test('Uint16Array length', u16.length, 4);
test('Uint16Array BYTES_PER_ELEMENT', u16.BYTES_PER_ELEMENT, 2);

const u32 = new Uint32Array(2);
test('Uint32Array length', u32.length, 2);
test('Uint32Array BYTES_PER_ELEMENT', u32.BYTES_PER_ELEMENT, 4);

const i8 = new Int8Array(8);
test('Int8Array length', i8.length, 8);
test('Int8Array BYTES_PER_ELEMENT', i8.BYTES_PER_ELEMENT, 1);

const u8c = new Uint8ClampedArray(8);
test('Uint8ClampedArray length', u8c.length, 8);
test('Uint8ClampedArray BYTES_PER_ELEMENT', u8c.BYTES_PER_ELEMENT, 1);

const bi64 = new BigInt64Array(2);
test('BigInt64Array length', bi64.length, 2);
test('BigInt64Array BYTES_PER_ELEMENT', bi64.BYTES_PER_ELEMENT, 8);

const bu64 = new BigUint64Array(2);
test('BigUint64Array length', bu64.length, 2);
test('BigUint64Array BYTES_PER_ELEMENT', bu64.BYTES_PER_ELEMENT, 8);

const buffer = new ArrayBuffer(16);
const view8 = new Uint8Array(buffer);
const view16 = new Uint16Array(buffer);
const view32 = new Uint32Array(buffer);

test('Uint8Array view length', view8.length, 16);
test('Uint16Array view length', view16.length, 8);
test('Uint32Array view length', view32.length, 4);

const viewWithOffset = new Uint8Array(buffer, 4);
test('view with offset length', viewWithOffset.length, 12);
test('view with offset byteOffset', viewWithOffset.byteOffset, 4);

const viewWithOffsetAndLength = new Uint8Array(buffer, 4, 8);
test('view offset+length length', viewWithOffsetAndLength.length, 8);
test('view offset+length byteOffset', viewWithOffsetAndLength.byteOffset, 4);

const dv = new DataView(buffer);
test('DataView byteLength', dv.byteLength, 16);
test('DataView byteOffset', dv.byteOffset, 0);

const dv2 = new DataView(buffer, 4);
test('DataView offset byteLength', dv2.byteLength, 12);
test('DataView offset byteOffset', dv2.byteOffset, 4);

const dv3 = new DataView(buffer, 4, 8);
test('DataView offset+length byteLength', dv3.byteLength, 8);
test('DataView offset+length byteOffset', dv3.byteOffset, 4);

dv.setUint8(0, 42);
test('DataView set/get Uint8', dv.getUint8(0), 42);

dv.setUint8(0, 0x12);
dv.setUint8(1, 0x34);
test('DataView Int16 LE', dv.getInt16(0, true), 0x3412);
test('DataView Int16 BE', dv.getInt16(0, false), 0x1234);

const buf1 = Buffer.alloc(10);
test('Buffer.alloc length', buf1.length, 10);

const buf2 = Buffer.from('Hello');
test('Buffer.from string length', buf2.length, 5);
test('Buffer.from string toString', buf2.toString(), 'Hello');

const buf3 = Buffer.from([72, 101, 108, 108, 111]);
test('Buffer.from array length', buf3.length, 5);
test('Buffer.from array toString', buf3.toString(), 'Hello');

const buf4 = Buffer.alloc(10);
const written = buf4.write('Hello', 0);
test('Buffer write bytes', written, 5);

const buf5 = Buffer.from('Hello');
test('Buffer toString hex', buf5.toString('hex'), '48656c6c6f');

const original = new Uint8Array(10);
const sliced = original.slice(2, 8);
test('TypedArray slice length', sliced.length, 6);

const subarrayed = original.subarray(2, 8);
test('TypedArray subarray length', subarrayed.length, 6);
test('TypedArray subarray byteOffset', subarrayed.byteOffset, 2);

test('Buffer.isBuffer with Buffer', Buffer.isBuffer(Buffer.from('test')), true);
test('Buffer.isBuffer with string', Buffer.isBuffer('test'), false);
test('Buffer.isBuffer with number', Buffer.isBuffer(123), false);
test('Buffer.isBuffer with object', Buffer.isBuffer({}), false);
test('Buffer.isBuffer with Uint8Array', Buffer.isBuffer(new Uint8Array(4)), false);

test('Buffer.isEncoding utf8', Buffer.isEncoding('utf8'), true);
test('Buffer.isEncoding utf-8', Buffer.isEncoding('utf-8'), true);
test('Buffer.isEncoding hex', Buffer.isEncoding('hex'), true);
test('Buffer.isEncoding base64', Buffer.isEncoding('base64'), true);
test('Buffer.isEncoding ascii', Buffer.isEncoding('ascii'), true);
test('Buffer.isEncoding latin1', Buffer.isEncoding('latin1'), true);
test('Buffer.isEncoding binary', Buffer.isEncoding('binary'), true);
test('Buffer.isEncoding invalid', Buffer.isEncoding('invalid'), false);
test('Buffer.isEncoding empty', Buffer.isEncoding(''), false);

test('Buffer.byteLength string', Buffer.byteLength('Hello'), 5);
test('Buffer.byteLength buffer', Buffer.byteLength(Buffer.from('Hello')), 5);
test('Buffer.byteLength empty', Buffer.byteLength(''), 0);

const concatBuf1 = Buffer.from('Hello');
const concatBuf2 = Buffer.from(' ');
const concatBuf3 = Buffer.from('World');
const concatenated = Buffer.concat([concatBuf1, concatBuf2, concatBuf3]);
test('Buffer.concat length', concatenated.length, 11);
test('Buffer.concat toString', concatenated.toString(), 'Hello World');

const emptyConcat = Buffer.concat([]);
test('Buffer.concat empty array', emptyConcat.length, 0);

const singleConcat = Buffer.concat([Buffer.from('Only')]);
test('Buffer.concat single buffer', singleConcat.toString(), 'Only');

const limitedConcat = Buffer.concat([concatBuf1, concatBuf2, concatBuf3], 5);
test('Buffer.concat with totalLength', limitedConcat.length, 5);
test('Buffer.concat with totalLength content', limitedConcat.toString(), 'Hello');

const cmpA = Buffer.from('abc');
const cmpB = Buffer.from('abc');
const cmpC = Buffer.from('abd');
const cmpD = Buffer.from('ab');
test('Buffer.compare equal', Buffer.compare(cmpA, cmpB), 0);
test('Buffer.compare less', Buffer.compare(cmpA, cmpC), -1);
test('Buffer.compare greater', Buffer.compare(cmpC, cmpA), 1);
test('Buffer.compare shorter', Buffer.compare(cmpD, cmpA), -1);
test('Buffer.compare longer', Buffer.compare(cmpA, cmpD), 1);

console.log('\nBuffer Encoding Tests\n');

const b64Buf = Buffer.from('Hello, World!');
test('Buffer.toString base64', b64Buf.toString('base64'), 'SGVsbG8sIFdvcmxkIQ==');
test('Buffer.toString BASE64 (case insensitive)', b64Buf.toString('BASE64'), 'SGVsbG8sIFdvcmxkIQ==');

const b64Decoded = Buffer.from('SGVsbG8sIFdvcmxkIQ==', 'base64');
test('Buffer.from base64', b64Decoded.toString(), 'Hello, World!');
test('Buffer.from BASE64 (case insensitive)', Buffer.from('SGVsbG8=', 'BASE64').toString(), 'Hello');

const b64Roundtrip = Buffer.from(Buffer.from('hello').toString('base64'), 'base64').toString();
test('Buffer base64 roundtrip', b64Roundtrip, 'hello');

const hexBuf = Buffer.from('Hello');
test('Buffer.toString hex', hexBuf.toString('hex'), '48656c6c6f');
test('Buffer.toString HEX (case insensitive)', hexBuf.toString('HEX'), '48656c6c6f');

const hexDecoded = Buffer.from('48656c6c6f', 'hex');
test('Buffer.from hex', hexDecoded.toString(), 'Hello');
test('Buffer.from HEX (case insensitive)', Buffer.from('48656C6C6F', 'HEX').toString(), 'Hello');

const hexRoundtrip = Buffer.from(Buffer.from('world').toString('hex'), 'hex').toString();
test('Buffer hex roundtrip', hexRoundtrip, 'world');

const utf8Buf = Buffer.from('Hello', 'utf8');
test('Buffer.from utf8', utf8Buf.toString(), 'Hello');
test('Buffer.from utf-8', Buffer.from('Hello', 'utf-8').toString(), 'Hello');
test('Buffer.from UTF8 (case insensitive)', Buffer.from('Hello', 'UTF8').toString(), 'Hello');
test('Buffer.from UTF-8 (case insensitive)', Buffer.from('Hello', 'UTF-8').toString(), 'Hello');

test('Buffer.toString utf8', utf8Buf.toString('utf8'), 'Hello');
test('Buffer.toString utf-8', utf8Buf.toString('utf-8'), 'Hello');

const asciiBuf = Buffer.from('Hello', 'ascii');
test('Buffer.from ascii', asciiBuf.toString(), 'Hello');
test('Buffer.from ASCII (case insensitive)', Buffer.from('Hello', 'ASCII').toString(), 'Hello');
test('Buffer.toString ascii', asciiBuf.toString('ascii'), 'Hello');

const latin1Buf = Buffer.from('Hello', 'latin1');
test('Buffer.from latin1', latin1Buf.toString(), 'Hello');
test('Buffer.from LATIN1 (case insensitive)', Buffer.from('Hello', 'LATIN1').toString(), 'Hello');
test('Buffer.from binary', Buffer.from('Hello', 'binary').toString(), 'Hello');
test('Buffer.from BINARY (case insensitive)', Buffer.from('Hello', 'BINARY').toString(), 'Hello');

test('Buffer.toString latin1', latin1Buf.toString('latin1'), 'Hello');
test('Buffer.toString binary', latin1Buf.toString('binary'), 'Hello');

const ucs2Buf = Buffer.from('Hi', 'ucs2');
test('Buffer.from ucs2 length', ucs2Buf.length, 4); // 2 chars * 2 bytes
test('Buffer.from ucs-2 length', Buffer.from('Hi', 'ucs-2').length, 4);
test('Buffer.from utf16le length', Buffer.from('Hi', 'utf16le').length, 4);
test('Buffer.from utf-16le length', Buffer.from('Hi', 'utf-16le').length, 4);
test('Buffer.from UCS2 (case insensitive)', Buffer.from('Hi', 'UCS2').length, 4);

test('Buffer.from ucs2 byte 0', ucs2Buf[0], 0x48);
test('Buffer.from ucs2 byte 1', ucs2Buf[1], 0x00);
test('Buffer.from ucs2 byte 2', ucs2Buf[2], 0x69);
test('Buffer.from ucs2 byte 3', ucs2Buf[3], 0x00);

test('Buffer.toString ucs2', ucs2Buf.toString('ucs2'), 'Hi');
test('Buffer.toString ucs-2', ucs2Buf.toString('ucs-2'), 'Hi');
test('Buffer.toString utf16le', ucs2Buf.toString('utf16le'), 'Hi');
test('Buffer.toString utf-16le', ucs2Buf.toString('utf-16le'), 'Hi');

const binaryBuf = Buffer.from([0x00, 0x01, 0x02, 0xff]);
test('Buffer binary toString base64', binaryBuf.toString('base64'), 'AAEC/w==');
test('Buffer binary toString hex', binaryBuf.toString('hex'), '000102ff');

const binaryFromB64 = Buffer.from('AAEC/w==', 'base64');
test('Buffer binary from base64 byte 0', binaryFromB64[0], 0x00);
test('Buffer binary from base64 byte 1', binaryFromB64[1], 0x01);
test('Buffer binary from base64 byte 2', binaryFromB64[2], 0x02);
test('Buffer binary from base64 byte 3', binaryFromB64[3], 0xff);

const binaryFromHex = Buffer.from('000102ff', 'hex');
test('Buffer binary from hex byte 0', binaryFromHex[0], 0x00);
test('Buffer binary from hex byte 1', binaryFromHex[1], 0x01);
test('Buffer binary from hex byte 2', binaryFromHex[2], 0x02);
test('Buffer binary from hex byte 3', binaryFromHex[3], 0xff);

summary();
