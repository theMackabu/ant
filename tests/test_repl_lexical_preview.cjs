const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

if (process.platform === 'win32') {
  console.log('skipping REPL lexical preview pty test on win32');
  process.exit(0);
}

const script = String.raw`
import os, select, signal, sys, time

pid, master = os.forkpty()
if pid == 0:
    os.execv(sys.argv[1], [sys.argv[1], '--no-color'])

pending = bytearray()
transcript = bytearray()
prompt = b'\xe2\x9d\xaf'

def expect(needle):
    deadline = time.time() + 3
    while needle not in pending:
        if time.time() >= deadline:
            raise AssertionError('missing ' + repr(needle) + '\n' + repr(bytes(transcript)))
        ready, _, _ = select.select([master], [], [], 0.05)
        if ready:
            chunk = os.read(master, 4096)
            if not chunk:
                raise AssertionError('REPL exited early')
            pending.extend(chunk)
            transcript.extend(chunk)
    end = pending.index(needle) + len(needle)
    del pending[:end]

try:
    expect(prompt)
    os.write(master, b'const previewBinding = { previewMember: 42 };\r')
    expect(b'\r\nundefined\r\n')
    expect(prompt)
    os.write(master, b'previewBinding.pre')
    expect(b'viewMember')
    os.write(master, b'\t\r')
    expect(b'\r\n42\r\n')
    expect(prompt)
    os.write(master, b'.exit\r')
    _, status = os.waitpid(pid, 0)
    pid = None
    assert os.waitstatus_to_exitcode(status) == 0
finally:
    if pid is not None:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
    os.close(master)
`;

const result = spawnSync('python3', ['-c', script, process.execPath], {
  encoding: 'utf8', timeout: 12000,
});
if (result.error?.code === 'ENOENT') {
  console.log('skipping REPL lexical preview pty test because python3 is unavailable');
  process.exit(0);
}
if (result.error) throw result.error;
assert.strictEqual(result.status, 0, `${result.stdout || ''}${result.stderr || ''}`);
console.log('REPL completes members of lexical bindings');
