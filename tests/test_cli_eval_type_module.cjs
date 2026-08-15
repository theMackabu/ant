const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const requireResult = spawnSync(process.execPath, [
  '-e',
  "console.log(typeof require('ant:shell'));",
], { encoding: 'utf8' });

if (requireResult.error) throw requireResult.error;

assert.equal(requireResult.status, 0, requireResult.stderr);
assert.equal(requireResult.stdout.trim(), 'object');

const defaultResult = spawnSync(process.execPath, [
  '-e',
  "import { spawnSync } from 'node:child_process'; console.log(typeof spawnSync);",
], { encoding: 'utf8' });

if (defaultResult.error) throw defaultResult.error;

assert.notEqual(defaultResult.status, 0, 'default -e should reject static import');
assert.match(defaultResult.stderr, /Cannot use import\/export syntax (?:outside a module|in CommonJS)/);

const moduleResult = spawnSync(process.execPath, [
  '--input-type=module',
  '-e',
  "import { spawnSync } from 'node:child_process'; console.log(typeof spawnSync);",
], { encoding: 'utf8' });

if (moduleResult.error) throw moduleResult.error;

assert.equal(moduleResult.status, 0, moduleResult.stderr);
assert.equal(moduleResult.stdout.trim(), 'function');

const exportResult = spawnSync(process.execPath, [
  '--input-type=module',
  '-e',
  "export const value = 42; console.log('export-ok');",
], { encoding: 'utf8' });

if (exportResult.error) throw exportResult.error;

assert.equal(exportResult.status, 0, exportResult.stderr);
assert.equal(exportResult.stdout.trim(), 'export-ok');

console.log('cli eval type module');
