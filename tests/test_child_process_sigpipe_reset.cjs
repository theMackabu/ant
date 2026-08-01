// src/main.c sets SIGPIPE to SIG_IGN, and an ignored disposition survives exec. Every
// path that spawns a child must reset it to SIG_DFL, or the child inherits ours and sees
// EPIPE from write() instead of dying on a closed pipe.
//
// The witness is bash's `trap -p SIGPIPE`, which prints an entry only for a non-default
// disposition. Empty output means SIG_DFL, which is what we want in a child. `/bin/sh` on
// macOS is bash 3.2 in sh mode and does not report it, so this invokes bash explicitly.
//
// `yes | head` is NOT a valid witness here: yes exits on write error regardless of the
// SIGPIPE disposition, so that pipeline terminates either way.
const { spawnSync, spawn, exec } = require('child_process');

// bash is the required witness (see header comment); fail loudly if it is absent
// rather than letting empty output read as success on minimal images
if (spawnSync('bash', ['-c', 'true']).status !== 0) {
  console.log('SKIP: bash not available; trap-based SIGPIPE witness needs bash');
  process.exit(0);
}

let failures = 0;
function check(label, actual) {
  const ok = actual === '';
  if (!ok) failures++;
  console.log(`${ok ? 'ok  ' : 'FAIL'}  ${label} ${ok ? '' : `-> ${JSON.stringify(actual)}`}`);
}

const TRAP = ['-c', 'trap -p SIGPIPE'];

// spawnSync uses the raw fork() + execvp() path in builtin_spawnSync
check('spawnSync', spawnSync('bash', TRAP, { encoding: 'utf8' }).stdout);

function viaSpawn() {
  return new Promise((resolve) => {
    const child = spawn('bash', TRAP);
    let out = '';
    child.stdout.on('data', (d) => { out += d; });
    child.on('close', () => resolve(out));
  });
}

function viaExec() {
  return new Promise((resolve) => {
    exec('bash -c "trap -p SIGPIPE"', (err, stdout) => resolve(err ? `exec error: ${err.message}` : stdout));
  });
}

(async () => {
  // both go through uv_spawn, which resets signal dispositions in the child for us
  check('spawn (uv_spawn)', await viaSpawn());
  check('exec (uv_spawn)', await viaExec());

  // execSync used to be built on popen(), which forks internally with no hook for the
  // child's signal dispositions. It now shares spawnSync's fork/exec path.
  const { execSync } = require('child_process');
  check('execSync', execSync('bash -c "trap -p SIGPIPE"', { encoding: 'utf8' }));

  // a missing binary is reported asynchronously through 'error' then 'close', not by
  // throwing out of spawn()
  const enoent = await new Promise((resolve) => {
    const seen = { error: null, close: null, threw: null };
    try {
      const child = spawn('definitely-not-a-real-binary-xyz');
      child.on('error', (e) => { seen.error = e; });
      child.on('close', (code, sig) => { seen.close = `${code}/${sig}`; });
      setTimeout(() => resolve(seen), 400);
    } catch (e) {
      seen.threw = e.message;
      resolve(seen);
    }
  });

  const enoentChecks = [
    ['spawn ENOENT does not throw', enoent.threw, null],
    ['spawn ENOENT emits error', enoent.error instanceof Error, true],
    ['spawn ENOENT error.code', enoent.error && enoent.error.code, 'ENOENT'],
    ['spawn ENOENT error.errno', enoent.error && enoent.error.errno, -2],
    ['spawn ENOENT error.syscall', enoent.error && enoent.error.syscall, 'spawn definitely-not-a-real-binary-xyz'],
    ['spawn ENOENT emits close', enoent.close, '-2/null'],
  ];
  for (const [label, actual, expected] of enoentChecks) {
    const ok = actual === expected;
    if (!ok) failures++;
    console.log(`${ok ? 'ok  ' : 'FAIL'}  ${label}${ok ? '' : ` -> ${JSON.stringify(actual)} (want ${JSON.stringify(expected)})`}`);
  }

  if (failures) { console.log(`FAIL: ${failures} check(s) failed`); process.exit(1); }
  console.log('PASS');
})();
