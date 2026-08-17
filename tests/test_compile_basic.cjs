const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const ant = path.resolve(process.execPath);
const repoRoot = path.dirname(path.dirname(ant));
const runtime = path.join(repoRoot, 'build', 'ant-runtime');
const fixture = path.join(repoRoot, 'tests', 'fixtures', 'compile-app', 'index.ts');

if (!fs.existsSync(runtime)) {
  console.log('skip: build/ant-runtime not built (ninja -C build ant-runtime)');
  process.exit(0);
}

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-compile-'));
try {
  const out = path.join(tmp, 'app');

  let r = spawnSync(ant, ['compile', '--runtime', runtime, '-o', out, fixture], { encoding: 'utf8' });
  assert.equal(r.status, 0, `compile failed: ${r.stderr}`);
  assert.ok(fs.existsSync(out), 'output binary missing');

  r = spawnSync(out, ['a', 'b'], { encoding: 'utf8', cwd: os.homedir() });
  assert.equal(r.status, 0, `compiled app failed: ${r.stderr}`);

  const stdout = r.stdout;
  assert.ok(stdout.includes('greet: hello world'), stdout);
  assert.ok(stdout.includes('data: 3'), stdout);
  assert.ok(stdout.includes('notes: from the vfs'), stdout);
  assert.ok(stdout.includes('pad: ---x'), stdout);
  assert.ok(stdout.includes('cjs: legacy-7'), stdout);
  assert.ok(stdout.includes('argv: ["a","b"]'), stdout);
  assert.ok(stdout.includes(`execPath: ${fs.realpathSync(out)}`), stdout);
  assert.ok(stdout.includes('dirname: /$ant'), stdout);

  r = spawnSync(out, ['--throw'], { encoding: 'utf8', cwd: os.homedir() });
  assert.match(r.stderr, /\/\$ant\/lib\/util\.ts:\d+:\d+/, r.stderr);
  assert.match(r.stderr, /kaboom/, r.stderr);

  r = spawnSync(out, ['--fork'], { encoding: 'utf8', cwd: os.homedir() });
  assert.equal(r.status, 0, r.stderr);
  assert.ok(r.stdout.includes('fork exit: 0'), r.stdout);

  r = spawnSync(out, ['--worker'], { encoding: 'utf8', cwd: os.homedir() });
  assert.match(r.stderr, /Worker is not available in compiled executables/, r.stderr);

  r = spawnSync(runtime, [], { encoding: 'utf8' });
  assert.equal(r.status, 1);
  assert.match(r.stderr, /no embedded program/, r.stderr);
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
