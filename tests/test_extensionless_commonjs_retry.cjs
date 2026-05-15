const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-extensionless-cjs-retry-'));

function writeExecutable(name, lines) {
  const file = path.join(tmpRoot, name);
  fs.writeFileSync(file, [...lines, ''].join('\n'));
  fs.chmodSync(file, 0o755);
  return file;
}

function run(file) {
  const result = spawnSync(process.execPath, [file], { encoding: 'utf8' });
  if (result.error) throw result.error;
  return result;
}

const requireWrapper = writeExecutable('require-wrapper', [
  '#!/usr/bin/env node',
  'const path = require("node:path");',
  'console.log("require:" + path.basename(__filename));',
]);

const moduleExportsWrapper = writeExecutable('module-exports-wrapper', [
  '#!/usr/bin/env node',
  'module.exports.answer = 42;',
  'console.log("module.exports:" + module.exports.answer);',
]);

const esmImport = writeExecutable('esm-import', [
  '#!/usr/bin/env node',
  'import path from "node:path";',
  'console.log("esm:" + path.basename(import.meta.url));',
]);

const localRequire = writeExecutable('local-require', [
  '#!/usr/bin/env node',
  'const require = async () => "local";',
  'console.log(await require());',
]);

const requireResult = run(requireWrapper);
assert(
  requireResult.status === 0,
  `expected extensionless free require() wrapper to retry as CommonJS, got ${requireResult.status}\nstdout:\n${requireResult.stdout}\nstderr:\n${requireResult.stderr}`
);
assert(requireResult.stdout === 'require:require-wrapper\n', `unexpected require stdout: ${JSON.stringify(requireResult.stdout)}`);

const moduleExportsResult = run(moduleExportsWrapper);
assert(
  moduleExportsResult.status === 0,
  `expected extensionless module.exports wrapper to retry as CommonJS, got ${moduleExportsResult.status}\nstdout:\n${moduleExportsResult.stdout}\nstderr:\n${moduleExportsResult.stderr}`
);
assert(moduleExportsResult.stdout === 'module.exports:42\n', `unexpected module.exports stdout: ${JSON.stringify(moduleExportsResult.stdout)}`);

const esmResult = run(esmImport);
assert(
  esmResult.status === 0,
  `expected extensionless import entrypoint to remain ESM, got ${esmResult.status}\nstdout:\n${esmResult.stdout}\nstderr:\n${esmResult.stderr}`
);
assert(esmResult.stdout === 'esm:esm-import\n', `unexpected ESM stdout: ${JSON.stringify(esmResult.stdout)}`);

const localRequireResult = run(localRequire);
assert(
  localRequireResult.status === 0,
  `expected locally-bound require entrypoint to remain ESM, got ${localRequireResult.status}\nstdout:\n${localRequireResult.stdout}\nstderr:\n${localRequireResult.stderr}`
);
assert(localRequireResult.stdout === 'local\n', `unexpected local require stdout: ${JSON.stringify(localRequireResult.stdout)}`);

console.log('extensionless CommonJS compile retry works');
