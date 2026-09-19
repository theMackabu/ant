// Expected filesystem failures are callback arguments or promise rejections,
// never a second uncaught exception left behind by native error construction.
const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-fs-handled-'));
const file = path.join(dir, 'file');
fs.writeFileSync(file, 'data');
let index = 0;

function run(source) {
  const script = path.join(dir, `case-${index++}.cjs`);
  fs.writeFileSync(script, `
    const assert = require('node:assert');
    const fs = require('node:fs');
    const file = ${JSON.stringify(file)};
    function handled(error, code) {
      assert(error instanceof Error, 'expected an Error object');
      if (code) assert.strictEqual(error.code, code);
      console.log('HANDLED');
      setTimeout(() => console.log('CONTINUED'), 10);
    }
    ${source}
  `);
  return spawnSync(process.execPath, [script], { encoding: 'utf8', timeout: 10000 });
}

function expectHandled(name, source) {
  const result = run(source);
  assert.strictEqual(result.status, 0, `${name}: ${result.stderr || result.error}`);
  assert.strictEqual(result.stderr, '', `${name}: unexpected stderr`);
  const lines = result.stdout.trim().split('\n');
  assert.strictEqual(lines.filter(line => line === 'HANDLED').length, 1, `${name}: ${result.stdout}`);
  assert(lines.includes('CONTINUED'), `${name}: later callback did not run: ${result.stdout}`);
}

function expectFatal(name, source, checkNoContinuation = true) {
  const result = run(source);
  assert.strictEqual(result.status, 1, `${name}: exit ${result.status}: ${result.stderr}`);
  assert(result.stderr.includes('callback-threw'), `${name}: callback exception was lost: ${result.stderr}`);
  if (checkNoContinuation) assert(!result.stdout.includes('CONTINUED'), `${name}: ran after uncaught callback exception`);
}

const closedFd = `
  const fd = fs.openSync(file, 'r');
  fs.closeSync(fd);
`;

try {
  expectHandled('fsync callback', closedFd + `fs.fsync(fd, error => handled(error, 'EBADF'));`);
  expectHandled('read callback', closedFd + `fs.read(fd, Buffer.alloc(1), 0, 1, 0, error => handled(error, 'EBADF'));`);
  expectHandled('readFile callback control', `fs.readFile(file + '-missing', error => handled(error, 'ENOENT'));`);

  // Attach .catch() without awaiting the rejected operation: awaiting it inside
  // try/catch can accidentally consume the stale native exception as well.
  const filehandleCalls = {
    stat: 'handle.stat()',
    sync: 'handle.sync()',
    read: 'handle.read(Buffer.alloc(1))',
    write: "handle.write('x')",
    writeFile: "handle.writeFile('x')",
  };
  for (const [name, call] of Object.entries(filehandleCalls)) {
    expectHandled(`FileHandle.${name}`, `
      fs.promises.open(file).then(async handle => {
        await handle.close();
        const promise = ${call};
        assert(promise instanceof Promise);
        promise.catch(error => handled(error));
      });
    `);
  }
  expectHandled('FileHandle.close', `
    fs.promises.open(file).then(handle => {
      fs.closeSync(handle.fd);
      handle.close().catch(error => handled(error, 'EBADF'));
    });
  `);

  const operations = {
    rm: "'rm', [file + '-missing']",
    mkdir: "'mkdir', [file + '/child', { recursive: true }]",
    appendFile: "'appendFile', [file + '/child', 'x']",
    copyFile: "'copyFile', [file + '-missing', file + '-copy']",
    cp: "'cp', [file + '-missing', file + '-copy']",
  };
  for (const [name, operation] of Object.entries(operations)) {
    for (const useCallback of [false, true]) {
      expectHandled(`${name} ${useCallback ? 'callback' : 'promise'}`, `
        const [method, args] = [${operation}];
        ${useCallback
          ? 'fs[method](...args, error => handled(error));'
          : 'fs.promises[method](...args).catch(error => handled(error));'}
      `);
    }
  }

  expectHandled('ReadStream missing file', `
    const stream = fs.createReadStream(file + '-missing');
    stream.on('error', error => handled(error, 'ENOENT'));
    stream.resume();
  `);
  expectHandled('WriteStream invalid parent', `
    const stream = fs.createWriteStream(file + '/child');
    stream.on('error', error => handled(error, 'ENOTDIR'));
    stream.end('x');
  `);

  // Consuming the native I/O failure must happen before entering the callback,
  // so a new exception thrown by user code still reaches uncaught reporting.
  expectFatal('fsync throwing callback', closedFd + `
    fs.fsync(fd, error => {
      assert(error instanceof Error);
      setTimeout(() => console.log('CONTINUED'), 10);
      throw new Error('callback-threw');
    });
  `);
  expectFatal('read throwing callback', closedFd + `
    fs.read(fd, Buffer.alloc(1), 0, 1, 0, error => {
      assert(error instanceof Error);
      setTimeout(() => console.log('CONTINUED'), 10);
      throw new Error('callback-threw');
    });
  `);
  expectFatal('ReadStream throwing error listener', `
    const stream = fs.createReadStream(file + '-missing');
    stream.on('error', error => {
      assert(error instanceof Error);
      setTimeout(() => console.log('CONTINUED'), 10);
      throw new Error('callback-threw');
    });
    stream.resume();
  `); // Real microtasks report escaped listener throws before later timers.
} finally {
  fs.rmSync(dir, { recursive: true, force: true });
}

console.log('PASS');
