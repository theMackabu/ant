const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const { join } = require('node:path');
const child = spawnSync(process.execPath, [join(__dirname, 'test_jit_fractional_bitwise.cjs')], {
  encoding: 'utf8',
  env: { ...process.env, ANT_DEBUG: 'dump/vm:jit' },
  maxBuffer: 32 * 1024 * 1024,
  timeout: 30000,
});
assert.strictEqual(child.status, 0, String(child.error || child.stderr));
assert.match(child.stdout, /PASS fractional bitwise conversion/);
const mirModule = child.stderr.match(/jit_bitwise_[^\n]*:\s*module[\s\S]*?endmodule/);
assert.ok(mirModule, 'missing bitwise compilation');
assert.match(mirModule[0], /\bd2i\s+spec_i_/);
assert.doesNotMatch(mirModule[0], /\bcall\s+helper[12]_proto, jit_helper_(?:band|bor|bxor|shl|shr|ushr|bnot),/,
  'fractional operands should use guarded native conversion');
assert.doesNotMatch(mirModule[0], /\bi2d\s+spec_rt_/, 'bitwise conversion truncates without an exactness check');
console.log('PASS fractional bitwise code generation');
