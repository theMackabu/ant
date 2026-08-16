const { spawn } = require('child_process');
const stream = require('stream');
const { promisify } = require('util');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const pipelineAsync = promisify(stream.pipeline);

// child stdio must be real stream instances, not ad-hoc event objects; execa and
// get-stream reach for pipe/pipeline rather than raw 'data' listeners
const shapes = spawn('echo', ['shape']);
assert(shapes.stdout instanceof stream.Readable, 'child.stdout should be a Readable');
assert(shapes.stderr instanceof stream.Readable, 'child.stderr should be a Readable');
assert(shapes.stdin instanceof stream.Writable, 'child.stdin should be a Writable');

for (const method of ['pipe', 'unpipe', 'read', 'pause', 'resume', 'setEncoding', 'removeListener', 'emit']) {
  assert(typeof shapes.stdout[method] === 'function', `child.stdout.${method} should be a function`);
}
assert(typeof shapes.stdin.pipe === 'function', 'child.stdin.pipe should be a function');
shapes.stdout.resume();
shapes.stderr.resume();

function collect(readable) {
  const sink = new stream.PassThrough();
  const chunks = [];
  sink.on('data', chunk => chunks.push(chunk));
  return pipelineAsync(readable, sink).then(() => Buffer.concat(chunks).toString());
}

async function main() {
  const piped = spawn('echo', ['piped']);
  assert(await collect(piped.stdout) === 'piped\n', 'pipeline over child.stdout should resolve with its output');

  const errs = spawn('sh', ['-c', 'echo oops >&2']);
  errs.stdout.resume();
  assert(await collect(errs.stderr) === 'oops\n', 'pipeline over child.stderr should resolve with its output');

  // stdin as a Writable: end(chunk) must write then close the pipe
  const cat = spawn('cat');
  cat.stderr.resume();
  const roundTripP = collect(cat.stdout);
  cat.stdin.end('through stdin');
  assert(await roundTripP === 'through stdin', 'writing to child.stdin should round trip through cat');

  // piping into child.stdin should also drive the write path
  const upper = spawn('tr', ['a-z', 'A-Z']);
  upper.stderr.resume();
  const upperP = collect(upper.stdout);
  stream.Readable.from(['shout']).pipe(upper.stdin);
  assert(await upperP === 'SHOUT', 'piping into child.stdin should feed the child');

  // _write must not complete until libuv has accepted the payload. A full pipe
  // should signal backpressure and resume through drain once the child reads.
  const slow = spawn('sh', ['-c', 'sleep 0.05; wc -c']);
  slow.stderr.resume();
  const slowOutputP = collect(slow.stdout);
  const payload = Buffer.alloc(64 * 1024, 120);
  let insideWrite = true;
  let writeCallbackWasAsync = false;
  const writeDoneP = new Promise((resolve, reject) => {
    const accepted = slow.stdin.write(payload, error => {
      if (error) return reject(error);
      writeCallbackWasAsync = !insideWrite;
      resolve();
    });
    assert(accepted === false, 'child.stdin.write should apply its highWaterMark');
  });
  insideWrite = false;
  slow.stdin.end();
  await writeDoneP;
  assert(writeCallbackWasAsync, 'child.stdin write callback should wait for uv_write');
  assert(
    Number((await slowOutputP).trim()) === payload.length,
    'backpressured child.stdin should preserve all bytes'
  );

  // The process exit callback can run before stdout reaches EOF. Keep the read
  // side alive long enough to drain output larger than the stream HWM.
  const largeCat = spawn('cat');
  largeCat.stderr.resume();
  const largeOutputP = collect(largeCat.stdout);
  const largePayload = Buffer.alloc(128 * 1024, 121);
  largeCat.stdin.end(largePayload);
  const largeOutput = await largeOutputP;
  assert(largeOutput.length === largePayload.length, 'large child output should not be truncated');
  assert(largeOutput === largePayload.toString(), 'large child output should preserve its bytes');

  // a paused readable buffers until something reads, rather than dropping chunks
  const buffered = spawn('echo', ['buffered']);
  buffered.stderr.resume();
  await new Promise(resolve => buffered.on('close', resolve));
  assert(
    await collect(buffered.stdout) === 'buffered\n',
    'output produced before a reader attaches should still be delivered'
  );

  console.log('PASS');
}

main().catch(error => {
  console.error(error && error.stack ? error.stack : error);
  process.exit(1);
});
