import assert from 'node:assert';
import { createRequire } from 'node:module';
import fsDefault, { readFileSync } from 'node:fs';
import * as fsStar from 'node:fs';

const require = createRequire(import.meta.url);

for (const name of ['fs', 'os', 'crypto', 'util', 'fs/promises', 'worker_threads', 'module', 'path', 'events', 'zlib']) {
  const ns = await import(`node:${name}`);
  const cjs = require(`node:${name}`);
  assert.strictEqual(ns.default, cjs, name);
  for (const key of Object.keys(cjs).filter((k) => k !== 'default').slice(0, 5)) assert.ok(key in ns, `${name}.${key}`);
}

assert.strictEqual(fsDefault, require('fs'));
assert.strictEqual(fsStar.default, require('fs'));
assert.strictEqual(typeof readFileSync, 'function');
assert.ok(!Object.keys(require('fs')).includes('default'));
assert.strictEqual(await import('node:fs'), await import('fs'));
console.log('dynamic import builtin default: ok');
