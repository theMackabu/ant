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
  const helper = path.join(__dirname, 'fixtures', 'readline_keypress_sequential_child.cjs');
  // Drive two sequential prompts: type "xy" + Enter, then "z" + Enter. The
  // child renders no prompt (no `output`), so steps key off "READY:" markers.
  const script = `
import os, select, signal, sys, time

exec_path, helper = sys.argv[1], sys.argv[2]
pid, master = os.forkpty()

if pid == 0:
    os.execv(exec_path, [exec_path, helper])

# Distinct markers, so match against the full transcript without clearing it.
steps = [(b'READY:one', b'xy\\r'), (b'READY:two', b'z\\r')]
step = 0
last_send = 0.0

full = bytearray()
exit_code = None
deadline = time.time() + 10.0

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
    if step < len(steps):
        marker, data = steps[step]
        if marker in full and (now - last_send) > 0.3:
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
    full.extend(chunk)

sys.stdout.buffer.write(bytes(full))
sys.exit(exit_code)
`;

  if (process.platform === 'win32') {
    console.log('skipping readline sequential keypress tty test on win32');
    process.exit(0);
  }

  return spawnSync('python3', ['-c', script, resolveExecutable(process.execPath), helper], {
    encoding: 'utf8',
    timeout: 12000,
  });
}

const result = runInPty();

if (result.error && result.error.code === 'ENOENT') {
  console.log('skipping readline sequential keypress tty test because `python3` is unavailable');
  process.exit(0);
}

if (result.error) throw result.error;

const output = `${result.stdout || ''}${result.stderr || ''}`;

if (result.status !== 0) {
  fail(`child exited ${result.status}\n${output}`);
}

// First prompt receives keystrokes...
if (!output.includes('A="xy"')) {
  fail(`expected first prompt to capture typed value\n${output}`);
}

// ...and the second prompt's re-subscribed keypress listener still works.
if (!output.includes('B="z"')) {
  fail(`expected second prompt to keep receiving keypresses after re-subscribe\n${output}`);
}

console.log('sequential @clack-style keypress prompts keep receiving input');
