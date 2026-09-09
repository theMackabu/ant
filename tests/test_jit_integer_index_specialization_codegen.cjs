const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const { join } = require('node:path');
const child = spawnSync(process.execPath, [join(__dirname, 'test_jit_integer_index_specialization.cjs')], {
  encoding: 'utf8',
  env: { ...process.env, ANT_DEBUG: 'dump/vm:jit' },
  maxBuffer: 32 * 1024 * 1024,
  timeout: 30000,
});
assert.strictEqual(child.status, 0, String(child.error || child.stderr));
assert.match(child.stdout, /PASS integer indices/);
for (const name of ['read', 'write', 'unsignedRead', 'unsignedWrite']) {
  const mirModule = child.stderr.match(new RegExp('jit_' + name + '_[^\\n]*:\\s*module[\\s\\S]*?endmodule'));
  assert.ok(mirModule, 'missing compilation for ' + name);
  assert.match(mirModule[0], /integer_index_/, 'keep the integer representation for ' + name);
  assert.doesNotMatch(mirModule[0], /\bi2d\s+spec_rt_/, 'no repeated integer exactness check for ' + name);
  const indexGuard = mirModule[0].match(
    /\bmov\s+(integer_index_\d+),\s*(s\d+)[\s\S]*?\bubge\s+(L\d+),\s*\1,/);
  assert.ok(indexGuard, 'missing integer index guard for ' + name);
  const bailoutStart = mirModule[0].indexOf('\n' + indexGuard[3] + ':', indexGuard.index);
  assert.ok(bailoutStart > indexGuard.index, 'missing index bailout for ' + name);
  const fastPath = mirModule[0].slice(indexGuard.index, bailoutStart);
  assert.doesNotMatch(fastPath, new RegExp('\\bi2d\\s+\\w+,\\s*' + indexGuard[2] + '\\b'),
    'box the integer key only on bailout for ' + name);
}
console.log('PASS specialized array access preserves integer indices');
