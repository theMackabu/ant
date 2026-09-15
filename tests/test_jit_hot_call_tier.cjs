'use strict';
const assert = require('node:assert');

if (process.argv[2] === 'child' || typeof Ant === 'undefined') {
  // arguments keeps this straight-line callee out of the inliner.
  function repeatedlyCalledLeaf(object) {
    return arguments[0].value + 1;
  }
  const object = { value: 3 };
  for (let i = 0; i < 2000; i++) assert.strictEqual(repeatedlyCalledLeaf(object), 4);
  let gets = 0;
  Object.defineProperty(object, 'value', { configurable: true, get() { gets++; return 4.5; } });
  for (let i = 0; i < 100; i++) assert.strictEqual(repeatedlyCalledLeaf(object), 5.5);
  assert.strictEqual(gets, 100, 'optimized calls must not replay getters');
  const error = new Error('getter failure');
  Object.defineProperty(object, 'value', { get() { throw error; } });
  assert.throws(() => repeatedlyCalledLeaf(object), thrown => thrown === error);
  console.log('PASS repeated-call values, getters and exceptions');
} else {
  const { spawnSync } = require('node:child_process');
  const child = spawnSync(process.execPath, [__filename, 'child'], {
    encoding: 'utf8', env: { ...process.env, ANT_DEBUG: 'dump/vm:op-warn' },
    timeout: 30000, maxBuffer: 4 * 1024 * 1024,
  });
  assert.strictEqual(child.status, 0, String(child.error || child.stderr));
  assert.match(child.stdout, /PASS repeated-call/);
  assert.match(child.stderr, /jit: compiled func=repeatedlyCalledLeaf .*tier=cheap\b/,
    'call frequency alone must not select the optimizing tier');
  console.log('PASS call-frequency cheap tier');
}
