import { test, testThrows, summary } from './helpers.js';
import zlib from 'node:zlib';

console.log('node:zlib Tests\n');

test('zlib is object', typeof zlib, 'object');
test('zlib toStringTag', Object.prototype.toString.call(zlib), '[object zlib]');

test('constants exists', typeof zlib.constants, 'object');
test('Z_OK is 0', zlib.constants.Z_OK, 0);
test('Z_NO_FLUSH is 0', zlib.constants.Z_NO_FLUSH, 0);
test('Z_FINISH is 4', zlib.constants.Z_FINISH, 4);
test('Z_DEFAULT_COMPRESSION is -1', zlib.constants.Z_DEFAULT_COMPRESSION, -1);
test('Z_BEST_SPEED is 1', zlib.constants.Z_BEST_SPEED, 1);
test('Z_BEST_COMPRESSION is 9', zlib.constants.Z_BEST_COMPRESSION, 9);

test('createGzip is function', typeof zlib.createGzip, 'function');
test('createGunzip is function', typeof zlib.createGunzip, 'function');
test('createDeflate is function', typeof zlib.createDeflate, 'function');
test('createInflate is function', typeof zlib.createInflate, 'function');
test('createDeflateRaw is function', typeof zlib.createDeflateRaw, 'function');
test('createInflateRaw is function', typeof zlib.createInflateRaw, 'function');
test('createUnzip is function', typeof zlib.createUnzip, 'function');
test('createBrotliCompress is function', typeof zlib.createBrotliCompress, 'function');
test('createBrotliDecompress is function', typeof zlib.createBrotliDecompress, 'function');

test('gzipSync is function', typeof zlib.gzipSync, 'function');
test('gunzipSync is function', typeof zlib.gunzipSync, 'function');
test('deflateSync is function', typeof zlib.deflateSync, 'function');
test('inflateSync is function', typeof zlib.inflateSync, 'function');
test('deflateRawSync is function', typeof zlib.deflateRawSync, 'function');
test('inflateRawSync is function', typeof zlib.inflateRawSync, 'function');
test('unzipSync is function', typeof zlib.unzipSync, 'function');
test('brotliCompressSync is function', typeof zlib.brotliCompressSync, 'function');
test('brotliDecompressSync is function', typeof zlib.brotliDecompressSync, 'function');

test('gzip is function', typeof zlib.gzip, 'function');
test('gunzip is function', typeof zlib.gunzip, 'function');
test('deflate is function', typeof zlib.deflate, 'function');
test('inflate is function', typeof zlib.inflate, 'function');
test('crc32 is function', typeof zlib.crc32, 'function');

const gz = zlib.createGzip();
test('createGzip returns object', typeof gz, 'object');
test('createGzip has write', typeof gz.write, 'function');
test('createGzip has end', typeof gz.end, 'function');
test('createGzip has on', typeof gz.on, 'function');
test('createGzip has pipe', typeof gz.pipe, 'function');
test('createGzip has destroy', typeof gz.destroy, 'function');
test('createGzip readable=true', gz.readable, true);
test('createGzip writable=true', gz.writable, true);
test('createGzip bytesWritten starts 0', gz.bytesWritten, 0);

const gun = zlib.createGunzip();
test('createGunzip has write', typeof gun.write, 'function');
test('createGunzip readable=true', gun.readable, true);

const inf = zlib.createInflate();
test('createInflate has write', typeof inf.write, 'function');

const br = zlib.createBrotliDecompress();
test('createBrotliDecompress returns object', typeof br, 'object');
test('createBrotliDecompress has write', typeof br.write, 'function');

const input = Buffer.from('Hello, zlib world!');

const gzipped = zlib.gzipSync(input);
test('gzipSync returns Buffer', gzipped instanceof Uint8Array, true);
test('gzipSync has gzip magic bytes', gzipped[0], 0x1f);
test('gzipSync magic byte 2', gzipped[1], 0x8b);
test('gzipSync result is smaller for repetitive input', gzipped.length > 0, true);

const gunzipped = zlib.gunzipSync(gzipped);
test('gunzipSync roundtrip length', gunzipped.length, input.length);
test('gunzipSync roundtrip value', Buffer.from(gunzipped).toString(), 'Hello, zlib world!');

const deflated = zlib.deflateSync(input);
test('deflateSync returns Buffer', deflated instanceof Uint8Array, true);
test('deflateSync result non-empty', deflated.length > 0, true);

