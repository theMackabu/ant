const { spawnSync } = require('child_process');

function fail(message) {
  throw new Error(message);
}

function runInPty() {
  const script = `
import os, re, select, signal, sys, time
PROMPT = re.compile(rb'\\xe2\\x9d\\xaf(?:\\x1b\\[[0-9;]*m)* ')

exec_path = sys.argv[1]
pid, master = os.forkpty()

if pid == 0:
    os.execv(exec_path, [exec_path])

buf = bytearray()
sent_import = False
sent_check = False
sent_exit = False
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
    if (not sent_import) and PROMPT.search(buf):
        os.write(master, b"import fs from 'node:fs/promises';\\r")
        sent_import = True
    elif sent_import and (not sent_check) and len(PROMPT.findall(buf)) >= 2:
        os.write(master, b"console.log('FS_READFILE', typeof fs.readFile);\\r")
        sent_check = True
    elif sent_check and (not sent_exit) and b'FS_READFILE function' in buf:
        os.write(master, b".exit\\r")
        sent_exit = True

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
    console.log('skipping REPL static import pty test on win32');
    process.exit(0);
  }

  return spawnSync('python3', ['-c', script, process.execPath], {
    encoding: 'utf8',
    timeout: 9000,
  });
}

const result = runInPty();

if (result.error && result.error.code === 'ENOENT') {
  console.log('skipping REPL static import pty test because `python3` is unavailable');
  process.exit(0);
}

if (result.error) throw result.error;

const output = `${result.stdout || ''}${result.stderr || ''}`;

if (result.status !== 0) {
  fail(`REPL exited ${result.status}\n${output}`);
}

if (output.includes('Cannot use import/export syntax outside a module')) {
  fail(`REPL rejected static import\n${output}`);
}

if (!output.includes('FS_READFILE function')) {
  fail(`expected imported fs binding to work\n${output}`);
}

console.log('REPL accepts static import');
