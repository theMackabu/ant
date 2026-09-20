const zlib = require('node:zlib');
const invalid = Buffer.from('invalid gzip');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function expectThrow(action, reason, label) {
  let caught = false;
  try { action(); }
  catch (error) {
    caught = true;
    assert(error === reason, label + ': original throw changed');
  }
  assert(caught, label + ': throw was not caught locally');
}

function streamFor(method) {
  const stream = zlib.createGunzip();
  if (method !== 'write') {
    // Keep the inflater's ordinary processing failure for end/flush to report.
    let errors = 0;
    const handle = () => errors++;
    stream.on('error', handle);
    assert(stream.write(invalid) === false, 'invalid write return changed');
    assert(errors === 1, 'invalid write error was not handled');
    stream.removeListener('error', handle);
  }
  return stream;
}

function invoke(stream, method, callback) {
  if (method === 'write') return stream.write(invalid, callback);
  return stream[method](callback);
}

let queued = false;
queueMicrotask(() => { queued = true; });

// Ant dispatches these processing errors synchronously. A listener or error
// callback throw must reach the surrounding catch at that same boundary.
for (const reason of [new Error('zlib listener'), undefined, null, false, 0, 'listener']) {
  for (const method of ['write', 'end', 'flush']) {
    const stream = streamFor(method);
    let errors = 0;
    let callbacks = 0;
    stream.on('error', error => {
      errors++;
      assert(error instanceof Error, method + ': processing error changed');
      throw reason;
    });
    expectThrow(() => invoke(stream, method, () => callbacks++), reason, method + ' listener');
    assert(errors === 1, method + ': listener did not run once');
    assert(callbacks === 0, method + ': callback ran after listener threw');
    stream.close();
  }

  const stream = streamFor('flush');
  let processingError;
  let callbacks = 0;
  stream.on('error', error => { processingError = error; });
  expectThrow(() => stream.flush(error => {
    callbacks++;
    assert(error instanceof Error && error === processingError, 'flush callback error changed');
    throw reason;
  }), reason, 'flush error callback');
  assert(callbacks === 1, 'flush error callback did not run once');
  stream.close();
}

for (const method of ['write', 'end', 'flush']) {
  const stream = streamFor(method);
  let processingError;
  let errors = 0;
  let callbacks = 0;
  stream.on('error', error => {
    errors++;
    processingError = error;
    assert(error instanceof Error, method + ': handled processing error changed');
  });
  const result = invoke(stream, method, error => {
    callbacks++;
    assert(error === processingError, method + ': handled callback error changed');
  });
  assert(result === (method === 'write' ? false : stream), method + ': handled return changed');
  assert(errors === 1, method + ': handled listener did not run once');
  assert(callbacks === (method === 'flush' ? 1 : 0), method + ': handled callback count changed');
  stream.close();
}

const stream = zlib.createGzip();
const output = [];
let flushed = false;
stream.on('data', chunk => output.push(chunk));
assert(stream.write('successful compression') === true, 'successful write return changed');
assert(stream.flush(error => {
  assert(error === null, 'successful flush callback error changed');
  flushed = true;
}) === stream, 'successful flush return changed');
assert(!flushed, 'successful flush callback must remain asynchronous');
assert(stream.end() === stream, 'successful end return changed');
assert(zlib.gunzipSync(Buffer.concat(output)).toString() === 'successful compression',
  'successful compression changed');
stream.close();

setTimeout(() => {
  assert(queued, 'queued work was dropped');
  assert(flushed, 'successful flush callback was dropped');
  console.log('PASS');
}, 0);
