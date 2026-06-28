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
  const helper = path.join(__dirname, 'fixtures', 'readline_keypress_arrow_child.cjs');
  // Send Up arrow (ESC [ A) as one write, then "q" to finish.
  const script = `
import os, select, signal, sys, time

exec_path, helper = sys.argv[1], sys.argv[2]
pid, master = os.forkpty()

if pid == 0:
    os.execv(exec_path, [exec_path, helper])

steps = [(b'READY', b'\\x1b[A'), (b'READY', b'q')]
step = 0
last_send = 0.0

full = bytearray()
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
        full.extend(chunk)

    now = time.time()
    if step < len(steps) and (b'READY' in full) and (now - last_send) > 0.4:
        os.write(master, steps[step][1])
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
    full.extend(chunk)

sys.stdout.buffer.write(bytes(full))
sys.exit(exit_code)
`;

  if (process.platform === 'win32') {
    console.log('skipping readline arrow keypress tty test on win32');
    process.exit(0);
  }

  return spawnSync('python3', ['-c', script, resolveExecutable(process.execPath), helper], {
    encoding: 'utf8',
    timeout: 10000,
  });
}

const result = runInPty();

if (result.error && result.error.code === 'ENOENT') {
  console.log('skipping readline arrow keypress tty test because `python3` is unavailable');
  process.exit(0);
}

if (result.error) throw result.error;

const output = `${result.stdout || ''}${result.stderr || ''}`;

if (result.status !== 0) {
  fail(`child exited ${result.status}\n${output}`);
}

const match = output.match(/KEYS=(\[.*\])/);
if (!match) {
  fail(`expected KEYS output\n${output}`);
}

const keys = JSON.parse(match[1]);

// The Up arrow must decode to a single "up" keypress...
if (!keys.includes('up')) {
  fail(`expected an "up" keypress from ESC [ A\n${output}`);
}

// ...and must NOT be misdecoded as a lone "escape" (which @clack maps to cancel).
if (keys.includes('escape')) {
  fail(`arrow sequence wrongly produced a spurious "escape" keypress\n${output}`);
}

console.log('arrow-key escape sequences decode to a single named keypress');
