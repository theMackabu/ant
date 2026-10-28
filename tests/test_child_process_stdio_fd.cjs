'use strict';

const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const cp = require('node:child_process');

const watchdog = setTimeout(() => {
  console.error('child stdio fd test timed out');
  process.exit(1);
}, 60000);

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-stdio-fd-'));
const exe = process.execPath;
const input = path.join(dir, 'input.txt');
fs.writeFileSync(input, 'input-data');

const echo = `
  const fs = require('fs');
  const data = fs.readFileSync(0);
  process.stdout.write('OUT:' + data.toString());
  process.stderr.write('ERR:' + data.length);
`;
const write = `
  const fs = require('fs');
  fs.writeSync(1, 'to-stdout\\n');
  fs.writeSync(2, 'to-stderr\\n');
`;
const fd3 = `
  const fs = require('fs');
  fs.writeSync(3, 'to-fd3\\n');
  fs.writeSync(1, 'out\\n');
`;

const text = value => (value === null ? null : value.toString());
const read = file => fs.readFileSync(file, 'utf8');
const target = name => path.join(dir, name);

function withFd(file, flags, fn) {
  const fd = fs.openSync(file, flags);
  try {
    return fn(fd);
  } finally {
    fs.closeSync(fd);
  }
}

function spawnAsync(script, options) {
  return new Promise((resolve, reject) => {
    const child = cp.spawn(exe, ['-e', script], options);
    const out = [];
    const err = [];
    if (child.stdout) child.stdout.on('data', chunk => out.push(chunk));
    if (child.stderr) child.stderr.on('data', chunk => err.push(chunk));
    child.on('error', reject);
    child.on('close', code => resolve({
      code,
      child,
      stdout: Buffer.concat(out).toString(),
      stderr: Buffer.concat(err).toString(),
    }));
  });
}

