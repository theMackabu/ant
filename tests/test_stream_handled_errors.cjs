// Stream failures that become rejections or callback arguments must not also
// escape through uncaughtException. Public callback throws must still escape.
const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-stream-errors-'));
let index = 0;

function run(source) {
  const file = path.join(dir, `case${index++}.cjs`);
  fs.writeFileSync(file, source);
  return spawnSync(process.execPath, [file], { encoding: 'utf8', timeout: 5000 });
}

const helpers = `
function assert(condition, message) {
  if (!condition) throw new Error(message);
}
function rejects(promise, expected) {
  // A JS catch could incidentally consume the stale native exception under test.
  return promise.then(
    () => { throw new Error('operation did not reject'); },
    error => {
      if (typeof expected === 'function') assert(error instanceof expected, 'wrong error type');
      else assert(error === expected, 'rejection lost its original reason');
    }
  );
}
`;

const handled = {
  writableWrite: `
    const reason = new Error('sink write');
    const writer = new WritableStream({ write() { throw reason; } }).getWriter();
    writer.closed.catch(() => {});
    await rejects(writer.write('x'), reason);
  `,
  writableClose: `
    const reason = new Error('sink close');
    const writer = new WritableStream({ close() { throw reason; } }).getWriter();
    writer.closed.catch(() => {});
    await rejects(writer.close(), reason);
  `,
  writableAbort: `
    const reason = new Error('sink abort');
    const writer = new WritableStream({ abort() { throw reason; } }).getWriter();
    writer.closed.catch(() => {});
    await rejects(writer.abort(), reason);
  `,
  writablePrimitive: `
    const writer = new WritableStream({ write() { throw undefined; } }).getWriter();
    writer.closed.catch(() => {});
    await rejects(writer.write('x'), undefined);
  `,
  writableSize: `
    const reason = new Error('strategy size');
    const writer = new WritableStream({}, { size() { throw reason; } }).getWriter();
    writer.closed.catch(() => {});
    await rejects(writer.write('x'), reason);
  `,
  writableInvalidSize: `
    const writer = new WritableStream({}, { size() { return -1; } }).getWriter();
    writer.closed.catch(() => {});
    await rejects(writer.write('x'), RangeError);
  `,
  writableSizeConversion: `
    const reason = new Error('strategy conversion');
    const writer = new WritableStream({}, { size() {
      return { valueOf() { throw reason; } };
    } }).getWriter();
    writer.closed.catch(() => {});
    await rejects(writer.write('x'), reason);
  `,
  writableSizeConversionReasons: `
    for (const reason of [undefined, null, 0, 'coercion', new Error('coercion')]) {
      for (const key of ['valueOf', Symbol.toPrimitive]) {
        const writer = new WritableStream({}, { size() {
          return { [key]() { throw reason; } };
        } }).getWriter();
        writer.closed.catch(() => {});
        await rejects(writer.write('x'), reason);
      }
    }
    for (const size of [NaN, Infinity]) {
      const writer = new WritableStream({}, { size() { return size; } }).getWriter();
      writer.closed.catch(() => {});
      await rejects(writer.write('x'), RangeError);
    }
  `,
  writableClosed: `
    const writer = new WritableStream().getWriter();
    await writer.close();
    await rejects(writer.write('x'), TypeError);
    await rejects(writer.close(), TypeError);
  `,
  writableLocked: `
    const stream = new WritableStream();
    const writer = stream.getWriter();
    await rejects(stream.abort(), TypeError);
    await rejects(stream.close(), TypeError);
    writer.releaseLock();
    await rejects(writer.write('x'), TypeError);
    await rejects(writer.close(), TypeError);
    await rejects(writer.abort(), TypeError);
  `,
  readablePull: `
    const reason = new Error('source pull');
    const reader = new ReadableStream({ pull() { throw reason; } }).getReader();
    reader.closed.catch(() => {});
    await rejects(reader.read(), reason);
  `,
  readablePullAfterClose: `
    const reader = new ReadableStream({ pull(controller) {
      controller.close();
      throw new Error('closed source pull');
    } }).getReader();
    assert((await reader.read()).done, 'reader did not close');
  `,
  readableCancel: `
    const reason = new Error('source cancel');
    const stream = new ReadableStream({ cancel() { throw reason; } });
    await rejects(stream.cancel(), reason);
  `,
  readableLocked: `
    const stream = new ReadableStream();
    const reader = stream.getReader();
    await rejects(stream.cancel(), TypeError);
    reader.releaseLock();
    await rejects(reader.cancel(), TypeError);
  `,
  readableSizeConversion: `
    const reason = new Error('readable strategy conversion');
    let controller;
    const stream = new ReadableStream({ start(value) { controller = value; } }, {
      size() { return { valueOf() { throw reason; } }; }
    });
    const reader = stream.getReader();
    const closed = rejects(reader.closed, reason);
    let caught = false;
    try { controller.enqueue('x'); }
    catch (error) { caught = true; assert(error === reason, 'enqueue lost its thrown reason'); }
    assert(caught, 'enqueue did not throw');
    await closed;
  `,
  readableSizeConversionReasons: `
    for (const reason of [undefined, null, 0, 'coercion', new Error('coercion')]) {
      let controller;
      const stream = new ReadableStream({ start(c) { controller = c; } }, {
        size() { return { valueOf() { throw reason; } }; }
      });
      const reader = stream.getReader();
      const closed = rejects(reader.closed, reason);
      let caught = false;
      try { controller.enqueue('x'); }
      catch (error) { caught = true; assert(error === reason, 'lost coercion reason'); }
      assert(caught, 'enqueue did not throw');
      await closed;
    }
  `,
  pipeToLocked: `
    const source = new ReadableStream();
    const reader = source.getReader();
    await rejects(source.pipeTo(new WritableStream()), TypeError);
    reader.releaseLock();
    const dest = new WritableStream();
    const writer = dest.getWriter();
    await rejects(source.pipeTo(dest), TypeError);
    writer.releaseLock();
  `,
  pipeToInvalid: `
    const source = new ReadableStream();
    await rejects(source.pipeTo({}), TypeError);
    await rejects(source.pipeTo(new WritableStream(), { signal: {} }), TypeError);
  `,
  pipeToOptionsGetter: `
    const reason = new Error('pipe options');
    const source = new ReadableStream();
    await rejects(source.pipeTo(new WritableStream(), {
      get signal() { throw reason; }
    }), reason);
  `,
  transformPrimitive: `
    const stream = new TransformStream({ transform() { throw 'transform reason'; } });
    const reader = stream.readable.getReader();
    const writer = stream.writable.getWriter();
    reader.closed.catch(() => {});
    writer.closed.catch(() => {});
    const read = rejects(reader.read(), 'transform reason');
    await rejects(writer.write('x'), 'transform reason');
    await read;
  `,
  zlibCallback: `
    await new Promise((resolve) => {
      require('zlib').gunzip(Buffer.from('invalid gzip'), error => {
        assert(error instanceof Error, 'zlib callback did not receive an Error');
        assert(typeof error.message === 'string', 'zlib callback lost error message');
        resolve();
      });
    });
  `,
  zlibErrorEvent: `
    await new Promise((resolve) => {
      const stream = require('zlib').createGunzip();
      stream.on('error', error => {
        assert(error instanceof Error, 'zlib error event did not receive an Error');
        resolve();
      });
      stream.write(Buffer.from('invalid gzip'));
    });
  `,
  nodeWriteAfterEnd: `
    await new Promise((resolve, reject) => {
      const stream = new (require('stream').Writable)({ write(chunk, encoding, done) { done(); } });
      stream.on('error', reject);
      stream.end();
      setTimeout(() => stream.write('x', error => {
        assert(error instanceof Error, 'write callback did not receive an Error');
        assert(error.message.includes('write after end'), 'write callback lost error message');
        resolve();
      }), 0);
    });
  `,
  nodeFinishedInvalid: `
    await rejects(require('stream/promises').finished(null), TypeError);
  `,
  nodeWriteThrows: `
    const { Writable } = require('stream');
    for (const reason of [undefined, null, false, 0, 'write failure', new Error('write failure')]) {
      for (const method of ['write', 'end']) {
        let callbacks = 0;
        let events = 0;
        const stream = new Writable({ write() { throw reason; } });
        stream.on('error', () => events++);
        let caught = false;
        try { stream[method]('x', () => callbacks++); }
        catch (error) { caught = true; assert(error === reason, 'lost thrown write reason'); }
        assert(caught, 'write did not throw');
        assert(callbacks === 0, 'throw was delivered as callback completion');
        assert(events === 0, 'throw was delivered as an error event');
      }
    }
  `,
  nodeBufferedWriteThrows: `
    const { Writable } = require('stream');
    for (const reason of [undefined, null, new Error('buffered write failure')]) {
      let completeFirst;
      let writes = 0;
      let callbacks = 0;
      const stream = new Writable({ write(chunk, encoding, done) {
        if (++writes === 1) completeFirst = done;
        else throw reason;
      } });
      stream.write('first');
      stream.write('second', () => callbacks++);
      let caught = false;
      try { completeFirst(); }
      catch (error) { caught = true; assert(error === reason, 'lost buffered write reason'); }
      assert(caught, 'buffered write did not throw');
      assert(callbacks === 0, 'buffered throw was delivered as callback completion');
    }
  `,
};

