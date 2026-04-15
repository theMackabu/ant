const { spawnSync } = require('child_process');
const path = require('path');

function fail(message) {
  throw new Error(message);
}

function runInPty() {
  const helper = path.join(__dirname, 'fixtures', 'readline_question_prompt_child.cjs');
  const script = `
import os, select, signal, sys, time

exec_path, helper = sys.argv[1], sys.argv[2]
pid, master = os.forkpty()

if pid == 0:
    os.execv(exec_path, [exec_path, helper])

buf = bytearray()
sent = False
exit_code = None
deadline = time.time() + 7.0

while time.time() < deadline:
    done, status = os.waitpid(pid, os.WNOHANG)
    if done == pid:
        exit_code = os.waitstatus_to_exitcode(status)
        break

    r, _, _ = select.select([master], [], [], 0.1)
    if master not in r:
        continue

    try:
        chunk = os.read(master, 4096)
    except OSError:
        break

    if not chunk:
        break

    buf.extend(chunk)
    if (not sent) and b'Q: ' in buf:
        os.write(master, b'ab\\x7fc\\r')
        sent = True

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
    console.log('skipping readline question prompt tty test on win32');
    process.exit(0);
  }

  return spawnSync('python3', ['-c', script, process.execPath, helper], {
    encoding: 'utf8',
    timeout: 9000,
  });
}

const result = runInPty();

if (result.error && result.error.code === 'ENOENT') {
  console.log('skipping readline question prompt tty test because `python3` is unavailable');
  process.exit(0);
}

if (result.error) throw result.error;

const output = `${result.stdout || ''}${result.stderr || ''}`;

if (result.status !== 0) {
  fail(`child exited ${result.status}\n${output}`);
}

if (!output.includes('Q: ')) {
  fail(`expected question prompt in output\n${output}`);
}

if (!output.includes('ANSWER "ac"')) {
  fail(`expected edited answer to round-trip\n${output}`);
}

if (output.includes('> ')) {
  fail(`default interface prompt leaked into question redraw\n${output}`);
}

console.log('readline question keeps the active prompt during redraw');