(async () => {
  {
    const result = withFd(input, 'r', fd => cp.spawnSync(exe, ['-e', echo], { stdio: [fd, 'pipe', 'pipe'] }));
    assert.strictEqual(result.status, 0);
    assert.strictEqual(text(result.stdout), 'OUT:input-data');
    assert.strictEqual(text(result.stderr), 'ERR:10');
    assert.strictEqual(result.output[0], null);
  }

  {
    const result = withFd(input, 'r', fd => cp.spawnSync(exe, ['-e', echo], { stdio: [{ fd }, 'pipe', 'pipe'] }));
    assert.strictEqual(result.status, 0);
    assert.strictEqual(text(result.stdout), 'OUT:input-data');
  }

  {
    const bad = cp.spawnSync(exe, ['-e', echo], { stdio: [987654, 'pipe', 'pipe'] });
    assert.strictEqual(bad.error && bad.error.code, 'EBADF');
  }

  {
    const file = target('sync-stdout.txt');
    const result = withFd(file, 'w', fd => cp.spawnSync(exe, ['-e', write], { stdio: ['ignore', fd, 'pipe'] }));
    assert.strictEqual(result.status, 0);
    assert.strictEqual(result.stdout, null);
    assert.strictEqual(result.output[1], null);
    assert.strictEqual(text(result.stderr), 'to-stderr\n');
    assert.strictEqual(read(file), 'to-stdout\n');
  }

  {
    const file = target('sync-both.txt');
    const result = withFd(file, 'w', fd => cp.spawnSync(exe, ['-e', write], { stdio: ['ignore', fd, fd] }));
    assert.strictEqual(result.stdout, null);
    assert.strictEqual(result.stderr, null);
    assert.strictEqual(read(file), 'to-stdout\nto-stderr\n');
  }

  {
    const file = target('sync-stderr.txt');
    const result = withFd(file, 'w', fd => cp.spawnSync(exe, ['-e', write], { stdio: ['ignore', 'pipe', fd] }));
    assert.strictEqual(text(result.stdout), 'to-stdout\n');
    assert.strictEqual(result.stderr, null);
    assert.strictEqual(read(file), 'to-stderr\n');
  }

  {
    const file = target('sync-fd3.txt');
    const result = withFd(file, 'w', fd => cp.spawnSync(exe, ['-e', fd3], { stdio: ['ignore', 'pipe', 'pipe', fd] }));
    assert.strictEqual(result.status, 0, text(result.stderr));
    assert.strictEqual(text(result.stdout), 'out\n');
    assert.strictEqual(result.output.length, 4);
    assert.strictEqual(read(file), 'to-fd3\n');
  }

  {
    const file = target('sync-stream.txt');
    const stream = fs.createWriteStream(file);
    await new Promise(resolve => stream.on('open', resolve));
    const result = cp.spawnSync(exe, ['-e', write], { stdio: ['ignore', stream, 'ignore'] });
    stream.end();
    await new Promise(resolve => stream.on('close', resolve));
    assert.strictEqual(result.stdout, null);
    assert.strictEqual(read(file), 'to-stdout\n');
  }

  {
    assert.strictEqual(text(cp.spawnSync(exe, ['-e', echo], { input: 'abc' }).stdout), 'OUT:abc');
    assert.strictEqual(text(cp.spawnSync(exe, ['-e', echo], { input: Buffer.from('xyz') }).stdout), 'OUT:xyz');
    assert.strictEqual(text(cp.spawnSync(exe, ['-e', echo], { input: new Uint8Array([65, 66]) }).stdout), 'OUT:AB');
    assert.strictEqual(text(cp.spawnSync(exe, ['-e', echo], { input: 'q', stdio: 'pipe' }).stdout), 'OUT:q');
  }

  {
    const file = target('exec-sync.txt');
    const out = withFd(file, 'w', fd => cp.execSync(`"${exe}" -e "require('fs').writeSync(1, 'shell-out')"`, { stdio: ['ignore', fd, 'pipe'] }));
    assert.strictEqual(out, null);
    assert.strictEqual(read(file), 'shell-out');

    const file2 = target('exec-file-sync.txt');
    const out2 = withFd(file2, 'w', fd => cp.execFileSync(exe, ['-e', write], { stdio: ['ignore', fd, 'ignore'] }));
    assert.strictEqual(out2, null);
    assert.strictEqual(read(file2), 'to-stdout\n');

    assert.strictEqual(cp.execFileSync(exe, ['-e', echo], { input: 'via-exec-file', encoding: 'utf8', stdio: ['pipe', 'pipe', 'ignore'] }), 'OUT:via-exec-file');
  }

  {
    const result = await withFd(input, 'r', fd => spawnAsync(echo, { stdio: [fd, 'pipe', 'pipe'] }));
    assert.strictEqual(result.code, 0);
    assert.strictEqual(result.child.stdin, null);
    assert.strictEqual(result.stdout, 'OUT:input-data');
  }

  {
    const file = target('async-stdout.txt');
    const result = await withFd(file, 'w', fd => spawnAsync(write, { stdio: ['ignore', fd, 'pipe'] }));
    assert.strictEqual(result.child.stdout, null);
    assert.strictEqual(result.stderr, 'to-stderr\n');
    assert.strictEqual(read(file), 'to-stdout\n');
    assert.strictEqual(result.child.stdio.length, 3);
  }

  {
    const file = target('async-fd3.txt');
    const result = await withFd(file, 'w', fd => spawnAsync(fd3, { stdio: ['ignore', 'pipe', 'pipe', fd] }));
    assert.strictEqual(result.code, 0, result.stderr);
    assert.strictEqual(result.stdout, 'out\n');
    assert.strictEqual(result.child.stdio.length, 4);
    assert.strictEqual(result.child.stdio[3], null);
    assert.strictEqual(read(file), 'to-fd3\n');
  }

  {
    const file = target('async-stream.txt');
    const stream = fs.createWriteStream(file);
    await new Promise(resolve => stream.on('open', resolve));
    const result = await spawnAsync(write, { stdio: ['ignore', stream, 'pipe'] });
    stream.end();
    await new Promise(resolve => stream.on('close', resolve));
    assert.strictEqual(result.stderr, 'to-stderr\n');
    assert.strictEqual(read(file), 'to-stdout\n');
  }

  {
    const result = await withFd(input, 'r', fd => spawnAsync(echo, { stdio: [fd, 'overlapped', 'overlapped'] }));
    assert.strictEqual(result.stdout, 'OUT:input-data');
    assert.strictEqual(result.stderr, 'ERR:10');
  }

  assert.throws(() => cp.spawn(exe, ['-e', ''], { stdio: [{}] }), {
    code: 'ERR_INVALID_ARG_VALUE',
    message: "The argument 'stdio' is invalid. Received {}",
  });
  assert.throws(() => cp.spawnSync(exe, ['-e', ''], { stdio: 5 }), { code: 'ERR_INVALID_ARG_VALUE' });

  fs.rmSync(dir, { recursive: true, force: true });
  clearTimeout(watchdog);
  console.log('child process stdio fd ok');
})().catch(err => {
  console.error(err);
  process.exit(1);
});