const inflated = zlib.inflateSync(deflated);
test('inflateSync roundtrip', Buffer.from(inflated).toString(), 'Hello, zlib world!');

const deflatedRaw = zlib.deflateRawSync(input);
test('deflateRawSync result non-empty', deflatedRaw.length > 0, true);

const inflatedRaw = zlib.inflateRawSync(deflatedRaw);
test('inflateRawSync roundtrip', Buffer.from(inflatedRaw).toString(), 'Hello, zlib world!');

const brotliCompressed = zlib.brotliCompressSync(input);
test('brotliCompressSync result non-empty', brotliCompressed.length > 0, true);

const brotliDecompressed = zlib.brotliDecompressSync(brotliCompressed);
test('brotliDecompressSync roundtrip', Buffer.from(brotliDecompressed).toString(), 'Hello, zlib world!');

const unzipped = zlib.unzipSync(gzipped);
test('unzipSync gzip roundtrip', Buffer.from(unzipped).toString(), 'Hello, zlib world!');

const fromString = zlib.gzipSync('hello string');
test('gzipSync accepts string', fromString instanceof Uint8Array, true);
const fromStringBack = zlib.gunzipSync(fromString);
test('gunzipSync string roundtrip', Buffer.from(fromStringBack).toString(), 'hello string');

const repetitive = Buffer.from('a'.repeat(1000));
const compressedRep = zlib.gzipSync(repetitive);
test('gzip compresses repetitive data', compressedRep.length < repetitive.length, true);

const crc0 = zlib.crc32('hello');
test('crc32 returns number', typeof crc0, 'number');
test('crc32 hello is consistent', crc0, zlib.crc32('hello'));
test('crc32 chaining', zlib.crc32('world', zlib.crc32('hello')) !== zlib.crc32('hello'), true);
test('crc32 different inputs differ', zlib.crc32('hello') !== zlib.crc32('world'), true);

function testAsyncGzip() {
  return new Promise((resolve) => {
    zlib.gzip(input, (err, result) => {
      test('async gzip no error', err, null);
      test('async gzip returns Buffer', result instanceof Uint8Array, true);
      test('async gzip magic byte', result[0], 0x1f);

      zlib.gunzip(result, (err2, result2) => {
        test('async gunzip no error', err2, null);
        test('async gunzip roundtrip', Buffer.from(result2).toString(), 'Hello, zlib world!');
        resolve();
      });
    });
  });
}

function testAsyncDeflate() {
  return new Promise((resolve) => {
    zlib.deflate(input, (err, result) => {
      test('async deflate no error', err, null);
      test('async deflate result non-empty', result.length > 0, true);

      zlib.inflate(result, (err2, result2) => {
        test('async inflate no error', err2, null);
        test('async inflate roundtrip', Buffer.from(result2).toString(), 'Hello, zlib world!');
        resolve();
      });
    });
  });
}

function testAsyncBrotli() {
  return new Promise((resolve) => {
    zlib.brotliCompress(input, (err, result) => {
      test('async brotliCompress no error', err, null);
      test('async brotliCompress result non-empty', result.length > 0, true);

      zlib.brotliDecompress(result, (err2, result2) => {
        test('async brotliDecompress no error', err2, null);
        test('async brotliDecompress roundtrip', Buffer.from(result2).toString(), 'Hello, zlib world!');
        resolve();
      });
    });
  });
}

function testTransformStream() {
  return new Promise((resolve) => {
    const chunks = [];
    const stream = zlib.createGunzip();

    stream.on('data', (chunk) => chunks.push(chunk));
    stream.on('end', () => {
      let total = 0;
      for (const c of chunks) total += c.length;
      const out = new Uint8Array(total);
      let off = 0;
      for (const c of chunks) { out.set(c, off); off += c.length; }
      test('transform stream roundtrip', Buffer.from(out).toString(), 'Hello, zlib world!');
      resolve();
    });

    const compressed = zlib.gzipSync(input);
    stream.write(compressed);
    stream.end();
  });
}

function testBytesWritten() {
  const stream = zlib.createDeflate();
  const data = Buffer.from('track bytes written');
  stream.write(data);
  test('bytesWritten after write', stream.bytesWritten, data.length);
}

testBytesWritten();

await testAsyncGzip();
await testAsyncDeflate();
await testAsyncBrotli();
await testTransformStream();

summary();
