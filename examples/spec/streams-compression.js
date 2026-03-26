import { test, testThrows, summary } from './helpers.js';

console.log('CompressionStream / DecompressionStream Tests\n');

test('CS typeof', typeof CompressionStream, 'function');
test('CS toStringTag', Object.prototype.toString.call(new CompressionStream('gzip')), '[object CompressionStream]');

testThrows('CS requires new', () => CompressionStream('gzip'));
testThrows('CS requires format', () => new CompressionStream());
testThrows('CS rejects invalid format', () => new CompressionStream('invalid'));

const cs0 = new CompressionStream('gzip');
test('CS readable is ReadableStream', cs0.readable instanceof ReadableStream, true);
test('CS writable is WritableStream', cs0.writable instanceof WritableStream, true);

test('DS typeof', typeof DecompressionStream, 'function');
test('DS toStringTag', Object.prototype.toString.call(new DecompressionStream('gzip')), '[object DecompressionStream]');

testThrows('DS requires new', () => DecompressionStream('gzip'));
testThrows('DS requires format', () => new DecompressionStream());
testThrows('DS rejects invalid format', () => new DecompressionStream('invalid'));

const ds0 = new DecompressionStream('gzip');
test('DS readable is ReadableStream', ds0.readable instanceof ReadableStream, true);
test('DS writable is WritableStream', ds0.writable instanceof WritableStream, true);

async function collectStream(readable) {
  const reader = readable.getReader();
  const chunks = [];
  while (true) {
    const { value, done } = await reader.read();
    if (done) break;
    chunks.push(value);
  }
  let total = 0;
  for (const c of chunks) total += c.byteLength;
  const result = new Uint8Array(total);
  let offset = 0;
  for (const c of chunks) {
    result.set(c, offset);
    offset += c.byteLength;
  }
  return result;
}

async function testGzipRoundTrip() {
  const input = new TextEncoder().encode('Hello, compression world!');
  const cs = new CompressionStream('gzip');
  const writer = cs.writable.getWriter();
  writer.write(input);
  writer.close();
  const compressed = await collectStream(cs.readable);
  test('gzip compressed is smaller or has gzip header', compressed[0], 0x1f);
  test('gzip compressed header byte 2', compressed[1], 0x8b);

  const ds = new DecompressionStream('gzip');
  const writer2 = ds.writable.getWriter();
  writer2.write(compressed);
  writer2.close();
  const decompressed = await collectStream(ds.readable);
  test('gzip roundtrip length', decompressed.length, input.length);
  test('gzip roundtrip value', new TextDecoder().decode(decompressed), 'Hello, compression world!');
}

async function testDeflateRoundTrip() {
  const input = new TextEncoder().encode('Deflate test data');
  const cs = new CompressionStream('deflate');
  const writer = cs.writable.getWriter();
  writer.write(input);
  writer.close();
  const compressed = await collectStream(cs.readable);
  test('deflate compressed length > 0', compressed.length > 0, true);

  const ds = new DecompressionStream('deflate');
  const writer2 = ds.writable.getWriter();
  writer2.write(compressed);
  writer2.close();
  const decompressed = await collectStream(ds.readable);
  test('deflate roundtrip', new TextDecoder().decode(decompressed), 'Deflate test data');
}

async function testDeflateRawRoundTrip() {
  const input = new TextEncoder().encode('Raw deflate test');
  const cs = new CompressionStream('deflate-raw');
  const writer = cs.writable.getWriter();
  writer.write(input);
  writer.close();
  const compressed = await collectStream(cs.readable);
  test('deflate-raw compressed length > 0', compressed.length > 0, true);

  const ds = new DecompressionStream('deflate-raw');
  const writer2 = ds.writable.getWriter();
  writer2.write(compressed);
  writer2.close();
  const decompressed = await collectStream(ds.readable);
  test('deflate-raw roundtrip', new TextDecoder().decode(decompressed), 'Raw deflate test');
}

async function testPipeThrough() {
  const input = 'pipe through compression test';
  const encoded = new TextEncoder().encode(input);
  const rs = new ReadableStream({
    start(c) {
      c.enqueue(encoded);
      c.close();
    }
  });
  const compressed = await collectStream(rs.pipeThrough(new CompressionStream('gzip')));

  const rs2 = new ReadableStream({
    start(c) {
      c.enqueue(compressed);
      c.close();
    }
  });
  const decompressed = await collectStream(rs2.pipeThrough(new DecompressionStream('gzip')));
  test('pipeThrough roundtrip', new TextDecoder().decode(decompressed), input);
}

async function testMultipleChunks() {
  const cs = new CompressionStream('gzip');
  const writer = cs.writable.getWriter();
  writer.write(new TextEncoder().encode('chunk1 '));
  writer.write(new TextEncoder().encode('chunk2 '));
  writer.write(new TextEncoder().encode('chunk3'));
  writer.close();
  const compressed = await collectStream(cs.readable);

  const ds = new DecompressionStream('gzip');
  const writer2 = ds.writable.getWriter();
  writer2.write(compressed);
  writer2.close();
  const decompressed = await collectStream(ds.readable);
  test('multi-chunk roundtrip', new TextDecoder().decode(decompressed), 'chunk1 chunk2 chunk3');
}

async function testLargeData() {
  const str = 'x'.repeat(100000);
  const input = new TextEncoder().encode(str);
  const cs = new CompressionStream('gzip');
  const writer = cs.writable.getWriter();
  writer.write(input);
  writer.close();
  const compressed = await collectStream(cs.readable);
  test('large data compresses', compressed.length < input.length, true);

  const ds = new DecompressionStream('gzip');
  const writer2 = ds.writable.getWriter();
  writer2.write(compressed);
  writer2.close();
  const decompressed = await collectStream(ds.readable);
  test('large data roundtrip length', decompressed.length, input.length);
}

await testGzipRoundTrip();
await testDeflateRoundTrip();
await testDeflateRawRoundTrip();
await testPipeThrough();
await testMultipleChunks();
await testLargeData();

summary();
