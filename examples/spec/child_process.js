import { test, testThrows, summary } from './helpers.js';
import { spawn, exec, execSync, spawnSync } from 'child_process';

console.log('Child Process Tests\n');

test('execSync returns stdout', execSync('echo hello').trim(), 'hello');
test('execSync with spaces', execSync('echo "hello world"').trim(), 'hello world');
test('execSync exit code 0', typeof execSync('true'), 'string');

testThrows('execSync throws on non-zero exit', () => {
  execSync('exit 1');
});

const syncResult = spawnSync('echo', ['hello', 'world']);
test('spawnSync stdout', syncResult.stdout.trim(), 'hello world');
test('spawnSync status 0', syncResult.status, 0);
test('spawnSync signal null', syncResult.signal, null);
test('spawnSync has pid', syncResult.pid > 0, true);

const failSync = spawnSync('sh', ['-c', 'exit 42']);
test('spawnSync non-zero status', failSync.status, 42);

const stderrSync = spawnSync('sh', ['-c', 'echo error >&2']);
test('spawnSync captures stderr', stderrSync.stderr.trim(), 'error');

const inputSync = spawnSync('cat', [], { input: 'test input' });
test('spawnSync with input', inputSync.stdout, 'test input');

let execDone = false;
exec('echo async').then(result => {
  test('exec resolves with stdout', result.stdout.trim(), 'async');
  test('exec exitCode', result.exitCode, 0);
  execDone = true;
});

let execFailDone = false;
exec('exit 1').catch(err => {
  test('exec rejects on non-zero exit', typeof err, 'string');
  execFailDone = true;
});

let spawnStdout = '';
let spawnExitCode = null;
let spawnClosed = false;

const child = spawn('sh', ['-c', 'echo line1; echo line2']);

test('spawn returns object', typeof child, 'object');
test('spawn has pid', child.pid > 0, true);
test('spawn has on method', typeof child.on, 'function');
test('spawn has once method', typeof child.once, 'function');
test('spawn has kill method', typeof child.kill, 'function');

child.on('stdout', data => {
  spawnStdout += data;
});

child.on('exit', code => {
  spawnExitCode = code;
});

child.on('close', () => {
  spawnClosed = true;
  test('spawn collects stdout', spawnStdout.trim(), 'line1\nline2');
  test('spawn exit code', spawnExitCode, 0);
  test('spawn close event fired', spawnClosed, true);

  const shellChild = spawn('echo $HOME', [], { shell: true });
  shellChild.on('close', () => {
    test('spawn with shell option', shellChild.stdout.length > 0, true);

    setTimeout(() => {
      test('exec async completed', execDone, true);
      test('exec fail async completed', execFailDone, true);
      summary();
    }, 100);
  });
});

const stderrChild = spawn('sh', ['-c', 'echo err >&2']);
let stderrData = '';
stderrChild.on('stderr', data => {
  stderrData += data;
});
stderrChild.on('close', () => {
  test('spawn captures stderr', stderrData.trim(), 'err');
});

const longChild = spawn('sleep', ['10']);
longChild.on('close', () => {});
const killResult = longChild.kill('SIGTERM');
test('spawn kill returns true', killResult, true);
