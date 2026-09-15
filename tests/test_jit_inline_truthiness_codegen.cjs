const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const { join } = require('node:path');

const child = spawnSync(process.execPath, [join(__dirname, 'test_jit_inline_truthiness.cjs')], {
  encoding: 'utf8',
  env: { ...process.env, ANT_DEBUG: 'dump/vm:jit' },
  maxBuffer: 32 * 1024 * 1024,
  timeout: 30000,
});
assert.strictEqual(child.status, 0, String(child.error || child.stderr));
assert.match(child.stdout, /PASS inline truthiness/);

for (const name of ['ChooseCount', 'ChooseNotCount', 'NegateCount', 'OrCount', 'AndCount']) {
  const module = child.stderr.match(new RegExp(`jit_call${name}_[^\\n]*:\\s*module[\\s\\S]*?endmodule`));
  assert.ok(module, `missing inline caller compilation: ${name}`);
  const dispatch = module[0].match(new RegExp([
    String.raw`\bursh\s+(\w+),\s*(inl\d+_s\d+),\s*47`,
    String.raw`beq\s+(L\d+),\s*\1,\s*131040`,
    String.raw`uble\s+(L\d+),\s*\2,\s*18442240474082181120`,
    String.raw`sub\s+\1,\s*\1,\s*131042`,
    String.raw`uble\s+\3,\s*\1,\s*4`,
    String.raw`beq\s+(L\d+),\s*\1,\s*-1`,
    String.raw`beq\s+\3,\s*\1,\s*10`,
  ].join(String.raw`\s*\n\s*`)));
  assert.ok(dispatch, `missing Object/Array range, String/Symbol shortcuts, and Number guard: ${name}`);
  const objectStart = module[0].indexOf(`${dispatch[5]}:`);
  const numericStart = module[0].indexOf(`${dispatch[4]}:`);
  assert.ok(objectStart >= 0 && numericStart > objectStart, 'missing separate object/string block');
  const object = module[0].slice(objectStart, numericStart);
  assert.doesNotMatch(object, /\bcall\b/, `objects and strings must bypass helpers: ${name}`);
  assert.match(object, /\band\s+\w+,\s*inl\d+_s\d+,\s*140737488355324/, 'strip cage and representation tags');
  assert.match(object, /\badd\s+\w+,\s*\w+,\s*cage_base/, 'decode the non-null string offset');
  const numeric = module[0].slice(numericStart).match(/^L\d+:\s*\n([\s\S]*?)(?=\n\s*L\d+:)/)?.[1];
  assert.ok(numeric, 'missing distinct numeric block');
  assert.doesNotMatch(numeric, /\bcall\b/, `numbers must bypass helpers: ${name}`);
  assert.match(numeric, /\blsh\s+\w+,\s*inl\d+_s\d+,\s*1/, 'handle either sign');
  assert.match(numeric, /\bbeq\s+L\d+,\s*\w+,\s*0/, 'handle both zeros');
  assert.match(numeric, /\bubgt\s+L\d+,\s*\w+,\s*18437736874454810624/, 'exclude NaN, retain infinities');
  assert.match(numeric, /\bjmp\s+L\d+\s*$/, 'numbers must not fall into helper handling');
}
for (const name of ['chooseWithBackedge', 'negateWithBackedge']) {
  const module = child.stderr.match(new RegExp(`jit_${name}_[^\\n]*:\\s*module[\\s\\S]*?endmodule`));
  assert.ok(module, `missing non-inlined compilation: ${name}`);
  assert.match(module[0], /\blsh\s+\w+,\s*\w+,\s*1/, 'numeric truthiness uses integer bits');
  assert.match(module[0], /\bubgt\s+L\d+,\s*\w+,\s*18437736874454810624/, 'numeric NaN guard');
  assert.match(module[0], /\band\s+\w+,\s*\w+,\s*140737488355324/, 'inline string offset');
  assert.doesNotMatch(module[0], /\bcall\s+[^\n]*jit_helper_not/, 'negation shares inline truthiness');
  assert.doesNotMatch(module[0], /\b(?:dne|deq)\s+cond_/, 'truthiness needs no FP temporary slots');
}
console.log('PASS inline and ordinary truthiness handle objects, strings, Symbols, and numbers without helpers');
