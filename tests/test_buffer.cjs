// Test Buffer and TypedArray implementations

console.log('=== ArrayBuffer Tests ===');

// Test ArrayBuffer creation
const ab = new ArrayBuffer(16);
console.log('ArrayBuffer byteLength:', ab.byteLength);

// Test ArrayBuffer slice
const ab2 = ab.slice(4, 12);
console.log('Sliced ArrayBuffer byteLength:', ab2.byteLength);

console.log('\n=== TypedArray Tests ===');

// Test Uint8Array
const u8 = new Uint8Array(8);
console.log('Uint8Array length:', u8.length);
console.log('Uint8Array byteLength:', u8.byteLength);
console.log('Uint8Array BYTES_PER_ELEMENT:', u8.BYTES_PER_ELEMENT);

// Test Int16Array
const i16 = new Int16Array(4);
console.log('Int16Array length:', i16.length);
console.log('Int16Array byteLength:', i16.byteLength);
console.log('Int16Array BYTES_PER_ELEMENT:', i16.BYTES_PER_ELEMENT);

// Test Int32Array
const i32 = new Int32Array(2);
console.log('Int32Array length:', i32.length);
console.log('Int32Array byteLength:', i32.byteLength);
console.log('Int32Array BYTES_PER_ELEMENT:', i32.BYTES_PER_ELEMENT);

// Test Float32Array
const f32 = new Float32Array(4);
console.log('Float32Array length:', f32.length);
console.log('Float32Array byteLength:', f32.byteLength);
console.log('Float32Array BYTES_PER_ELEMENT:', f32.BYTES_PER_ELEMENT);

// Test Float64Array
const f64 = new Float64Array(2);
console.log('Float64Array length:', f64.length);
console.log('Float64Array byteLength:', f64.byteLength);
console.log('Float64Array BYTES_PER_ELEMENT:', f64.BYTES_PER_ELEMENT);

// Test Uint16Array
const u16 = new Uint16Array(4);
console.log('Uint16Array length:', u16.length);
console.log('Uint16Array BYTES_PER_ELEMENT:', u16.BYTES_PER_ELEMENT);

// Test Uint32Array
const u32 = new Uint32Array(2);
console.log('Uint32Array length:', u32.length);
console.log('Uint32Array BYTES_PER_ELEMENT:', u32.BYTES_PER_ELEMENT);

// Test Int8Array
const i8 = new Int8Array(8);
console.log('Int8Array length:', i8.length);
console.log('Int8Array BYTES_PER_ELEMENT:', i8.BYTES_PER_ELEMENT);

// Test Uint8ClampedArray
const u8c = new Uint8ClampedArray(8);
console.log('Uint8ClampedArray length:', u8c.length);
console.log('Uint8ClampedArray BYTES_PER_ELEMENT:', u8c.BYTES_PER_ELEMENT);

// Test BigInt64Array
const bi64 = new BigInt64Array(2);
console.log('BigInt64Array length:', bi64.length);
console.log('BigInt64Array BYTES_PER_ELEMENT:', bi64.BYTES_PER_ELEMENT);

// Test BigUint64Array
const bu64 = new BigUint64Array(2);
console.log('BigUint64Array length:', bu64.length);
console.log('BigUint64Array BYTES_PER_ELEMENT:', bu64.BYTES_PER_ELEMENT);

console.log('\n=== TypedArray from ArrayBuffer ===');

// Create TypedArray views on same ArrayBuffer
const buffer = new ArrayBuffer(16);
const view8 = new Uint8Array(buffer);
const view16 = new Uint16Array(buffer);
const view32 = new Uint32Array(buffer);

console.log('Buffer size:', buffer.byteLength);
console.log('Uint8Array view length:', view8.length);
console.log('Uint16Array view length:', view16.length);
console.log('Uint32Array view length:', view32.length);

// Test with offset
const viewWithOffset = new Uint8Array(buffer, 4);
console.log('View with offset length:', viewWithOffset.length);
console.log('View with offset byteOffset:', viewWithOffset.byteOffset);

