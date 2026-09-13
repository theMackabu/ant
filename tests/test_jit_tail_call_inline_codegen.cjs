const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const { join } = require('node:path');

const child = spawnSync(process.execPath, [join(__dirname, 'test_jit_tail_call_inline.cjs')], {
  encoding: 'utf8',
  env: { ...process.env, ANT_DEBUG: 'dump/vm:jit' },
  maxBuffer: 32 * 1024 * 1024,
  timeout: 30000,
});
assert.strictEqual(child.status, 0, String(child.error || child.stderr));
assert.match(child.stdout, /PASS tail call inlining/);

function compiled(name) {
  const module = child.stderr.match(new RegExp(`jit_${name}_[^\\n]*:\\s*module[\\s\\S]*?endmodule`));
  assert.ok(module, `${name}: missing compilation`);
  return module[0];
}

assert.match(compiled('tailCall'), /\bbne\s+L\d+,\s*inl\d+_gf,/,
  'direct tail call must emit the inline target guard');
assert.match(compiled('tailMethod'), /\bbne\s+L\d+,\s*mi\d+_fn,/,
  'method tail call must emit the inline target guard');
assert.doesNotMatch(compiled('selfTail'), /\bbne\s+L\d+,\s*inl\d+_gf,/,
  'self-tail calls must retain frame reuse');
assert.match(compiled('tailUpdateRead'), /\bcall\s+[^\n]*jit_helper_get_field,/,
  'reads after a store must use a helper that cannot replay the callee');
assert.match(compiled('tailUpdateLength'), /\bcall\s+[^\n]*jit_helper_get_length,/,
  'length reads after a store must use a helper that cannot replay the callee');
console.log('PASS tail call inline code generation');
