const { spawnSync } = require('child_process');

function fail(message) {
  throw new Error(message);
}

if (process.platform === 'win32') {
  console.log('skipping mini amp tty restore test on win32');
  process.exit(0);
}

const script = `
import os, pty, select, signal, sys, termios, time

exec_path = sys.argv[1]
script_path = sys.argv[2]

pid, master = pty.fork()
if pid == 0:
    os.execv(exec_path, [exec_path, script_path])

sent = False
exit_code = None
deadline = time.time() + 8.0

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

    if not sent:
        os.write(master, b'q')
        sent = True

if exit_code is None:
    os.kill(pid, signal.SIGKILL)
    _, status = os.waitpid(pid, 0)
    exit_code = os.waitstatus_to_exitcode(status)

attrs = termios.tcgetattr(master)
lflag = attrs[3]

print(f"exit={exit_code}")
print(f"isig={1 if (lflag & termios.ISIG) else 0}")
print(f"icanon={1 if (lflag & termios.ICANON) else 0}")
print(f"echo={1 if (lflag & termios.ECHO) else 0}")
`;

const result = spawnSync('python3', ['-c', script, process.execPath, 'tests/amp/mini_amp_tui_repro.js'], {
  encoding: 'utf8',
  timeout: 10000,
});

if (result.error && result.error.code === 'ENOENT') {
  console.log('skipping mini amp tty restore test because `python3` is unavailable');
  process.exit(0);
}

if (result.error) throw result.error;

const output = `${result.stdout || ''}${result.stderr || ''}`;

if (!output.includes('exit=0')) {
  fail(`expected repro process to exit cleanly\n${output}`);
}

if (!output.includes('isig=1')) {
  fail(`expected ISIG restored after repro exit\n${output}`);
}

if (!output.includes('icanon=1')) {
  fail(`expected ICANON restored after repro exit\n${output}`);
}

if (!output.includes('echo=1')) {
  fail(`expected ECHO restored after repro exit\n${output}`);
}

console.log('mini amp tui repro exits and restores tty flags');