// Test with offset and length
const viewWithOffsetAndLength = new Uint8Array(buffer, 4, 8);
console.log('View with offset and length:', viewWithOffsetAndLength.length);
console.log('View with offset and length byteOffset:', viewWithOffsetAndLength.byteOffset);

console.log('\n=== DataView Tests ===');

// Test DataView creation
const dv = new DataView(buffer);
console.log('DataView byteLength:', dv.byteLength);
console.log('DataView byteOffset:', dv.byteOffset);

// Test DataView with offset
const dv2 = new DataView(buffer, 4);
console.log('DataView with offset byteLength:', dv2.byteLength);
console.log('DataView with offset byteOffset:', dv2.byteOffset);

// Test DataView with offset and length
const dv3 = new DataView(buffer, 4, 8);
console.log('DataView with offset and length byteLength:', dv3.byteLength);
console.log('DataView with offset and length byteOffset:', dv3.byteOffset);

// Test DataView get/set operations
dv.setUint8(0, 42);
const val = dv.getUint8(0);
console.log('Set/Get Uint8:', val);

// Test Int16 operations
dv.setUint8(0, 0x12);
dv.setUint8(1, 0x34);
const int16LE = dv.getInt16(0, true);  // little endian
const int16BE = dv.getInt16(0, false); // big endian
console.log('Int16 little endian:', int16LE.toString(16));
console.log('Int16 big endian:', int16BE.toString(16));

// Test Int32 operations
dv.setUint8(0, 0x12);
dv.setUint8(1, 0x34);
dv.setUint8(2, 0x56);
dv.setUint8(3, 0x78);
const int32LE = dv.getInt32(0, true);
const int32BE = dv.getInt32(0, false);
console.log('Int32 little endian:', int32LE.toString(16));
console.log('Int32 big endian:', int32BE.toString(16));

// Test Float32 operations
dv.setUint8(0, 0x3f);
dv.setUint8(1, 0x80);
dv.setUint8(2, 0x00);
dv.setUint8(3, 0x00);
const float32 = dv.getFloat32(0, false); // 1.0 in IEEE 754
console.log('Float32:', float32);

console.log('\n=== Buffer Tests (Node.js-style) ===');

// Test Buffer.alloc
const buf1 = Buffer.alloc(10);
console.log('Buffer.alloc length:', buf1.length);
console.log('Buffer.alloc byteLength:', buf1.byteLength);

// Test Buffer.from with string
const buf2 = Buffer.from('Hello');
console.log('Buffer.from string length:', buf2.length);
console.log('Buffer.from string toString:', buf2.toString());

// Test Buffer.from with array
const buf3 = Buffer.from([72, 101, 108, 108, 111]);
console.log('Buffer.from array length:', buf3.length);
console.log('Buffer.from array toString:', buf3.toString());

// Test Buffer write
const buf4 = Buffer.alloc(10);
const written = buf4.write('Hello', 0);
console.log('Bytes written:', written);
console.log('Buffer after write:', buf4.toString());

// Test Buffer toString with hex encoding
const buf5 = Buffer.from('Hello');
console.log('Buffer toString hex:', buf5.toString('hex'));

// Test Buffer toString with base64 encoding
const buf6 = Buffer.from('Hello World');
const base64 = buf6.toString('base64');
console.log('Buffer toString base64:', base64);

// Test Buffer.toBase64 method
const buf7 = Buffer.from('Test');
const base64_2 = buf7.toBase64();
console.log('Buffer.toBase64():', base64_2);

console.log('\n=== TypedArray slice and subarray ===');

// Test slice (creates a copy)
const original = new Uint8Array(10);
const sliced = original.slice(2, 8);
console.log('Original length:', original.length);
console.log('Sliced length:', sliced.length);
console.log('Sliced byteOffset:', sliced.byteOffset);

// Test subarray (creates a view)
const subarrayed = original.subarray(2, 8);
console.log('Subarray length:', subarrayed.length);
console.log('Subarray byteOffset:', subarrayed.byteOffset);

console.log('\n=== All tests completed ===');
