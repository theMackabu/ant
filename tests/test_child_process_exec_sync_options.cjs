// execSync was built on popen(), which cannot express cwd, env, input, stdio or timeout,
// and which stripped the trailing newline from the output. It now shares spawnSync's
// fork/exec path. Every expectation here was checked against node first.
const { execSync, execFileSync, spawnSync } = require('child_process');

// the timeout/maxBuffer/killSignal paths exercised below exist only in the POSIX
// spawn_sync_impl, and the shell commands are POSIX-specific
if (process.platform === 'win32') {
  console.log('SKIP: POSIX-only execSync option coverage');
  process.exit(0);
}

let failures = 0;
function eq(label, actual, expected) {
  const ok = actual === expected;
  if (!ok) failures++;
  console.log(`${ok ? 'ok  ' : 'FAIL'}  ${label}${ok ? '' : `\n      expected ${JSON.stringify(expected)}\n      actual   ${JSON.stringify(actual)}`}`);
}

// the trailing newline is part of the output; popen()-era execSync ate it
eq('keeps trailing newline', execSync('printf "a\\nb\\n"', { encoding: 'utf8' }), 'a\nb\n');
eq('keeps multiple newlines', execSync('printf "x\\n\\n"', { encoding: 'utf8' }), 'x\n\n');
eq('empty output stays empty', execSync('true', { encoding: 'utf8' }), '');

// cwd, env and input are honoured rather than silently dropped
eq('honours cwd', execSync('pwd', { cwd: '/', encoding: 'utf8' }).trim(), '/');
eq(
  'honours env',
  execSync('echo $ANT_OPT_TEST', {
    env: { ANT_OPT_TEST: 'set-by-test', PATH: process.env.PATH },
    encoding: 'utf8',
  }).trim(),
  'set-by-test'
);
eq('honours input', execSync('cat', { input: 'fed-on-stdin', encoding: 'utf8' }), 'fed-on-stdin');

// a non-zero exit throws an Error carrying node's fields
{
  let err = null;
  try {
    execSync('printf OUT; printf ERR >&2; exit 7', { encoding: 'utf8', stdio: 'pipe' });
  } catch (e) { err = e; }
  eq('non-zero throws', err instanceof Error, true);
  eq('error.status', err && err.status, 7);
  eq('error.signal', err && err.signal, null);
  eq('error.stdout', err && err.stdout, 'OUT');
  eq('error.stderr', err && err.stderr, 'ERR');
  eq('error.message', err && err.message, 'Command failed: printf OUT; printf ERR >&2; exit 7\nERR');
}

// timeout kills the child and reports it the way node does
{
  const started = Date.now();
  let err = null;
  try {
    execSync('sleep 10', { timeout: 250, encoding: 'utf8', stdio: 'pipe' });
  } catch (e) { err = e; }
  const elapsed = Date.now() - started;
  eq('timeout throws', err instanceof Error, true);
  eq('timeout status is null', err && err.status, null);
  eq('timeout signal is SIGTERM', err && err.signal, 'SIGTERM');
  eq('timeout message', err && err.message, 'spawnSync /bin/sh ETIMEDOUT');
  const quick = elapsed < 5000;
  if (!quick) failures++;
  console.log(`${quick ? 'ok  ' : 'FAIL'}  timeout actually fired (${elapsed}ms)`);
}

// killSignal picks the signal used on timeout
{
  let err = null;
  try {
    execSync('sleep 10', { timeout: 250, killSignal: 'SIGKILL', encoding: 'utf8', stdio: 'pipe' });
  } catch (e) { err = e; }
  eq('killSignal honoured', err && err.signal, 'SIGKILL');
}

// execFileSync shares the same failure shape, and names the whole command line
{
  let err = null;
  try {
    execFileSync('sh', ['-c', 'printf E >&2; exit 5'], { encoding: 'utf8', stdio: 'pipe' });
  } catch (e) { err = e; }
  eq('execFileSync throws', err instanceof Error, true);
  eq('execFileSync status', err && err.status, 5);
  eq('execFileSync stderr', err && err.stderr, 'E');
  eq('execFileSync message', err && err.message, 'Command failed: sh -c printf E >&2; exit 5\nE');
}

// node exposes the same payloads positionally under output, with null for stdin
{
  const r = spawnSync('printf', ['hi'], { encoding: 'utf8' });
  eq('output is an array', Array.isArray(r.output), true);
  eq('output[0] is null', r.output[0], null);
  eq('output[1] is stdout', r.output[1], 'hi');
  eq('output[2] is stderr', r.output[2], '');
}

// shell may name a specific shell rather than just being a boolean; minimal images
// may only ship /bin/sh, so probe for bash and fall back
const shellPath = spawnSync('/bin/bash', ['-c', 'true']).status === 0 ? '/bin/bash' : '/bin/sh';
eq(
  'shell as a path',
  execSync('echo $0', { shell: shellPath, encoding: 'utf8' }).trim(),
  shellPath
);

// maxBuffer caps collected output and reports ENOBUFS, and must not wait for a child
// that keeps writing forever
{
  let err = null;
  try { execSync('printf "%0.sx" $(seq 1 5000)', { maxBuffer: 100, encoding: 'utf8', stdio: 'pipe' }); }
  catch (e) { err = e; }
  eq('maxBuffer throws', err && err.message, 'spawnSync /bin/sh ENOBUFS');
}
{
  const started = Date.now();
  let err = null;
  try { execSync('yes 2>/dev/null', { maxBuffer: 100, encoding: 'utf8', stdio: 'pipe' }); }
  catch (e) { err = e; }
  eq('maxBuffer stops an endless writer', err && err.message, 'spawnSync /bin/sh ENOBUFS');
  const quick = Date.now() - started < 5000;
  if (!quick) failures++;
  console.log(`${quick ? 'ok  ' : 'FAIL'}  maxBuffer did not hang`);
}
{
  const started = Date.now();
  let err = null;
  try { execSync('yes 2>/dev/null', { timeout: 300, encoding: 'utf8', stdio: 'pipe' }); }
  catch (e) { err = e; }
  eq('timeout stops an endless writer', err instanceof Error, true);
  const quick = Date.now() - started < 5000;
  if (!quick) failures++;
  console.log(`${quick ? 'ok  ' : 'FAIL'}  timeout did not hang on an endless writer`);
}

if (failures) { console.log(`FAIL: ${failures} check(s) failed`); process.exit(1); }
console.log('PASS');
