const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-esm-self-reference-'));
const ant = path.resolve(process.execPath);

try {
  const storeDir = path.join(tmp, 'store', 'selfref-fixture');
  fs.mkdirSync(storeDir, { recursive: true });

  fs.writeFileSync(
    path.join(storeDir, 'package.json'),
    JSON.stringify({
      name: 'selfref-fixture',
      exports: {
        './find': { import: './find.mjs', require: './find.cjs' },
        './walk': { import: './walk.mjs' },
        './resolve': { import: './resolve.mjs', require: './resolve.cjs' },
        './probe': './probe.mjs'
      }
    })
  );
  fs.writeFileSync(path.join(storeDir, 'find.mjs'), 'import { up } from "selfref-fixture/walk";\nexport const chain = `find->${up}`;\n');
  fs.writeFileSync(path.join(storeDir, 'walk.mjs'), 'import { leaf } from "selfref-fixture/resolve";\nexport const up = `walk->${leaf}`;\n');
  fs.writeFileSync(path.join(storeDir, 'resolve.mjs'), 'export const leaf = "resolve:import";\n');
  fs.writeFileSync(path.join(storeDir, 'resolve.cjs'), 'module.exports = { leaf: "resolve:require" };\n');
  fs.writeFileSync(
    path.join(storeDir, 'find.cjs'),
    'const { leaf } = require("selfref-fixture/resolve");\nmodule.exports = { chain: `find->${leaf}` };\n'
  );
  fs.writeFileSync(path.join(storeDir, 'secret.mjs'), 'export const secret = true;\n');
  fs.writeFileSync(
    path.join(storeDir, 'probe.mjs'),
    'export async function probeSecret() {\n' +
      '  try { await import("selfref-fixture/secret"); return "resolved"; }\n' +
      '  catch { return "rejected"; }\n' +
      '}\n'
  );

  const appDir = path.join(tmp, 'app');
  const nodeModules = path.join(appDir, 'node_modules');
  fs.mkdirSync(nodeModules, { recursive: true });
  fs.writeFileSync(path.join(appDir, 'package.json'), JSON.stringify({ name: 'app-fixture', type: 'module', exports: { '.': './main.mjs' } }));
  fs.symlinkSync(storeDir, path.join(nodeModules, 'selfref-fixture'), 'junction');

  const shadowDir = path.join(nodeModules, 'app-fixture');
  fs.mkdirSync(shadowDir, { recursive: true });
  fs.writeFileSync(path.join(shadowDir, 'package.json'), JSON.stringify({ name: 'app-fixture', exports: { './hidden': './hidden.mjs' } }));
  fs.writeFileSync(path.join(shadowDir, 'hidden.mjs'), 'export const hidden = "shadow";\n');

  fs.writeFileSync(
    path.join(nodeModules, 'loose.mjs'),
    'export async function probeApp() {\n' +
      '  try { await import("app-fixture"); return "resolved"; }\n' +
      '  catch { return "rejected"; }\n' +
      '}\n'
  );

  const externalDir = path.join(nodeModules, 'external-pkg');
  fs.mkdirSync(externalDir, { recursive: true });
  fs.writeFileSync(path.join(externalDir, 'package.json'), JSON.stringify({ name: 'external-pkg', exports: { '.': { import: './index.mjs' } } }));
  fs.writeFileSync(path.join(externalDir, 'index.mjs'), 'export const external = "external-ok";\n');

  fs.writeFileSync(
    path.join(appDir, 'main.mjs'),
    [
      'import { createRequire } from "node:module";',
      '',
      'const { chain } = await import("selfref-fixture/find");',
      'if (chain !== "find->walk->resolve:import") throw new Error(`bad import chain: ${chain}`);',
      '',
      'const { external } = await import("external-pkg");',
      'if (external !== "external-ok") throw new Error(`bad external import: ${external}`);',
      '',
      'const requireFromHere = createRequire(import.meta.url);',
      'const cjs = requireFromHere("selfref-fixture/find");',
      'if (cjs.chain !== "find->resolve:require") throw new Error(`bad require chain: ${cjs.chain}`);',
      '',
      'const { probeSecret } = await import("selfref-fixture/probe");',
      'const secretResult = await probeSecret();',
      'if (secretResult !== "rejected") throw new Error(`unexported subpath should stay rejected, got ${secretResult}`);',
      '',
      'let shadowResult;',
      'try { await import("app-fixture/hidden"); shadowResult = "resolved"; }',
      'catch { shadowResult = "rejected"; }',
      'if (shadowResult !== "rejected") throw new Error(`self reference must not fall back to shadow package, got ${shadowResult}`);',
      '',
      'const { probeApp } = await import("./node_modules/loose.mjs");',
      'const looseResult = await probeApp();',
      'if (looseResult !== "rejected") throw new Error(`node_modules boundary should block self reference, got ${looseResult}`);',
      '',
      'console.log("self reference chain ok");',
      ''
    ].join('\n')
  );

  const result = spawnSync(ant, ['--no-color', path.join(appDir, 'main.mjs')], {
    cwd: appDir,
    encoding: 'utf8'
  });
  if (result.error) throw result.error;

  assert.strictEqual(result.status, 0, `self reference fixture failed\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`);
  assert.match(result.stdout, /self reference chain ok/);

  console.log('esm package self reference ok');
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
