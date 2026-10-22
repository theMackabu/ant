const assert = require('node:assert');
const { Readable, Writable, Duplex, Transform, PassThrough, isDestroyed, isErrored } = require('node:stream');

const isAnt = !!process.versions.ant;
const tick = () => new Promise(resolve => setImmediate(resolve));

function collect(stream) {
  return new Promise((resolve, reject) => {
    const out = [];
    stream.on('data', chunk => out.push(typeof chunk === 'string' ? chunk : String(chunk)));
    stream.on('error', reject);
    stream.on('end', () => resolve(out));
  });
}

async function fromSyncIterator() {
  let i = 0;
  const source = {
    [Symbol.asyncIterator]() {
      return { next: () => (i < 3 ? { value: 'c' + i++, done: false } : { value: undefined, done: true }) };
    },
  };
  assert.deepStrictEqual(await collect(Readable.from(source)), ['c0', 'c1', 'c2']);
}

async function fromThenableIterator() {
  let i = 0;
  const source = {
    [Symbol.asyncIterator]() {
      return {
        next: () => ({ then: resolve => resolve(i < 2 ? { value: 't' + i++, done: false } : { done: true }) }),
      };
    },
  };
  assert.deepStrictEqual(await collect(Readable.from(source)), ['t0', 't1']);
}

async function stateObjects() {
  const r = new Readable({ read() {}, objectMode: true, highWaterMark: 4 });
  const state = r._readableState;
  assert.strictEqual(r._readableState, state);
  assert.strictEqual(state.highWaterMark, 4);
  assert.strictEqual(state.objectMode, true);
  assert.strictEqual(r._writableState, undefined);
  assert.strictEqual(Object.prototype.hasOwnProperty.call(r, 'destroyed'), false);

  r.push('ab');
  assert.strictEqual(state.length, 1);
  r.push('c');
  assert.strictEqual(state.length, 2);
  assert.strictEqual(r.read(), 'ab');
  assert.strictEqual(state.length, 1);

  r.destroyed = true;
  assert.strictEqual(isDestroyed(r), true);
  assert.strictEqual(state.destroyed, true);

  const w = new Writable({ write(chunk, enc, cb) { cb(); } });
  assert.strictEqual(w._readableState, undefined);
  assert.strictEqual(w._writableState.ended, false);
  assert.ok(!w.readable);
}

async function stateSetters() {
  if (!isAnt) return;
  const r = new Readable({ read() {}, objectMode: true });
  const state = r._readableState;
  const hwm = state.highWaterMark;

  state.highWaterMark = undefined;
  assert.strictEqual(state.highWaterMark, hwm);
  assert.throws(() => { state.highWaterMark = Symbol('x'); }, TypeError);
  assert.strictEqual(state.highWaterMark, hwm);

  r.push('a');
  r.push('b');
  assert.strictEqual(r.read(), 'a');
  state.buffer = [];
  r.push('c');
  assert.strictEqual(r.read(), 'c');
}

async function duplexAutoDestroy() {
  const d = new Duplex({ read() {}, write(chunk, enc, cb) { cb(); } });
  d._writableState.autoDestroy = false;
  d.on('data', () => {});
  d.push(null);
  d.end();
  await new Promise(resolve => d.on('finish', resolve));
  await tick();
  assert.strictEqual(d.destroyed, false);
}

async function defaultDestroy() {
  assert.strictEqual(typeof Readable.prototype._destroy, 'function');
  assert.strictEqual(typeof Writable.prototype._destroy, 'function');

  const seen = [];
  class R extends Readable {
    _read() {}
    _destroy(err, cb) {
      seen.push('destroy');
      super._destroy(err, cb);
    }
  }

  const r = new R();
  const err = new Error('boom');
  r.on('error', e => seen.push(e === err ? 'error' : 'wrong'));
  await new Promise(resolve => { r.on('close', resolve); r.destroy(err); });
  assert.deepStrictEqual(seen, ['destroy', 'error']);
  assert.strictEqual(isErrored(r), true);
}

