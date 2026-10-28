const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const splay = fs.readFileSync(path.join(__dirname, '../examples/bench-v8/tests/splay.js'), 'utf8');
const body = splay
  .slice(splay.indexOf('// Configuration.'), splay.indexOf('var Splay = new BenchmarkSuite'))
  .replace('var kSplayTreeSize = 8000;', 'var kSplayTreeSize = 2000;')
  .replace('var kSplayTreePayloadDepth = 5;', 'var kSplayTreePayloadDepth = 3;');

const source = `'use strict';
let seed = 49734321;
Math.random = function () {
  seed = ((seed + 0x7ed55d16) + (seed << 12)) & 0xffffffff;
  seed = ((seed ^ 0xc761c23c) ^ (seed >>> 19)) & 0xffffffff;
  seed = ((seed + 0x165667b1) + (seed << 5)) & 0xffffffff;
  seed = ((seed + 0xd3a2646c) ^ (seed << 9)) & 0xffffffff;
  seed = ((seed + 0xfd7046c5) + (seed << 3)) & 0xffffffff;
  seed = ((seed ^ 0xb55a4f09) ^ (seed >>> 16)) & 0xffffffff;
  return (seed & 0xfffffff) / 0x10000000;
};
${body}
SplaySetup();
let peak = 0;
for (let i = 0; i < 6000; i++) {
  SplayRun();
  if (i % 500 === 0) peak = Math.max(peak, Ant.stats().alloc.objectCount);
}
SplayTearDown();
console.log('peak objects', peak);
`;

const result = spawnSync(process.execPath, ['-e', source], { encoding: 'utf8', timeout: 120000, env: { ...process.env, NO_COLOR: '1' } });
if (result.error) throw result.error;
assert.strictEqual(result.status, 0, result.stdout + result.stderr);
const peak = Number(/peak objects (\d+)/.exec(result.stdout)?.[1]);
assert.ok(peak > 0, result.stdout);
assert.ok(peak < 2000000, `removed splay nodes stay reachable from stale stack words of a long-running loop: ${peak} objects`);
console.log('gc-osr-stale-stack: ok', peak);
