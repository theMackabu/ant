const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-cli-eval-print-'));

function run(args, input) {
  const result = spawnSync(process.execPath, args, {
    cwd: tmp,
    input: input ?? '',
    encoding: 'utf8',
    env: { ...process.env, NO_COLOR: '1', FORCE_COLOR: '0' },
  });
  if (result.error) throw result.error;
  return result;
}

function out(args, input) {
  const result = run(args, input);
  assert.equal(result.status, 0, `${args.join(' ')}\n${result.stderr}`);
  return result.stdout.trim();
}

try {
  fs.writeFileSync(path.join(tmp, 'dep.cjs'), 'module.exports = "dep ok";\n');
  fs.writeFileSync(path.join(tmp, 'argv.cjs'), 'console.log(JSON.stringify(process.argv.slice(2)), require("path").basename(process.argv[1]));\n');

  assert.equal(out(['-p', '1+1']), '2');
  assert.equal(out(['--print', '6*7']), '42');
  assert.equal(out(['-p', '-e', '7*6']), '42');
  assert.equal(out(['-p', '"str"']), 'str');
  assert.equal(out(['-p', 'var q = 5; q * 2']), '10');
  assert.equal(out(['-p', 'undefined']), 'undefined');
  assert.equal(out(['-p', 'this === globalThis']), 'true');
  assert.equal(out(['-p', 'typeof require + " " + typeof module + " " + (module.exports === exports)']), 'function object true');
  assert.equal(out(['-p', '__filename + " " + __dirname + " " + module.id']), '[eval] . [eval]');
  assert.equal(out(['-p', 'require("./dep.cjs")']), 'dep ok');
  assert.equal(out(['-e', 'var leaked = 1; console.log(globalThis.leaked)']), '1');
  assert.equal(out(['-p'], '1+1'), '2');

  const returned = run(['-e', 'return 1']);
  assert.notEqual(returned.status, 0);
  assert.match(returned.stderr.replace(/\x1b\[[0-9;]*m/g, ''), /SyntaxError: Illegal return statement/);

  assert.equal(out(['-e', 'console.log(JSON.stringify(process.argv.slice(1)))', '--', 'a', 'b']), '["a","b"]');
  assert.equal(out(['-e', 'console.log(JSON.stringify(process.argv.slice(1)))', 'a', 'b']), '["a","b"]');
  assert.equal(out(['-', 'a', 'b'], 'console.log(JSON.stringify(process.argv.slice(1)))'), '["-","a","b"]');
  assert.equal(out(['--', 'argv.cjs', '--flag', 'x']), '["--flag","x"] argv.cjs');
  assert.equal(out(['argv.cjs', 'x', '--', 'y']), '["x","--","y"] argv.cjs');

  console.log('cli eval and print follow node');
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
