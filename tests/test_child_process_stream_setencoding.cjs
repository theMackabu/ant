const { spawn } = require('child_process');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const child = spawn(process.execPath, [
  '-e',
  'process.stdout.write("OUT"); process.stderr.write("ERR");'
]);

assert(child.stdout && typeof child.stdout.setEncoding === 'function', 'child.stdout.setEncoding should exist');
assert(child.stderr && typeof child.stderr.setEncoding === 'function', 'child.stderr.setEncoding should exist');
assert(child.stdout.setEncoding('utf8') === child.stdout, 'stdout.setEncoding should return stdout');
assert(child.stderr.setEncoding('utf8') === child.stderr, 'stderr.setEncoding should return stderr');
assert(child.stdout.readableEncoding === 'utf8', 'stdout readableEncoding should be utf8');
assert(child.stderr.readableEncoding === 'utf8', 'stderr readableEncoding should be utf8');

let stdout = '';
let stderr = '';

child.stdout.on('data', chunk => {
  assert(typeof chunk === 'string', 'stdout data should be a string after setEncoding');
  stdout += chunk;
});

child.stderr.on('data', chunk => {
  assert(typeof chunk === 'string', 'stderr data should be a string after setEncoding');
  stderr += chunk;
});

child.on('close', code => {
  assert(code === 0, `child exited ${code}`);
  assert(stdout === 'OUT', `expected stdout OUT, got ${JSON.stringify(stdout)}`);
  assert(stderr === 'ERR', `expected stderr ERR, got ${JSON.stringify(stderr)}`);
  console.log('child process stream setEncoding ok');
});
