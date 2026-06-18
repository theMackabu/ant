const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function runSource(dir, file, source) {
  const scriptPath = path.join(dir, file);
  fs.writeFileSync(scriptPath, source);
  const result = spawnSync(process.execPath, [scriptPath], { encoding: 'utf8' });
  if (result.error) throw result.error;
  return result;
}

const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-template-parse-'));

const missingBacktick = runSource(
  tmpDir,
  'missing-backtick.cjs',
  'console.log(`${a});\n'
);

assert(missingBacktick.status !== 0, 'missing backtick should fail during parsing');
assert(
  missingBacktick.stderr.includes('SyntaxError'),
  `expected SyntaxError for missing backtick\n${missingBacktick.stderr}`
);
assert(
  !missingBacktick.stderr.includes('ReferenceError'),
  `unterminated template should not execute expression\n${missingBacktick.stderr}`
);

const missingExpressionClose = runSource(
  tmpDir,
  'missing-expression-close.cjs',
  'console.log(`value: ${1 + 2`);\n'
);

assert(missingExpressionClose.status !== 0, 'missing template expression close should fail during parsing');
assert(
  missingExpressionClose.stderr.includes('SyntaxError'),
  `expected SyntaxError for missing expression close\n${missingExpressionClose.stderr}`
);

const closedAtEof = runSource(tmpDir, 'closed-at-eof.cjs', 'console.log(`ok ${1 + 2}`)');

assert(closedAtEof.status === 0, `template closed at EOF should run\n${closedAtEof.stderr}`);
assert(closedAtEof.stdout === 'ok 3\n', `unexpected closed-at-EOF output: ${JSON.stringify(closedAtEof.stdout)}`);

console.log('parser unterminated template test passed');
