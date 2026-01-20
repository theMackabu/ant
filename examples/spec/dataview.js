import { test, summary } from './helpers.js';

console.log('DataView Tests\n');

const buffer = new ArrayBuffer(16);
const dv = new DataView(buffer);

test('DataView byteLength', dv.byteLength, 16);
test('DataView byteOffset', dv.byteOffset, 0);

const dv2 = new DataView(buffer, 4);
test('DataView with offset byteLength', dv2.byteLength, 12);
test('DataView with offset byteOffset', dv2.byteOffset, 4);

const dv3 = new DataView(buffer, 4, 8);
test('DataView with offset+length byteLength', dv3.byteLength, 8);
test('DataView with offset+length byteOffset', dv3.byteOffset, 4);

dv.setUint8(0, 255);
test('setUint8/getUint8', dv.getUint8(0), 255);

dv.setUint8(1, 0);
test('setUint8/getUint8 zero', dv.getUint8(1), 0);

dv.setInt16(0, 0x1234);
test('setInt16 BE / getInt16 BE', dv.getInt16(0), 0x1234);
test('setInt16 BE byte 0', dv.getUint8(0), 0x12);
test('setInt16 BE byte 1', dv.getUint8(1), 0x34);

dv.setInt16(2, 0x5678, true);
test('setInt16 LE / getInt16 LE', dv.getInt16(2, true), 0x5678);
test('setInt16 LE byte 0', dv.getUint8(2), 0x78);
test('setInt16 LE byte 1', dv.getUint8(3), 0x56);

test('setInt16 LE / getInt16 BE', dv.getInt16(2, false), 0x7856);

dv.setInt32(4, 0x12345678);
test('setInt32 BE / getInt32 BE', dv.getInt32(4), 0x12345678);
test('setInt32 BE byte 0', dv.getUint8(4), 0x12);
test('setInt32 BE byte 1', dv.getUint8(5), 0x34);
test('setInt32 BE byte 2', dv.getUint8(6), 0x56);
test('setInt32 BE byte 3', dv.getUint8(7), 0x78);

dv.setInt32(8, 0xaabbccdd, true);
test('setInt32 LE / getInt32 LE', dv.getInt32(8, true), 0xaabbccdd | 0);
test('setInt32 LE byte 0', dv.getUint8(8), 0xdd);
test('setInt32 LE byte 1', dv.getUint8(9), 0xcc);
test('setInt32 LE byte 2', dv.getUint8(10), 0xbb);
test('setInt32 LE byte 3', dv.getUint8(11), 0xaa);

const testFloat = 3.14;
dv.setFloat32(0, testFloat);
const readFloat = dv.getFloat32(0);
test('setFloat32/getFloat32 BE approx', Math.abs(readFloat - testFloat) < 0.001, true);

dv.setFloat32(4, testFloat, true);
const readFloatLE = dv.getFloat32(4, true);
test('setFloat32/getFloat32 LE approx', Math.abs(readFloatLE - testFloat) < 0.001, true);

const testDouble = 3.141592653589793;
dv.setFloat64(0, testDouble);
test('setFloat64/getFloat64 BE', dv.getFloat64(0), testDouble);

dv.setFloat64(8, testDouble, true);
test('setFloat64/getFloat64 LE', dv.getFloat64(8, true), testDouble);

dv.setInt16(0, -1234);
test('setInt16/getInt16 negative BE', dv.getInt16(0), -1234);

dv.setInt16(2, -1234, true);
test('setInt16/getInt16 negative LE', dv.getInt16(2, true), -1234);

dv.setInt32(4, -123456789);
test('setInt32/getInt32 negative BE', dv.getInt32(4), -123456789);

dv.setInt32(8, -123456789, true);
test('setInt32/getInt32 negative LE', dv.getInt32(8, true), -123456789);

const sharedBuffer = new ArrayBuffer(8);
const sharedDV = new DataView(sharedBuffer);
const sharedU8 = new Uint8Array(sharedBuffer);

sharedDV.setInt32(0, 0x01020304);
test('DataView/TypedArray share buffer byte 0', sharedU8[0], 0x01);
test('DataView/TypedArray share buffer byte 1', sharedU8[1], 0x02);
test('DataView/TypedArray share buffer byte 2', sharedU8[2], 0x03);
test('DataView/TypedArray share buffer byte 3', sharedU8[3], 0x04);

sharedU8[4] = 0xaa;
sharedU8[5] = 0xbb;
sharedU8[6] = 0xcc;
sharedU8[7] = 0xdd;
test('TypedArray write / DataView read', sharedDV.getInt32(4), 0xaabbccdd | 0);

const exampleBuffer = new ArrayBuffer(16);
const exampleView = new DataView(exampleBuffer);
exampleView.setInt32(0, 123456);
exampleView.setInt16(4, 32000);
test('Node example getInt32', exampleView.getInt32(0), 123456);
test('Node example getInt16', exampleView.getInt16(4), 32000);

summary();
