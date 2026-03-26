import { test, testDeep, testThrows, summary } from './helpers.js';

console.log('TextEncoderStream / TextDecoderStream Tests\n');

test('TES typeof', typeof TextEncoderStream, 'function');
test('TES toStringTag', Object.prototype.toString.call(new TextEncoderStream()), '[object TextEncoderStream]');

testThrows('TES requires new', () => TextEncoderStream());

const tes0 = new TextEncoderStream();
test('TES encoding', tes0.encoding, 'utf-8');
test('TES readable is ReadableStream', tes0.readable instanceof ReadableStream, true);
test('TES writable is WritableStream', tes0.writable instanceof WritableStream, true);

async function testTESBasic() {
  const tes = new TextEncoderStream();
  const writer = tes.writable.getWriter();
  const reader = tes.readable.getReader();
  writer.write('hello');
  const { value } = await reader.read();
  test('TES basic encode type', value instanceof Uint8Array, true);
  testDeep('TES basic encode value', Array.from(value), [104, 101, 108, 108, 111]);
  writer.close();
}

async function testTESEmpty() {
  const tes = new TextEncoderStream();
  const writer = tes.writable.getWriter();
  const reader = tes.readable.getReader();
  writer.write('a');
  writer.close();
  const { value } = await reader.read();
  testDeep('TES non-empty chunk', Array.from(value), [97]);
  const { done } = await reader.read();
  test('TES stream closes', done, true);
}

async function testTESMultiChunks() {
  const tes = new TextEncoderStream();
  const writer = tes.writable.getWriter();
  const reader = tes.readable.getReader();
  writer.write('abc');
  writer.write('def');
  const r1 = await reader.read();
  const r2 = await reader.read();
  testDeep('TES chunk1', Array.from(r1.value), [97, 98, 99]);
  testDeep('TES chunk2', Array.from(r2.value), [100, 101, 102]);
}

async function testTESUnicode() {
  const tes = new TextEncoderStream();
  const writer = tes.writable.getWriter();
  const reader = tes.readable.getReader();
  writer.write('café');
  const { value } = await reader.read();
  testDeep('TES unicode', Array.from(value), [99, 97, 102, 195, 169]);
}

async function testTESPipeThrough() {
  const rs = new ReadableStream({
    start(c) {
      c.enqueue('hello');
      c.close();
    }
  });
  const tes = new TextEncoderStream();
  const reader = rs.pipeThrough(tes).getReader();
  const { value } = await reader.read();
  test('TES pipeThrough type', value instanceof Uint8Array, true);
  testDeep('TES pipeThrough value', Array.from(value), [104, 101, 108, 108, 111]);
}

test('TDS typeof', typeof TextDecoderStream, 'function');
test('TDS toStringTag', Object.prototype.toString.call(new TextDecoderStream()), '[object TextDecoderStream]');
testThrows('TDS requires new', () => TextDecoderStream());

const tds0 = new TextDecoderStream();
test('TDS encoding default', tds0.encoding, 'utf-8');
test('TDS fatal default', tds0.fatal, false);
test('TDS ignoreBOM default', tds0.ignoreBOM, false);
test('TDS readable is ReadableStream', tds0.readable instanceof ReadableStream, true);
test('TDS writable is WritableStream', tds0.writable instanceof WritableStream, true);

const tds1 = new TextDecoderStream('utf-8', { fatal: true, ignoreBOM: true });
test('TDS fatal option', tds1.fatal, true);
test('TDS ignoreBOM option', tds1.ignoreBOM, true);

testThrows('TDS invalid encoding', () => new TextDecoderStream('invalid-encoding'));

async function testTDSBasic() {
  const tds = new TextDecoderStream();
  const writer = tds.writable.getWriter();
  const reader = tds.readable.getReader();
  writer.write(new Uint8Array([104, 101, 108, 108, 111]));
  const { value } = await reader.read();
  test('TDS basic decode', value, 'hello');
  writer.close();
}

async function testTDSMultiChunks() {
  const tds = new TextDecoderStream();
  const writer = tds.writable.getWriter();
  const reader = tds.readable.getReader();
  writer.write(new Uint8Array([97, 98, 99]));
  writer.write(new Uint8Array([100, 101, 102]));
  const r1 = await reader.read();
  const r2 = await reader.read();
  test('TDS chunk1', r1.value, 'abc');
  test('TDS chunk2', r2.value, 'def');
}

async function testTDSSplitMultibyte() {
  const tds = new TextDecoderStream();
  const writer = tds.writable.getWriter();
  const reader = tds.readable.getReader();
  writer.write(new Uint8Array([99, 97, 102, 0xc3]));
  writer.write(new Uint8Array([0xa9]));
  writer.close();
  const chunks = [];
  while (true) {
    const { value, done } = await reader.read();
    if (done) break;
    if (value) chunks.push(value);
  }
  test('TDS split multibyte', chunks.join(''), 'café');
}

async function testTDSPipeThrough() {
  const rs = new ReadableStream({
    start(c) {
      c.enqueue(new Uint8Array([104, 101, 108, 108, 111]));
      c.close();
    }
  });
  const tds = new TextDecoderStream();
  const reader = rs.pipeThrough(tds).getReader();
  const { value } = await reader.read();
  test('TDS pipeThrough', value, 'hello');
}

async function testRoundTrip() {
  const rs = new ReadableStream({
    start(c) {
      c.enqueue('hello world');
      c.close();
    }
  });
  const reader = rs.pipeThrough(new TextEncoderStream()).pipeThrough(new TextDecoderStream()).getReader();
  const { value } = await reader.read();
  test('encode→decode roundtrip', value, 'hello world');
}

async function testTDSUTF16LE() {
  const tds = new TextDecoderStream('utf-16le');
  test('TDS utf-16le encoding', tds.encoding, 'utf-16le');
  const writer = tds.writable.getWriter();
  const reader = tds.readable.getReader();
  writer.write(new Uint8Array([0x68, 0x00, 0x69, 0x00]));
  const { value } = await reader.read();
  test('TDS utf-16le decode', value, 'hi');
}

await testTESBasic();
await testTESEmpty();
await testTESMultiChunks();
await testTESUnicode();
await testTESPipeThrough();
await testTDSBasic();
await testTDSMultiChunks();
await testTDSSplitMultibyte();
await testTDSPipeThrough();
await testRoundTrip();
await testTDSUTF16LE();

summary();
