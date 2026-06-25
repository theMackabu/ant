const assert = require('node:assert');
const { spawn } = require('node:child_process');

function collect(stream) {
  return new Promise((resolve, reject) => {
    const chunks = [];

    function onReadable() {
      let chunk;
      while ((chunk = stream.read()) !== null) {
        chunks.push(chunk);
      }
    }

    function onEnd() {
      stream.off('readable', onReadable);
      stream.off('error', onError);
      stream.off('end', onEnd);
      resolve(chunks.map(String).join(''));
    }

    function onError(error) {
      reject(error);
    }

    stream.on('readable', onReadable);
    stream.once('error', onError);
    stream.once('end', onEnd);
    onReadable();
  });
}

async function run(command, args) {
  const child = spawn(command, args);
  assert.strictEqual(typeof child.on, 'function');
  assert.strictEqual(typeof child.off, 'function');
  assert.strictEqual(typeof child.addListener, 'function');
  assert.strictEqual(typeof child.removeListener, 'function');

  assert.strictEqual(typeof child.stdout.read, 'function');
  assert.strictEqual(typeof child.stdout.off, 'function');
  assert.strictEqual(typeof child.stdout.addListener, 'function');
  assert.strictEqual(child.stdout.readable, true);

  let didSpawn = false;
  child.on('spawn', () => {
    didSpawn = true;
  });

  const [stdout, stderr, exit] = await Promise.all([
    collect(child.stdout),
    collect(child.stderr),
    new Promise((resolve) => child.on('exit', (code, signal) => resolve({ code, signal }))),
  ]);

  assert.strictEqual(didSpawn, true);
  return { stdout, stderr, exit };
}

(async () => {
  const stdoutOnly = await run(process.execPath, ['-e', 'process.stdout.write("out")']);
  assert.strictEqual(stdoutOnly.stdout, 'out');
  assert.strictEqual(stdoutOnly.stderr, '');
  assert.deepStrictEqual(stdoutOnly.exit, { code: 0, signal: null });

  const stderrOnly = await run(process.execPath, ['-e', 'process.stderr.write("err"); process.exit(2)']);
  assert.strictEqual(stderrOnly.stdout, '');
  assert.strictEqual(stderrOnly.stderr, 'err');
  assert.deepStrictEqual(stderrOnly.exit, { code: 2, signal: null });

  const empty = await run(process.execPath, ['-e', '']);
  assert.strictEqual(empty.stdout, '');
  assert.strictEqual(empty.stderr, '');
  assert.deepStrictEqual(empty.exit, { code: 0, signal: null });

  console.log('child_process readable stream ok');
})();
