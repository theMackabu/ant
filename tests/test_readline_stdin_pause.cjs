const { spawnSync } = require('child_process');
const fs = require('fs');
const path = require('path');

function fail(message) {
  throw new Error(message);
}

function resolveExecutable(execPath) {
  if (path.isAbsolute(execPath)) return execPath;

  const localPath = path.resolve(execPath);
  if (fs.existsSync(localPath)) return localPath;

  for (const dir of (process.env.PATH || '').split(path.delimiter)) {
    if (!dir) continue;
    const candidate = path.join(dir, execPath);
    if (fs.existsSync(candidate)) return candidate;
  }

  return execPath;
}

function runInPty() {
  const helper = path.join(__dirname, 'fixtures', 'readline_stdin_pause_child.cjs');
  // Send "a" (expect one DATA), then "b" after pause() (expect it suppressed
  // even though the readline Interface keeps the shared reader alive).
  const script = `
import os, select, signal, sys, time

exec_path, helper = sys.argv[1], sys.argv[2]
pid, master = os.forkpty()

if pid == 0:
    os.execv(exec_path, [exec_path, helper])

steps = [(b'READY', b'abc\\r')]
step = 0
last_send = 0.0

buf = bytearray()
exit_code = None
deadline = time.time() + 8.0

while time.time() < deadline:
    done, status = os.waitpid(pid, os.WNOHANG)
    if done == pid:
        exit_code = os.waitstatus_to_exitcode(status)
        break

    r, _, _ = select.select([master], [], [], 0.1)
    if master in r:
        try:
            chunk = os.read(master, 4096)
        except OSError:
            break
        if not chunk:
            break
        buf.extend(chunk)

    now = time.time()
    if step < len(steps):
        marker, data = steps[step]
        if marker in buf and (now - last_send) > 0.3:
            os.write(master, data)
            last_send = now
            step += 1

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

  if (process.platform === 'win32') {
    console.log('skipping stdin pause tty test on win32');
    process.exit(0);
  }

  return spawnSync('python3', ['-c', script, resolveExecutable(process.execPath), helper], {
    encoding: 'utf8',
    timeout: 10000,
  });
}

const result = runInPty();

if (result.error && result.error.code === 'ENOENT') {
  console.log('skipping stdin pause tty test because `python3` is unavailable');
  process.exit(0);
}

if (result.error) throw result.error;

const output = `${result.stdout || ''}${result.stderr || ''}`;

if (result.status !== 0) {
  fail(`child exited ${result.status}\n${output}`);
}

// The child reached pause() and ran to completion.
if (!output.includes('READY') || !output.includes('DONE')) {
  fail(`child did not run to completion\n${output}`);
}

// Input typed after pause() must not surface as a data event...
if (output.includes('DATA:')) {
  fail(`data event fired after process.stdin.pause()\n${output}`);
}

// ...nor reach the readline Interface as a line (or be echoed).
if (output.includes('LINE:')) {
  fail(`readline line event fired after process.stdin.pause()\n${output}`);
}
if (/\babc\b/.test(output)) {
  fail(`input was echoed after process.stdin.pause()\n${output}`);
}

console.log('process.stdin.pause() stops data, keypress, and readline delivery');
