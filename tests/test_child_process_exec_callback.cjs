const { exec, execFile } = require('child_process');
const { promisify } = require('util');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

// node's exec takes an optional trailing callback and returns the ChildProcess.
// ant additionally resolves a promise when no callback is given, so only the
// callback form returns a child.
const pending = new Set(['ok', 'fail', 'stderr', 'signal', 'execFile']);

// assertions thrown inside an exec callback would otherwise escape as an uncaught
// exception from a libuv callback, reporting differently than the promise phase below
function guard(fn) {
  return (...args) => {
    try { fn(...args); }
    catch (error) { console.log('FAIL:', error && error.message); process.exit(1); }
  };
}

// a callback that never fires would leave finish() unreachable and hang the test
const watchdog = setTimeout(() => {
  console.log(`FAIL: exec callbacks never completed: ${[...pending].join(', ')}`);
  process.exit(1);
}, 15000);

const child = exec('printf hello', guard((error, stdout, stderr) => {
  assert(error === null, `expected no error, got ${error}`);
  assert(stdout === 'hello', `expected stdout "hello", got ${JSON.stringify(stdout)}`);
  assert(stderr === '', `expected empty stderr, got ${JSON.stringify(stderr)}`);
  pending.delete('ok');
  finish();
}));

assert(typeof child.pid === 'number', 'exec with a callback should return a ChildProcess');
assert(!!child.stdout, 'returned child should expose stdout');
assert(typeof child.kill === 'function', 'returned child should expose kill');

const failingCommand = 'printf OUT; printf ERR >&2; exit 3';
exec(failingCommand, guard((error, stdout, stderr) => {
  assert(error instanceof Error, `expected an Error, got ${typeof error}`);
  assert(error.code === 3, `expected error.code 3, got ${error.code}`);
  assert(error.killed === false, `expected killed false, got ${error.killed}`);
  assert(error.signal === null, `expected signal null, got ${error.signal}`);
  assert(error.cmd === failingCommand, `expected error.cmd ${JSON.stringify(failingCommand)}, got ${JSON.stringify(error.cmd)}`);
  assert(error.message === `Command failed: ${failingCommand}\nERR`, `unexpected error.message ${JSON.stringify(error.message)}`);
  assert(stdout === 'OUT', `expected failed stdout "OUT", got ${JSON.stringify(stdout)}`);
  assert(stderr === 'ERR', `expected failed stderr "ERR", got ${JSON.stringify(stderr)}`);
  pending.delete('fail');
  finish();
}));

exec('printf oops >&2', guard((error, stdout, stderr) => {
  assert(error === null, `expected no error for stderr-only output, got ${error}`);
  assert(stdout === '', `expected empty stdout, got ${JSON.stringify(stdout)}`);
  assert(stderr === 'oops', `expected stderr "oops", got ${JSON.stringify(stderr)}`);
  pending.delete('stderr');
  finish();
}));

const signaledCommand = 'sleep 10';
const signaledChild = exec(signaledCommand, guard((error, stdout, stderr) => {
  assert(error instanceof Error, `expected signal Error, got ${typeof error}`);
  assert(error.code === null, `expected signal error.code null, got ${error.code}`);
  assert(error.killed === true, `expected signal killed true, got ${error.killed}`);
  assert(error.signal === 'SIGTERM', `expected SIGTERM, got ${error.signal}`);
  assert(error.cmd === signaledCommand, `expected signal error.cmd, got ${JSON.stringify(error.cmd)}`);
  assert(stdout === '', `expected empty signal stdout, got ${JSON.stringify(stdout)}`);
  assert(stderr === '', `expected empty signal stderr, got ${JSON.stringify(stderr)}`);
  pending.delete('signal');
  finish();
}));
// guard() here too: a bare throw from a timer callback escapes as an uncaught exception
// rather than the FAIL line the harness matches on
setTimeout(guard(() => {
  assert(signaledChild.kill('SIGTERM') === true, 'expected SIGTERM kill to succeed');
}), 20);

const execFileArgs = ['-c', 'printf FILEOUT; printf FILEERR >&2; exit 4'];
const execFileCommand = `sh ${execFileArgs.join(' ')}`;
execFile('sh', execFileArgs, guard((error, stdout, stderr) => {
  assert(error instanceof Error, `expected execFile Error, got ${typeof error}`);
  assert(error.code === 4, `expected execFile code 4, got ${error.code}`);
  assert(error.killed === false, `expected execFile killed false, got ${error.killed}`);
  assert(error.signal === null, `expected execFile signal null, got ${error.signal}`);
  assert(error.cmd === execFileCommand, `expected execFile cmd ${JSON.stringify(execFileCommand)}, got ${JSON.stringify(error.cmd)}`);
  assert(error.message === `Command failed: ${execFileCommand}\nFILEERR`, `unexpected execFile message ${JSON.stringify(error.message)}`);
  assert(stdout === 'FILEOUT', `expected execFile stdout, got ${JSON.stringify(stdout)}`);
  assert(stderr === 'FILEERR', `expected execFile stderr, got ${JSON.stringify(stderr)}`);
  pending.delete('execFile');
  finish();
}));

// The direct Promise form is an Ant extension and retains its established
// string rejection. util.promisify(exec) follows Node and rejects with Error.
function finish() {
  if (pending.size > 0) return;
  clearTimeout(watchdog);

  exec('printf done').then(result => {
    assert(result.stdout === 'done', `promise form stdout, got ${JSON.stringify(result.stdout)}`);
    return exec('printf bad >&2; exit 2').then(
      () => { throw new Error('expected rejection'); },
      error => {
        assert(typeof error === 'string', `direct Promise rejection should stay a string, got ${typeof error}`);
      }
    );
  }).then(() => {
    return exec('kill -TERM $$').then(
      () => { throw new Error('expected direct Promise signal rejection'); },
      error => {
        assert(typeof error === 'string', `direct signal rejection should be a string, got ${typeof error}`);
        assert(error.includes('SIGTERM'), `direct signal rejection should name SIGTERM, got ${JSON.stringify(error)}`);
      }
    );
  }).then(() => {
    return promisify(exec)('printf bad >&2; exit 2').then(
      () => { throw new Error('expected promisified rejection'); },
      error => {
        assert(error instanceof Error, 'promisify(exec) should reject with an Error');
        assert(error.code === 2, `expected code 2, got ${error.code}`);
        assert(error.stderr === 'bad', `expected stderr "bad", got ${JSON.stringify(error.stderr)}`);
      }
    );
  }).then(() => {
    return promisify(exec)('kill -TERM $$').then(
      () => { throw new Error('expected promisified signal rejection'); },
      error => {
        assert(error instanceof Error, 'promisified signal rejection should be an Error');
        assert(error.code === null, `expected promisified signal code null, got ${error.code}`);
        assert(error.signal === 'SIGTERM', `expected promisified SIGTERM, got ${error.signal}`);
        assert(error.killed === false, `expected self-signaled killed false, got ${error.killed}`);
      }
    );
  }).then(
    () => console.log('PASS'),
    error => { console.log('FAIL:', error && error.message); process.exit(1); }
  );
}
