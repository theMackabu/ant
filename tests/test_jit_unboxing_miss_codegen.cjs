const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const { join } = require('node:path');

function compile(fixture) {
  const child = spawnSync(process.execPath, [join(__dirname, fixture)], {
    encoding: 'utf8',
    env: { ...process.env, ANT_DEBUG: 'dump/vm:jit' },
    maxBuffer: 32 * 1024 * 1024,
    timeout: 30000,
  });
  assert.strictEqual(child.status, 0, String(child.error || child.stderr));
  return name => {
    const modules = child.stderr.match(new RegExp(`jit_${name}_[^\\n]*:\\s*module[\\s\\S]*?endmodule`, 'g'));
    assert.ok(modules && modules.length, `${name}: missing compilation`);
    return modules.join('\n');
  };
}

const fields = compile('test_jit_missing_field_ic.cjs');
for (const name of ['missingField', 'loopMissingField']) {
  // test_jit_property_ic.c checks the immediate against the C enum directly.
  assert.match(fields(name), /\bbne\s+L\d+,\s*gf_miss_kind_\S+,\s*\d+\b/,
    `${name}: missing reads must guard the live missing-handler kind`);
  assert.doesNotMatch(fields(name), /\bgf_(?:idx|h|ovf)_/,
    `${name}: specialized missing reads must not generate property-slot loads`);
}

const updates = compile('test_jit_post_inc_numeric.cjs');
assert.match(updates('countPostIncrement'), /\bdadd\s+sd\d+,\s*sd\d+,\s*d_one/,
  'post-increment must produce an unboxed numeric stack result');

const options = compile('test_jit_inline_options.cjs');
for (const name of ['optionsDriver', 'localDriver', 'conditionalDriver']) {
  assert.match(options(name), /\bbne\s+L\d+,\s*inl\d+_gf,/,
    `${name}: options handling must inline`);
  assert.match(options(name), /inl\d+_empty_site\d+/,
    `${name}: read-only empty objects must avoid repeated allocation`);
}
assert.match(options('optionsDriver'), /inl\d+_arg1/,
  'written parameters need private inline storage');
assert.doesNotMatch(options('escaping'), /inl\d+_empty_site\d+/,
  'escaping object allocations must retain identity');
assert.doesNotMatch(options('storedDriver'), /inl\d+_empty_site\d+/,
  'stored object allocations must retain identity');
console.log('PASS unboxing and missing-property code generation');
