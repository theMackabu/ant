const assert = require('node:assert');
const { Writable, Transform } = require('node:stream');

function sink(options = {}) {
  const got = [];
  const stream = new Writable({
    ...options,
    write(chunk, encoding, callback) {
      got.push({ chunk, encoding });
      callback();
    },
  });
  stream.got = got;
  return stream;
}

function finished(stream) {
  return new Promise(resolve => stream.on('finish', resolve));
}

function code(fn) {
  try {
    fn();
  } catch (err) {
    return err.code;
  }
  return 'none';
}

(async () => {
  const w = sink();
  w.write('é');
  w.write('é', 'latin1');
  w.write(Buffer.from('xy'));
  w.write(new Uint8Array([1, 2]));
  w.write(new Uint16Array([1, 2]));
  w.write(new DataView(new ArrayBuffer(3)));
  assert.strictEqual(code(() => w.write(1)), 'ERR_INVALID_ARG_TYPE');
  assert.strictEqual(code(() => w.write({})), 'ERR_INVALID_ARG_TYPE');
  assert.strictEqual(code(() => w.write(undefined)), 'ERR_INVALID_ARG_TYPE');
  assert.strictEqual(code(() => w.write(null)), 'ERR_STREAM_NULL_VALUES');
  assert.strictEqual(code(() => w.write('x', 'nope')), 'ERR_UNKNOWN_ENCODING');
  w.end();
  await finished(w);

  assert.deepStrictEqual(w.got.map(g => g.encoding), ['buffer', 'buffer', 'buffer', 'buffer', 'buffer', 'buffer']);
  assert.ok(w.got.every(g => Buffer.isBuffer(g.chunk)));
  assert.deepStrictEqual(w.got.map(g => g.chunk.length), [2, 1, 2, 2, 4, 3]);

  const raw = sink({ decodeStrings: false });
  raw.write('str');
  raw.write('str', 'latin1');
  assert.strictEqual(raw.writableLength, 0);
  raw.end();
  await finished(raw);
  assert.deepStrictEqual(raw.got.map(g => [typeof g.chunk, g.encoding]), [['string', 'utf8'], ['string', 'latin1']]);

  const latin = sink({ defaultEncoding: 'latin1' });
  latin.write('é');
  latin.end();
  await finished(latin);
  assert.strictEqual(latin.got[0].chunk.length, 1);

  assert.strictEqual(code(() => sink({ defaultEncoding: 'nope' })), 'ERR_UNKNOWN_ENCODING');

  const hex = sink();
  assert.strictEqual(hex.setDefaultEncoding('HEX'), hex);
  assert.strictEqual(code(() => hex.setDefaultEncoding('nope')), 'ERR_UNKNOWN_ENCODING');
  hex.write('ff');
  hex.end();
  await finished(hex);
  assert.deepStrictEqual([...hex.got[0].chunk], [255]);

  const obj = sink({ objectMode: true });
  obj.write({ a: 1 });
  obj.write(1);
  obj.write('x', 'hex');
  assert.strictEqual(code(() => obj.write(null)), 'ERR_STREAM_NULL_VALUES');
  obj.end();
  await finished(obj);
  assert.deepStrictEqual(obj.got.map(g => g.encoding), [undefined, undefined, 'hex']);

  const ended = sink();
  ended.on('error', () => {});
  assert.strictEqual(code(() => ended.end(1)), 'ERR_INVALID_ARG_TYPE');
  assert.strictEqual(ended.writableEnded, false);

  const seen = [];
  const t = new Transform({ transform(chunk, encoding, callback) { seen.push(encoding); callback(null, chunk); } });
  t.resume();
  t.write('s');
  t.write(new Uint8Array([1]));
  t.end();
  await new Promise(resolve => t.on('finish', resolve));
  assert.deepStrictEqual(seen, ['buffer', 'buffer']);

  console.log('ok');
})().catch(err => {
  console.error(err);
  process.exit(1);
});
