const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

if (process.platform === 'win32') {
  console.log('skipping package lifecycle test on win32');
  process.exit(0);
}

const ant = path.resolve(process.execPath);
const tmp = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'ant-pkg-lifecycle-')));
const home = path.join(tmp, 'home');
const outPath = path.join(tmp, 'lifecycle.log');

function antIn(dir, ...args) {
  const env = { ...process.env, HOME: home, NO_COLOR: '1', ANT_TEST_LIFECYCLE_OUT: outPath };
  delete env.XDG_DATA_HOME;
  delete env.XDG_CACHE_HOME;
  delete env.FORCE_COLOR;
  const result = spawnSync(ant, args, { cwd: dir, encoding: 'utf8', env });
  if (result.error) throw result.error;
  return result;
}

function ok(result, what) {
  assert.strictEqual(result.status, 0, `${what} failed\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`);
  return result;
}

function project(name, manifest, files = {}) {
  const root = path.join(tmp, name);
  const pkg = path.join(root, 'src', 'package');
  fs.mkdirSync(pkg, { recursive: true });
  fs.writeFileSync(path.join(pkg, 'package.json'), JSON.stringify({ name, version: '1.0.0', ...manifest }));
  for (const [file, text] of Object.entries(files)) fs.writeFileSync(path.join(pkg, file), text);
  assert.strictEqual(spawnSync('tar', ['-czf', path.join(root, 'dep.tgz'), '-C', path.dirname(pkg), 'package']).status, 0);
  fs.writeFileSync(path.join(root, 'package.json'), JSON.stringify({ name: `${name}-app`, dependencies: { [name]: 'file:./dep.tgz' } }));

  const installed = ok(antIn(root, 'install'), `install ${name}`);
  assert.match(installed.stdout + installed.stderr, new RegExp(`lifecycle scripts not run for ${name}`));
  return root;
}

try {
  fs.mkdirSync(home);

  const dual = project('dual-lifecycle', {
    scripts: {
      install: 'printf install >> "$ANT_TEST_LIFECYCLE_OUT"',
      postinstall: 'printf ",postinstall" >> "$ANT_TEST_LIFECYCLE_OUT"',
    },
  });
  assert.strictEqual(fs.existsSync(outPath), false);

  const listed = ok(antIn(dual, 'trust'), 'trust listing');
  assert.match(listed.stdout, /dual-lifecycle/);
  assert.strictEqual(fs.existsSync(outPath), false);

  ok(antIn(dual, 'trust', 'dual-lifecycle'), 'trust dual-lifecycle');
  assert.strictEqual(fs.readFileSync(outPath, 'utf8'), 'install,postinstall');
  assert.deepStrictEqual(JSON.parse(fs.readFileSync(path.join(dual, 'package.json'), 'utf8')).trustedDependencies, ['dual-lifecycle']);

  const again = ok(antIn(dual, 'trust', 'dual-lifecycle'), 'second trust');
  assert.match(again.stdout + again.stderr, /no package has lifecycle scripts waiting to run/);
  ok(antIn(dual, 'install'), 'install after trust');
  assert.strictEqual(fs.readFileSync(outPath, 'utf8'), 'install,postinstall');

  const quiet = project('quiet-lifecycle', {
    scripts: { install: 'echo quiet lifecycle stdout; echo quiet lifecycle stderr >&2' },
  });
  const quietRun = ok(antIn(quiet, 'trust', 'quiet-lifecycle'), 'trust quiet-lifecycle');
  assert.doesNotMatch(quietRun.stdout + quietRun.stderr, /quiet lifecycle/);

  const failing = project('fail-lifecycle', {
    scripts: { install: 'echo lifecycle failed stdout; echo lifecycle failed stderr >&2; exit 7' },
  });
  const failed = antIn(failing, 'trust', 'fail-lifecycle');
  assert.notStrictEqual(failed.status, 0);
  assert.match(failed.stderr, /lifecycle failed stdout/);
  assert.match(failed.stderr, /lifecycle failed stderr/);
  assert.match(failed.stderr, /fail-lifecycle: install script exited with code 7/);
  assert.match(ok(antIn(failing, 'trust'), 'trust listing after failure').stdout, /fail-lifecycle/);

  const gyp = project('gyp-lifecycle', {}, { 'binding.gyp': '{ "targets": [] }' });
  assert.match(ok(antIn(gyp, 'trust'), 'trust listing for gyp').stdout, /install: node-gyp rebuild/);

  const shim = project('node-shim-lifecycle', {
    scripts: { postinstall: 'node -e "require(\'fs\').writeFileSync(process.env.ANT_TEST_LIFECYCLE_OUT, String(!!process.versions.ant))"' },
  });
  ok(antIn(shim, 'trust', 'node-shim-lifecycle'), 'trust node-shim-lifecycle');
  assert.strictEqual(fs.readFileSync(outPath, 'utf8'), 'true');

  console.log('package lifecycle order, reruns, output and shims work');
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
