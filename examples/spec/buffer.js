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

summary();
