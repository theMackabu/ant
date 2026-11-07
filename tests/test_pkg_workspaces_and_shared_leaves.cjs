const assert = require('node:assert');
const { spawn, spawnSync } = require('node:child_process');
const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');

if (process.platform === 'win32') {
  console.log('skipping workspaces and shared leaves test on win32');
  process.exit(0);
}

const ant = path.resolve(process.execPath);
const tmp = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'ant-pkg-ws-leaves-')));
const home = path.join(tmp, 'home');
const files = new Map();
const packuments = new Map();

function tarball(name, manifest, extra) {
  const dir = path.join(tmp, 'src', name, 'package');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(path.join(dir, 'package.json'), JSON.stringify(manifest));
  for (const [file, { text, mode }] of Object.entries(extra)) {
    fs.writeFileSync(path.join(dir, file), text);
    if (mode) fs.chmodSync(path.join(dir, file), mode);
  }
  const file = path.join(tmp, 'src', name, 'package.tgz');
  assert.strictEqual(spawnSync('tar', ['-czf', file, '-C', path.dirname(dir), 'package']).status, 0);
  return fs.readFileSync(file);
}

function publish(base, manifest, extra = {}) {
  const bytes = tarball(`${manifest.name}-${manifest.version}`, manifest, extra);
  const url = `/${manifest.name}/-/${manifest.name}-${manifest.version}.tgz`;
  files.set(url, bytes);
  const integrity = `sha512-${crypto.createHash('sha512').update(bytes).digest('base64')}`;
  const doc = packuments.get(manifest.name) ?? { name: manifest.name, versions: {}, 'dist-tags': {}, time: {} };
  doc.versions[manifest.version] = { ...manifest, dist: { tarball: `${base}${url}`, integrity } };
  doc['dist-tags'].latest = manifest.version;
  doc.time[manifest.version] = '2020-01-01T00:00:00.000Z';
  packuments.set(manifest.name, doc);
}

function antIn(dir, ...args) {
  const env = { ...process.env, HOME: home, NO_COLOR: '1' };
  for (const key of ['XDG_DATA_HOME', 'XDG_CACHE_HOME', 'XDG_CONFIG_HOME', 'FORCE_COLOR', 'UPM_DEFAULT_REGISTRY', 'UPM_FALLBACK_REGISTRY', 'UPM_STORE']) delete env[key];
  return new Promise((resolve, reject) => {
    const child = spawn(ant, args, { cwd: dir, env });
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', chunk => (stdout += chunk));
    child.stderr.on('data', chunk => (stderr += chunk));
    child.on('error', reject);
    child.on('close', status => resolve({ status, stdout, stderr }));
  });
}

function ok(result, what) {
  assert.strictEqual(result.status, 0, `${what} failed\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`);
  return result;
}

function project(name, manifest, extra = {}) {
  const dir = path.join(tmp, name);
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(path.join(dir, 'package.json'), JSON.stringify(manifest));
  for (const [file, text] of Object.entries(extra)) {
    fs.mkdirSync(path.dirname(path.join(dir, file)), { recursive: true });
    fs.writeFileSync(path.join(dir, file), text);
  }
  return dir;
}

const server = http.createServer((req, res) => {
  const url = decodeURIComponent(req.url.split('?')[0]);
  if (files.has(url)) {
    res.writeHead(200, { 'content-type': 'application/octet-stream' });
    return res.end(files.get(url));
  }
  const doc = packuments.get(url.slice(1));
  if (doc) {
    res.writeHead(200, { 'content-type': 'application/json' });
    return res.end(JSON.stringify(doc));
  }
  res.writeHead(404, { 'content-type': 'application/json' });
  res.end('{"error":"not found"}');
});

