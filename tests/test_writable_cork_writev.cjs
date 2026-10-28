const assert = require('node:assert');
const { Writable, Duplex } = require('node:stream');

const tick = () => new Promise(resolve => setImmediate(resolve));
const describe = chunk => Buffer.isBuffer(chunk) ? 'B' + chunk.length : typeof chunk === 'string' ? 'S' + chunk : JSON.stringify(chunk);

function writable(options = {}, events = []) {
  const stream = new Writable({
    ...options,
    write: options.noWrite ? undefined : (chunk, encoding, callback) => {
      events.push(`w(${describe(chunk)})`);
      if (options.asyncWrite) setImmediate(callback);
      else callback();
    },
    writev: options.noWritev ? undefined : (chunks, callback) => {
      events.push(`v[${chunks.map(c => describe(c.chunk) + ':' + c.encoding).join(',')}|${chunks.allBuffers}]`);
      callback(options.writevError);
    },
  });
  stream.on('error', err => events.push('error:' + err.message));
  return stream;
}

(async () => {
  assert.strictEqual(Writable.prototype._writev, null);

  let events = [];
  let w = writable({}, events);
  w.cork();
  assert.strictEqual(w.writableCorked, 1);
  w.write('a');
  w.write(Buffer.from('bb'));
  assert.strictEqual(w.writableLength, 3);
  w.uncork();
  assert.strictEqual(w.writableCorked, 0);
  assert.deepStrictEqual(events, ['v[B1:buffer,B2:buffer|true]']);

  events = [];
  w = writable({}, events);
  w.cork();
  w.cork();
  w.write('a');
  w.uncork();
  assert.deepStrictEqual(events, []);
  w.write('b');
  w.uncork();
  w.uncork();
  assert.strictEqual(w.writableCorked, 0);
  assert.deepStrictEqual(events, ['v[B1:buffer,B1:buffer|true]']);

  events = [];
  w = writable({}, events);
  w.cork();
  w.write('a');
  w.write('b');
  await new Promise(resolve => w.end('c', resolve));
  assert.deepStrictEqual(events, ['v[B1:buffer,B1:buffer,B1:buffer|true]']);

  events = [];
  w = writable({ noWritev: true }, events);
  w.cork();
  w.write('a');
  w.write('b');
  w.uncork();
  assert.deepStrictEqual(events, ['w(B1)', 'w(B1)']);

  events = [];
  w = writable({ asyncWrite: true }, events);
  w.write('a');
  w.write('b');
  w.write('c');
  await tick();
  await tick();
  assert.deepStrictEqual(events, ['w(B1)', 'v[B1:buffer,B1:buffer|true]']);

  events = [];
  w = writable({ noWrite: true }, events);
  w.write('a');
  assert.deepStrictEqual(events, ['v[B1:buffer|undefined]']);

  events = [];
  w = writable({ objectMode: true }, events);
  w.cork();
  w.write({ x: 1 });
  w.write('s');
  w.uncork();
  assert.deepStrictEqual(events, ['v[{"x":1}:undefined,Ss:undefined|false]']);

  events = [];
  w = writable({ decodeStrings: false }, events);
  w.cork();
  w.write('s');
  w.write(Buffer.from('b'));
  w.uncork();
  assert.deepStrictEqual(events, ['v[Ss:utf8,B1:buffer|false]']);

  events = [];
  const errors = [];
  w = writable({ writevError: new Error('vx') }, events);
  w.cork();
  w.write('a', err => errors.push(err && err.message));
  w.write('b', err => errors.push(err && err.message));
  w.uncork();
  await tick();
  assert.deepStrictEqual(errors, ['vx', 'vx']);

  events = [];
  w = writable({}, events);
  w.cork();
  w.write('a');
  w.destroy();
  w.uncork();
  await tick();
  assert.ok(!events.some(e => e.startsWith('v') || e.startsWith('w')));

  const seen = [];
  const d = new Duplex({ read() {}, write(c, e, cb) { seen.push('w'); cb(); }, writev(cs, cb) { seen.push('v' + cs.length); cb(); } });
  d.cork();
  d.write('a');
  d.write('b');
  d.uncork();
  assert.deepStrictEqual(seen, ['v2']);

  assert.throws(() => new Writable().write('a'), { code: 'ERR_METHOD_NOT_IMPLEMENTED' });

  console.log('ok');
})().catch(err => {
  console.error(err);
  process.exit(1);
});
