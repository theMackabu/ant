const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const tmp = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'ant-update-url-pin-')));
const project = path.join(tmp, 'project');
const manifest = path.join(project, 'package.json');
const pinned = 'https://npm.ants.land/maid/-/maid-0.4.0.tgz';
const ant = path.resolve(process.execPath);

function run(...args) {
  const env = { ...process.env, HOME: path.join(tmp, 'home'), NO_COLOR: '1' };
  delete env.XDG_DATA_HOME;
  delete env.XDG_CACHE_HOME;
  delete env.FORCE_COLOR;
  delete env.UPM_DEFAULT_REGISTRY;
  delete env.UPM_FALLBACK_REGISTRY;
  const result = spawnSync(ant, args, { cwd: project, encoding: 'utf8', env });
  assert.strictEqual(result.status, 0, `ant ${args.join(' ')} failed\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`);
}

try {
  fs.mkdirSync(project, { recursive: true });
  fs.writeFileSync(manifest, JSON.stringify({ name: 'pinned', private: true, dependencies: { maid: pinned } }));

  run('update', 'maid');

  const declared = JSON.parse(fs.readFileSync(manifest, 'utf8')).dependencies.maid;
  assert.notStrictEqual(declared, pinned);
  assert.match(declared, /^https:\/\/npm\.ants\.land\/maid\/-\/maid-\d+\.\d+\.\d+\.tgz$/);

  const installed = JSON.parse(fs.readFileSync(path.join(project, 'node_modules', 'maid', 'package.json'), 'utf8'));
  assert.notStrictEqual(installed.version, '0.4.0');
  assert.ok(declared.endsWith(`maid-${installed.version}.tgz`));

  console.log('ok');
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