async function npmWorkspaces() {
  const dir = project('npm-workspaces', { name: 'root', private: true, workspaces: ['packages/a'] }, {
    'packages/a/package.json': JSON.stringify({ name: 'a', version: '1.0.0' }),
  });
  const first = ok(await antIn(dir, 'install'), 'npm workspaces install');
  assert.match(first.stdout + first.stderr, /1 ws/);
  const lock = JSON.parse(fs.readFileSync(path.join(dir, 'ant.lock'), 'utf8'));
  assert.deepStrictEqual(lock.root.workspaces, ['packages/a']);
  const second = ok(await antIn(dir, 'install'), 'npm workspaces reinstall');
  assert.match(second.stdout + second.stderr, /up to date/);
}

async function sharedLeaf(registry) {
  const dir = project('shared-leaf', { name: 'leaf-app', private: true, dependencies: { leafpkg: '1.0.0' } }, {
    '.npmrc': `registry=${registry}/\n`,
  });
  ok(await antIn(dir, 'install'), 'shared leaf install');
  const resolved = spawnSync(ant, ['-e', "console.log(require('fs').realpathSync(require.resolve('leafpkg')))"], {
    cwd: dir,
    encoding: 'utf8',
  });
  assert.strictEqual(resolved.status, 0, resolved.stderr);
  const real = resolved.stdout.trim();
  assert.ok(!real.startsWith(dir), `leafpkg should be shared from the store, got ${real}`);
  assert.ok(real.endsWith(`${path.sep}node_modules${path.sep}leafpkg${path.sep}index.js`), `shared leaf realpath keeps node_modules/<name>: ${real}`);

  const entry = fs.readdirSync(path.join(dir, 'node_modules', '.ant')).find(key => key.startsWith('leafpkg@'));
  const at = path.join(dir, 'node_modules', '.ant', entry, 'node_modules', 'leafpkg');
  ok(await antIn(dir, 'config', 'set', 'install.sharedLeaves', 'off'), 'turn shared leaves off');
  ok(await antIn(dir, 'install'), 'install with shared leaves off');
  assert.strictEqual(fs.lstatSync(at).isSymbolicLink(), false, 'turning sharing off relinks the entry with its own files');
  assert.strictEqual(fs.readFileSync(path.join(at, 'index.js'), 'utf8'), 'module.exports = 1;\n');
  ok(await antIn(dir, 'config', 'set', 'install.sharedLeaves', 'on'), 'turn shared leaves on');
}

async function execBesideForeignWorkspaces(registry) {
  const dir = project('pnpm-workspace', { name: 'root', private: true }, {
    '.npmrc': `registry=${registry}/\n`,
    'pnpm-workspace.yaml': 'packages:\n  - packages/*\n',
    'packages/a/package.json': JSON.stringify({ name: 'a', version: '1.0.0' }),
    'pnpm-lock.yaml': "lockfileVersion: '9.0'\n\nimporters:\n\n  .: {}\n\n  packages/a: {}\n",
  });
  const before = fs.readdirSync(dir).sort();
  const ran = ok(await antIn(dir, 'x', 'exectool@1.0.0'), 'ant x beside a pnpm workspace lock');
  assert.match(ran.stdout, /exectool ran/);
  assert.deepStrictEqual(fs.readdirSync(dir).sort(), before, 'ant x leaves the project alone');
}

async function main() {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const registry = `http://127.0.0.1:${server.address().port}`;
  publish(registry, { name: 'leafpkg', version: '1.0.0', main: 'index.js' }, { 'index.js': { text: 'module.exports = 1;\n' } });
  publish(registry, { name: 'exectool', version: '1.0.0', bin: { exectool: 'bin.sh' } }, {
    'bin.sh': { text: '#!/bin/sh\necho exectool ran\n', mode: 0o755 },
  });
  fs.mkdirSync(home);

  await npmWorkspaces();
  await sharedLeaf(registry);
  await execBesideForeignWorkspaces(registry);
  console.log('ok');
}

main()
  .catch(error => {
    process.exitCode = 1;
    console.error(error);
  })
  .finally(() => {
    server.close();
    fs.rmSync(tmp, { recursive: true, force: true });
  });
