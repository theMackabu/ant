const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

if (process.platform === 'win32') {
  console.log('skipping ant global bins test on win32');
  process.exit(0);
}

const tmp = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'ant-global-bins-')));
const home = path.join(tmp, 'home');
const pkg = path.join(tmp, 'src', 'package');
const tarball = path.join(tmp, 'evilbin-1.0.0.tgz');
const bin = path.join(home, '.ant', 'bin');
const ant = path.resolve(process.execPath);

function run(...args) {
  const env = { ...process.env, HOME: home, NO_COLOR: '1' };
  delete env.XDG_DATA_HOME;
  delete env.XDG_CACHE_HOME;
  delete env.FORCE_COLOR;
  const result = spawnSync(ant, args, { cwd: tmp, encoding: 'utf8', env });
  assert.strictEqual(result.status, 0, `ant ${args.join(' ')} failed\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`);
  return result.stdout + result.stderr;
}

try {
  fs.mkdirSync(bin, { recursive: true });
  fs.mkdirSync(pkg, { recursive: true });
  fs.writeFileSync(path.join(pkg, 'x.js'), '#!/usr/bin/env node\nconsole.log("x")\n');
  fs.writeFileSync(
    path.join(pkg, 'package.json'),
    JSON.stringify({
      name: 'evilbin',
      version: '1.0.0',
      bin: { ok: 'x.js', taken: 'x.js', '../escape': 'x.js', outside: '../../../outside.js' },
    })
  );
  fs.writeFileSync(path.join(bin, 'taken'), 'mine');
  assert.strictEqual(spawnSync('tar', ['-czf', tarball, '-C', path.dirname(pkg), 'package']).status, 0);

  const added = run('add', '-g', `file:./${path.basename(tarball)}`);
  assert.match(added, /taken exists and was not linked by ant/);

  assert.ok(fs.lstatSync(path.join(bin, 'ok')).isSymbolicLink());
  assert.strictEqual(fs.readFileSync(path.join(bin, 'taken'), 'utf8'), 'mine');
  assert.strictEqual(fs.existsSync(path.join(home, '.ant', 'escape')), false);
  assert.deepStrictEqual(fs.readdirSync(bin).sort(), ['escape', 'ok', 'outside', 'taken']);

  const installed = path.join(home, '.ant', 'pkg', 'global', 'node_modules', 'evilbin');
  for (const name of ['escape', 'ok', 'outside']) {
    const target = path.resolve(bin, fs.readlinkSync(path.join(bin, name)));
    assert.ok(target.startsWith(installed + path.sep), `${name} -> ${target}`);
  }

  run('remove', '-g', 'evilbin');
  assert.deepStrictEqual(fs.readdirSync(bin), ['taken']);
  assert.strictEqual(fs.readFileSync(path.join(bin, 'taken'), 'utf8'), 'mine');

  console.log('ant global bins stay inside the bin dir');
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
