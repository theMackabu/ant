const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

if (process.platform === 'win32') {
  console.log('skipping ant run fresh tree test on win32');
  process.exit(0);
}

const ant = path.resolve(process.execPath);
const tmp = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'ant-run-fresh-')));
const home = path.join(tmp, 'home');
const app = path.join(tmp, 'app');

function antIn(dir, ...args) {
  const env = { ...process.env, HOME: home, NO_COLOR: '1' };
  delete env.XDG_DATA_HOME;
  delete env.XDG_CACHE_HOME;
  delete env.FORCE_COLOR;
  const result = spawnSync(ant, args, { cwd: dir, encoding: 'utf8', env });
  if (result.error) throw result.error;
  return result;
}

function tarball(name, version) {
  const pkg = path.join(tmp, 'src', name, 'package');
  fs.mkdirSync(pkg, { recursive: true });
  fs.writeFileSync(path.join(pkg, 'package.json'), JSON.stringify({ name, version, main: 'index.js' }));
  fs.writeFileSync(path.join(pkg, 'index.js'), `module.exports = ${JSON.stringify(`${name}@${version}`)};\n`);
  const file = path.join(app, `${name}-${version}.tgz`);
  assert.strictEqual(spawnSync('tar', ['-czf', file, '-C', path.dirname(pkg), 'package']).status, 0);
  return `file:./${path.basename(file)}`;
}

function manifest(deps) {
  fs.writeFileSync(
    path.join(app, 'package.json'),
    JSON.stringify({
      name: 'app',
      scripts: {
        show: 'node -e "console.log(require(\'dep\'), process.env.npm_lifecycle_event, process.argv.slice(1).join(\',\'))" --',
        fail: 'exit 7',
      },
      dependencies: deps,
    })
  );
}

try {
  fs.mkdirSync(home, { recursive: true });
  fs.mkdirSync(app, { recursive: true });
  manifest({ dep: tarball('dep', '1.0.0') });
  assert.strictEqual(antIn(app, 'install').status, 0);

  const fresh = antIn(app, 'run', 'show', 'a', 'b c');
  assert.strictEqual(fresh.status, 0, fresh.stderr);
  assert.match(fresh.stdout, /dep@1\.0\.0 show a,b c/);
  assert.doesNotMatch(fresh.stdout + fresh.stderr, /installed/);

  const dashed = antIn(app, 'run', 'show', '--', '--flag');
  assert.match(dashed.stdout, /dep@1\.0\.0 show --flag/);

  assert.strictEqual(antIn(app, 'run', 'fail').status, 7);

  const missing = antIn(app, 'run', 'nope');
  assert.strictEqual(missing.status, 1);
  assert.match(missing.stderr, /missing script "nope"/);

  fs.rmSync(path.join(app, 'node_modules', 'dep'));
  const broken = antIn(app, 'run', 'show');
  assert.strictEqual(broken.status, 0, broken.stderr);
  assert.match(broken.stdout, /dep@1\.0\.0 show/);
  assert.ok(fs.existsSync(path.join(app, 'node_modules', 'dep', 'index.js')));

  console.log('ant run uses a fresh tree directly and repairs a broken one first');
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
