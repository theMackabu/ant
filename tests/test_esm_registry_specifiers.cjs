const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const tmp = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'ant-registry-specifiers-')));
const home = path.join(tmp, 'home');
const imports = path.join(home, '.cache', 'ant', 'imports');
const ant = path.resolve(process.execPath);

function run(cwd, file, source) {
  fs.mkdirSync(cwd, { recursive: true });
  fs.writeFileSync(path.join(cwd, file), source);
  const env = { ...process.env, HOME: home, NO_COLOR: '1' };
  delete env.XDG_DATA_HOME;
  delete env.XDG_CACHE_HOME;
  delete env.FORCE_COLOR;
  delete env.UPM_DEFAULT_REGISTRY;
  delete env.UPM_FALLBACK_REGISTRY;
  return spawnSync(ant, [file], { cwd, encoding: 'utf8', env });
}

function ok(cwd, file, source) {
  const result = run(cwd, file, source);
  assert.strictEqual(result.status, 0, `${file} failed\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`);
  return result.stdout.trim();
}

function slots() {
  return fs.readdirSync(imports).flatMap(scope => fs.readdirSync(path.join(imports, scope)).map(slot => path.join(imports, scope, slot)));
}

try {
  const scripts = path.join(tmp, 'scripts');

  const mixed = `
    import ms from 'npm:ms@2.1.3';
    import { join } from 'jsr:@std/path@1/posix';
    import pkg from 'npm:ms@2.1.3/package.json' with { type: 'json' };
    import { createRequire } from 'node:module';
    const required = createRequire(import.meta.url)('npm:/ms@2.1.3');
    const dynamic = await import('npm:ms@2.1.3');
    console.log(ms('2h'), join('a', 'b'), pkg.version, required === dynamic.default, typeof dynamic.default);
  `;
  assert.strictEqual(ok(scripts, 'mixed.mjs', mixed), '7200000 a/b 2.1.3 true function');
  assert.strictEqual(slots().length, 1);

  const before = fs.statSync(path.join(slots()[0], 'ant.lock')).mtimeMs;
  assert.strictEqual(ok(scripts, 'mixed.mjs', mixed), '7200000 a/b 2.1.3 true function');
  assert.strictEqual(fs.statSync(path.join(slots()[0], 'ant.lock')).mtimeMs, before);

  const peers = `
    import React from 'npm:react@18.3.1';
    import { renderToString } from 'npm:react-dom@18.3.1/server';
    import { createRequire } from 'node:module';
    const fromDom = createRequire(import.meta.resolve('npm:react-dom@18.3.1'))('react');
    console.log(renderToString(React.createElement('b', null, 'hi')), fromDom === React);
  `;
  assert.strictEqual(ok(scripts, 'peers.mjs', peers), '<b>hi</b> true');

  const versions = `
    import a from 'npm:ms@2.0.0/package.json' with { type: 'json' };
    import b from 'npm:ms@2.1.3/package.json' with { type: 'json' };
    console.log(a.version, b.version);
  `;
  assert.strictEqual(ok(scripts, 'versions.mjs', versions), '2.0.0 2.1.3');

  const project = path.join(tmp, 'project');
  fs.mkdirSync(path.join(project, 'node_modules', 'ms'), { recursive: true });
  fs.writeFileSync(path.join(project, 'package.json'), JSON.stringify({ dependencies: { ms: '0.0.1' } }));
  fs.writeFileSync(path.join(project, 'node_modules', 'ms', 'package.json'), JSON.stringify({ name: 'ms', version: '0.0.1', main: 'index.js' }));
  fs.writeFileSync(path.join(project, 'node_modules', 'ms', 'index.js'), 'module.exports = "local";');
  const declared = `
    import local from 'npm:ms';
    import pinned from 'npm:ms@2.1.3';
    console.log(local, typeof pinned);
  `;
  assert.strictEqual(ok(project, 'declared.mjs', declared), 'local function');

  const land = `
    import a from 'ants.land:maid/package.json' with { type: 'json' };
    import b from 'land:maid/package.json' with { type: 'json' };
    console.log(a.name, a === b);
  `;
  assert.strictEqual(ok(scripts, 'land.mjs', land), 'maid true');

  const missing = run(scripts, 'missing.mjs', `import 'npm:ant-registry-specifier-test-does-not-exist';`);
  assert.notStrictEqual(missing.status, 0);
  assert.match(missing.stderr, /not found in registry/);
  assert.match(missing.stderr, /Cannot resolve module: npm:ant-registry-specifier-test-does-not-exist/);
  for (const slot of slots()) {
    const deps = JSON.parse(fs.readFileSync(path.join(slot, 'package.json'), 'utf8')).dependencies ?? {};
    assert.ok(!Object.hasOwn(deps, 'ant-registry-specifier-test-does-not-exist'));
  }

  for (const bad of ['npm:../../etc', 'npm:ms@file:../x', 'jsr:unscoped', 'npm:']) {
    const result = run(scripts, 'bad.cjs', `require(${JSON.stringify(bad)});`);
    assert.notStrictEqual(result.status, 0, bad);
    assert.match(result.stderr, /Cannot (resolve|find) module/, bad);
  }

  console.log('ok');
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
