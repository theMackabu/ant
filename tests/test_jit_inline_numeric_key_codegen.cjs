const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const { join } = require('node:path');

const child = spawnSync(process.execPath, [join(__dirname, 'test_jit_inline_numeric_key.cjs')], {
  encoding: 'utf8',
  env: { ...process.env, ANT_DEBUG: 'dump/vm:jit' },
  maxBuffer: 32 * 1024 * 1024,
  timeout: 30000,
});
assert.strictEqual(child.status, 0, String(child.error || child.stderr));
assert.match(child.stdout, /PASS inline numeric keys/);
const mirModule = child.stderr.match(/jit_callReadKey_[^\n]*:\s*module[\s\S]*?endmodule/);
assert.ok(mirModule, 'missing caller compilation');
const mir = mirModule[0];
assert.match(mir, /\bdmov\s+inl\d+_dt\d+,/, 'numeric argument must be inlined');
assert.match(mir, /\bcall\s+[^\n]*jit_helper_get_elem_inline,/, 'generic element fallback must exist');
assert.match(mir, /\bd2i\s+index_integer_\d+,\s*inl\d+_dt\d+\b/,
  'index guard must use the preserved double register');
assert.match(mir, /\bubgt\s+L\d+,\s*index_bits_/,
  'numeric key bits must guard the nonnegative finite index range');
assert.doesNotMatch(mir, /index_magnitude_|\bubgt\s+L\d+,\s*index_integer_/,
  'the bit range guard must replace magnitude extraction and the integer range guard');
assert.doesNotMatch(mir, /\bubgt\s+L\d+,\s*inl\d+_s\d+,/,
  'known numeric key must not repeat the number-tag guard');
assert.doesNotMatch(mir, /\bi2db\s+index_double_\d+,/,
  'known numeric key must not be unboxed again');
console.log('PASS inline numeric key code generation');
