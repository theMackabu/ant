const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const ant = path.resolve(process.execPath);
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-compile-'));

function run(bin, args, opts) {
  const result = spawnSync(bin, args, { encoding: 'utf8', ...opts });
  if (result.error) throw result.error;
  return result;
}

try {
  const appDir = path.join(tmp, 'app');
  fs.mkdirSync(path.join(appDir, 'lib'), { recursive: true });
  fs.writeFileSync(
    path.join(appDir, 'main.js'),
    "import { greet } from './lib/util.js';\n" +
      "console.log(greet('fused'));\n" +
      "console.log('args:' + process.argv.slice(2).join(','));\n"
  );
  fs.writeFileSync(
    path.join(appDir, 'lib', 'util.js'),
    "export function greet(who) { return 'hello, ' + who; }\n"
  );

  const out = path.join(tmp, 'myapp');
  const compiled = run(ant, [
    '--no-color',
    'compile',
    path.join(appDir, 'main.js'),
    '-o',
    out,
  ]);
  assert.strictEqual(
    compiled.status,
    0,
    `compile failed\nstdout:\n${compiled.stdout}\nstderr:\n${compiled.stderr}`
  );
  assert.ok(fs.existsSync(out), 'output binary was not written');

  const ran = run(out, ['alpha', 'beta']);
  assert.strictEqual(
    ran.status,
    0,
    `fused binary failed\nstdout:\n${ran.stdout}\nstderr:\n${ran.stderr}`
  );
  assert.match(ran.stdout, /hello, fused/);
  assert.match(ran.stdout, /args:alpha,beta/);

  // A plain ant binary must not be mistaken for a fused binary.
  const plain = run(ant, ['--no-color', '-e', "console.log('plain-ok')"]);
  assert.strictEqual(plain.status, 0);
  assert.match(plain.stdout, /plain-ok/);

  console.log('compile single-file ok');
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
