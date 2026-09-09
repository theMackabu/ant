const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const { join } = require('node:path');

const child = spawnSync(process.execPath, [join(__dirname, 'test_jit_forward_arguments.cjs')], {
  encoding: 'utf8',
  env: { ...process.env, ANT_DEBUG: 'dump/vm:jit' },
  maxBuffer: 32 * 1024 * 1024,
  timeout: 30000,
});
assert.strictEqual(child.status, 0, String(child.error || child.stderr));
assert.match(child.stdout, /PASS guarded argument forwarding/);

function compiled(name) {
  const module = child.stderr.match(new RegExp(`jit_${name}_[^\\n]*:\\s*module[\\s\\S]*?endmodule`));
  assert.ok(module, `${name}: missing JIT compilation`);
  return module[0];
}

for (const name of ['forward', 'Construct']) {
  const mir = compiled(name);
  // Imports exist in every module: require an emitted call, not just the symbol.
  assert.match(mir, /\bcall\s+forward_arguments_proto,\s*jit_helper_forward_arguments,/,
    `${name}: missing guarded argument forwarding`);
  assert.match(mir, /\bcall\s+jit_proto,\s*forward_code,[^\n]*, args, argc, forward_closure/,
    `${name}: missing direct call with the incoming arguments`);
  assert.doesNotMatch(mir, /\bcall\s+(?:soj_proto|strict_arguments_proto),\s*jit_helper_(?:special_obj|strict_arguments),/,
    `${name}: eagerly materialized arguments`);
}
assert.match(compiled('forward'), /\bret\s+s0\s+L\d+:\s+ret\s+s0\b/,
  'tail forwarding must return its result after the error guard');
for (const name of ['changed', 'escaped']) {
  const mir = compiled(name);
  assert.doesNotMatch(mir, /\bcall\s+forward_arguments_proto,\s*jit_helper_forward_arguments,/,
    `${name}: unsafe argument forwarding`);
  assert.match(mir, /\bcall\s+strict_arguments_proto,\s*jit_helper_strict_arguments,/,
    `${name}: arguments must be materialized`);
}
console.log('PASS argument forwarding code generation');
