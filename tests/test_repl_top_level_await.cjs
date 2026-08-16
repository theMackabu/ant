const assert = require('assert');
const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

if (process.platform === 'win32') {
  console.log('skipping REPL top-level await pty test on win32');
  process.exit(0);
}

const script = `
import os, select, signal, sys, time

exec_path = sys.argv[1]
pid, master = os.forkpty()

if pid == 0:
    os.execv(exec_path, [exec_path, '--no-color'])

tick = bytes([96])
commands = [
    b"await Promise.resolve(42)\\r",
    b"const replAwaitValue = await Promise.resolve(7)\\r",
    b"replAwaitValue\\r",
    b"Promise.resolve(9)\\r",
    b"await new Promise(resolve => setTimeout(() => resolve(23), 20))\\r",
    b"const replKeepAlive = setInterval(() => {}, 1000)\\r",
    b"await new Promise(resolve => setTimeout(() => resolve(31), 20))\\r",
    b"clearInterval(replKeepAlive)\\r",
    b"import {$} from 'ant:shell'\\r",
    b"await $" + tick + b"printf repl-shell-ok" + tick + b".text()\\r",
    b"await Promise.reject(new Error('repl-await-boom'))\\r",
    b"1\\r",
    b".copy await Promise.resolve('copy-await-ok')\\r",
    b"await new Promise(() => {})\\r",
    b"2\\r",
    b".exit\\r",
]

prompt = b'\\xe2\\x9d\\xaf'
buf = bytearray()
sent = 0
interrupt_at = None
interrupt_sent = False
exit_code = None
deadline = time.time() + 12.0

while time.time() < deadline:
    done, status = os.waitpid(pid, os.WNOHANG)
    if done == pid:
        exit_code = os.waitstatus_to_exitcode(status)
        break

    if interrupt_at is not None and not interrupt_sent and time.time() >= interrupt_at:
        os.kill(pid, signal.SIGINT)
        interrupt_sent = True

    r, _, _ = select.select([master], [], [], 0.05)
    if master in r:
        try:
            chunk = os.read(master, 4096)
        except OSError:
            break
        if not chunk:
            break
        buf.extend(chunk)

    prompt_count = bytes(buf).count(prompt)
    while prompt_count > sent and sent < len(commands):
        os.write(master, commands[sent])
        if sent == 13:
            interrupt_at = time.time() + 0.1
        sent += 1

if exit_code is None:
    os.kill(pid, signal.SIGKILL)
    _, status = os.waitpid(pid, 0)
    exit_code = os.waitstatus_to_exitcode(status)

while True:
    r, _, _ = select.select([master], [], [], 0.05)
    if master not in r:
        break
    try:
        chunk = os.read(master, 4096)
    except OSError:
        break
    if not chunk:
        break
    buf.extend(chunk)

sys.stdout.buffer.write(bytes(buf))
sys.exit(exit_code)
`;

const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-repl-tla-'));
const copyCapture = path.join(tempDir, 'copy.txt');
const fakePbcopy = path.join(tempDir, 'pbcopy');
fs.writeFileSync(fakePbcopy, '#!/bin/sh\ncat > "$REPL_COPY_CAPTURE"\n', { mode: 0o755 });
fs.chmodSync(fakePbcopy, 0o755);

const result = spawnSync('python3', ['-c', script, process.execPath], {
  encoding: 'utf8',
  timeout: 14000,
  env: {
    ...process.env,
    PATH: `${tempDir}:${process.env.PATH || ''}`,
    REPL_COPY_CAPTURE: copyCapture,
  },
});
const copied = fs.existsSync(copyCapture) ? fs.readFileSync(copyCapture, 'utf8') : null;
fs.rmSync(tempDir, { recursive: true, force: true });

if (result.error && result.error.code === 'ENOENT') {
  console.log('skipping REPL top-level await pty test because `python3` is unavailable');
  process.exit(0);
}
if (result.error) throw result.error;

const output = `${result.stdout || ''}${result.stderr || ''}`;
assert.strictEqual(result.status, 0, output);

assert.match(output, /\r?\n42\r?\n/, output);
assert.match(output, /\r?\nundefined\r?\n/, output);
assert.match(output, /\r?\n7\r?\n/, output);
assert.match(output, /\r?\nPromise \{\r?\n  9,/, output);
assert.match(output, /\r?\n23\r?\n/, output);
assert.match(output, /\r?\n31\r?\n/, output);
assert.match(output, /\r?\n'repl-shell-ok'\r?\n/, output);
assert.match(output, /Error: repl-await-boom/, output);
assert.strictEqual(
  (output.match(/Error: repl-await-boom/g) || []).length,
  1,
  output
);
assert.doesNotMatch(output, /UnhandledPromiseRejection|Unhandled promise rejection/, output);
assert.strictEqual(
  copied,
  'copy-await-ok',
  `clipboard capture: ${JSON.stringify(copied)}\n${output.slice(-2000)}`
);
assert.match(output, /\^C/, output);
assert.match(output, /\r?\n2\r?\n/, output);

console.log('REPL awaits only top-level-await entry completions');