async function flowingDataIsSynchronous() {
  const r = new Readable({ read() {} });
  const seen = [];
  r.on('data', chunk => seen.push(String(chunk)));
  await tick();
  r.push('x');
  assert.deepStrictEqual(seen, ['x']);
  r.push('y');
  r.push(null);
  await new Promise(resolve => r.on('end', resolve));
  assert.deepStrictEqual(seen, ['x', 'y']);
}

async function writableQueue() {
  const order = [];
  let pending = null;
  const w = new Writable({
    highWaterMark: 4,
    write(chunk, enc, cb) {
      order.push('write:' + chunk);
      if (String(chunk) === 'sy') return cb();
      pending = cb;
    },
  });

  assert.strictEqual(w.write('sy', () => order.push('cb:sy')), true);
  assert.strictEqual(w.write('a1', () => order.push('cb:a1')), true);
  assert.strictEqual(w.write('a2', () => order.push('cb:a2')), false);
  assert.strictEqual(w.writableLength, 4);
  assert.strictEqual(w.writableNeedDrain, true);

  const drained = new Promise(resolve => w.on('drain', resolve));
  const first = pending;
  pending = null;
  first();
  assert.ok(pending);
  pending();
  await drained;

  const finished = new Promise(resolve => w.on('finish', resolve));
  w.end('sy');
  await finished;
  await tick();
  assert.deepStrictEqual(order.filter(entry => entry.startsWith('write:')), ['write:sy', 'write:a1', 'write:a2', 'write:sy']);
  assert.deepStrictEqual(order.filter(entry => entry.startsWith('cb:')).sort(), ['cb:a1', 'cb:a2', 'cb:sy']);
  assert.strictEqual(w.writableFinished, true);
}

async function transformAndPipe() {
  const upper = new Transform({
    transform(chunk, enc, cb) {
      setImmediate(() => cb(null, String(chunk).toUpperCase()));
    },
  });

  const out = [];
  const sink = new Writable({ write(chunk, enc, cb) { out.push(String(chunk)); cb(); } });
  const done = new Promise(resolve => sink.on('finish', resolve));
  Readable.from(['a', 'b', 'c'], { objectMode: false }).pipe(upper).pipe(sink);
  await done;
  assert.deepStrictEqual(out, ['A', 'B', 'C']);
}

async function asyncIteration() {
  const p = new PassThrough();
  const expected = [];
  for (let i = 0; i < 40; i++) {
    expected.push('n' + i);
    p.write('n' + i);
  }
  p.end();

  const seen = [];
  for await (const chunk of p) {
    seen.push(String(chunk));
    if (seen.length % 7 === 0) await tick();
  }
  assert.strictEqual(seen.join(''), expected.join(''));
}

async function manyStreamsUnderPressure() {
  let ends = 0;
  let bytes = 0;
  for (let round = 0; round < 20; round++) {
    const streams = [];
    for (let i = 0; i < 200; i++) {
      const r = new Readable({ read() {} });
      r.on('data', chunk => { bytes += chunk.length; });
      r.on('end', () => ends++);
      r.push('payload-' + i);
      r.push(null);
      streams.push(r);
      const junk = [];
      for (let j = 0; j < 50; j++) junk.push({ j, s: 'x' + j });
    }
    await tick();
  }
  await tick();
  assert.strictEqual(ends, 4000);
  assert.ok(bytes > 0);
}

(async () => {
  await fromSyncIterator();
  await fromThenableIterator();
  await stateObjects();
  await stateSetters();
  await duplexAutoDestroy();
  await defaultDestroy();
  await flowingDataIsSynchronous();
  await writableQueue();
  await transformAndPipe();
  await asyncIteration();
  await manyStreamsUnderPressure();
  console.log('ok');
})().catch(err => {
  console.error(err);
  process.exit(1);
});