const escaping = {
  zlibCallback: `
    require('zlib').gunzip(Buffer.from('invalid gzip'), () => { throw new Error('public-callback'); });
  `,
  zlibErrorEvent: `
    const stream = require('zlib').createGunzip();
    stream.on('error', () => { throw new Error('public-callback'); });
    stream.write(Buffer.from('invalid gzip'));
  `,
  nodeWriteCallback: `
    const stream = new (require('stream').Writable)({ write(chunk, encoding, done) { done(); } });
    stream.write('x', () => { throw new Error('public-callback'); });
  `,
  nodeWriteErrorCallback: `
    const stream = new (require('stream').Writable)({ write(chunk, encoding, done) { done(); } });
    stream.end();
    stream.write('x', () => { throw new Error('public-callback'); });
  `,
};

try {
  for (const [name, source] of Object.entries(handled)) {
    for (const listener of [false, true]) {
      const result = run(helpers +
        (listener ? `process.on('uncaughtException', e => console.log('UNCAUGHT ' + e));\n` : '') + `
        const watchdog = setTimeout(() => { console.error('TIMEOUT'); process.exit(2); }, 1000);
        (async () => {
          ${source}
          clearTimeout(watchdog);
          console.log('HANDLED');
          setTimeout(() => console.log('LATER'), 20);
        })().catch(error => {
          console.error('FAIL', error);
          process.exit(1);
        });
      `);
      const detail = `${name} listener=${listener}: exit=${result.status}, stdout=${result.stdout}, stderr=${result.stderr}`;
      assert(result.status === 0, detail);
      assert(result.stdout.includes('HANDLED') && result.stdout.includes('LATER'), detail);
      assert(!result.stdout.includes('UNCAUGHT'), detail);
      assert(result.stderr === '', detail);
    }
  }

  for (const [name, source] of Object.entries(escaping)) {
    const result = run(`
      setTimeout(() => { ${source} }, 0);
      setTimeout(() => console.log('LATER'), 30);
    `);
    const detail = `${name}: exit=${result.status}, stdout=${result.stdout}, stderr=${result.stderr}`;
    assert(result.status === 1 && result.stderr.includes('public-callback'), detail);
    assert(!result.stdout.includes('LATER'), detail);
  }
} finally {
  fs.rmSync(dir, { recursive: true, force: true });
}

console.log('PASS');
