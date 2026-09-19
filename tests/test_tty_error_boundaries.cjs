const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-tty-errors-'));
const source = `
  const assert = require('node:assert');
  const proto = require('node:tty').WriteStream.prototype;
  const stream = { fd: 2147483647 };
  const operations = [
    ['write', ['x']],
    ['clearLine', [0]],
    ['clearScreenDown', []],
    ['cursorTo', [0]],
    ['moveCursor', [1, 0]],
  ];
  let calls = 0;
  for (const [method, args] of operations) {
    const result = proto[method].call(stream, ...args, error => {
      assert.ok(error instanceof Error, method + ': expected an Error value');
      assert.match(error.message, /tty stream .* failed for fd 2147483647/);
      calls++;
    });
    assert.strictEqual(result, method === 'write' ? false : stream);
  }
  assert.strictEqual(calls, operations.length);

  assert.throws(() => proto.clearLine.call(stream, 0), /tty stream clearLine failed/);
  for (const reason of [undefined, null, new Error('callback throw')]) {
    let caught = false;
    try {
      proto.clearLine.call(stream, 0, () => { throw reason; });
    } catch (error) {
      caught = true;
      assert.strictEqual(error, reason);
    }
    assert.ok(caught, 'callback exception must propagate');
  }
  queueMicrotask(() => console.log('PASS'));
`;

try {
  for (const observe of [false, true]) {
    const file = path.join(root, 'case.cjs');
    fs.writeFileSync(file,
      (observe ? "process.on('uncaughtException', () => console.log('UNCAUGHT'));\n" : '') +
      "process.on('unhandledRejection', () => console.log('UNHANDLED'));\n" + source);
    const result = spawnSync(process.execPath, ['--no-color', file], {
      encoding: 'utf8', timeout: 5000,
    });
    assert.strictEqual(result.status, 0, result.stderr);
    assert.strictEqual(result.stdout.trim(), 'PASS');
    assert.strictEqual(result.stderr, '');
  }
} finally {
  fs.rmSync(root, { recursive: true, force: true });
}
console.log('TTY error boundaries ok');
