const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const { join } = require('node:path');
const child = spawnSync(process.execPath, [join(__dirname, 'test_jit_wide_word32_feedback.cjs')], {
  encoding: 'utf8',
  env: { ...process.env, ANT_DEBUG: 'dump/vm:jit opt:off' },
  maxBuffer: 64 * 1024 * 1024,
  timeout: 30000,
});
assert.strictEqual(child.status, 0, String(child.error || child.stderr));
assert.match(child.stdout, /PASS wide word32 feedback/);

const helper = /\bcall\s+helper[12]_proto, jit_helper_(?:band|bor|bxor|shl|shr|ushr|bnot),/;
function module(name) {
  const found = child.stderr.match(new RegExp(`jit_${name}_[^\\n]*:\\s*module[\\s\\S]*?endmodule`));
  assert.ok(found, `missing ${name} compilation`);
  return found[0];
}

const wide = module('wideMix');
assert.match(wide, /\bd2i\s+word_integer_/);
assert.doesNotMatch(wide, helper, 'operands below 2^63 should use the native word path');
assert.doesNotMatch(module('djb2'), helper, 'hash wraparound should use the native word path');
assert.match(module('hugeMix'), helper, 'operands of 2^63 and above must keep the helper path');
console.log('PASS wide word32 feedback code generation');
