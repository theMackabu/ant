const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const body = `
  let local = value + 1;
  const closure = () => value + local;
  if (value < 0) throw closure;
  return closure;
`;
const source = `
function first(value) { ${body} }
function second(value) { ${body} }
for (const factory of [first, second]) {
  for (let i = 0; i < 400; i++) {
    const value = i % 2 ? -2 : 2;
    let closure;
    try { closure = factory(value); } catch (error) { closure = error; }
    if (closure() !== value * 2 + 1) throw new Error('captured value mismatch');
  }
}
console.log('closures ok');
`;
const child = spawnSync(process.execPath, ['-e', source], {
  encoding: 'utf8',
  env: { ...process.env, ANT_DEBUG: 'dump/vm:jit' },
  maxBuffer: 8 * 1024 * 1024,
  timeout: 10000,
});
assert.strictEqual(child.status, 0, child.stderr);
assert.match(child.stdout, /closures ok/);

function sites(name) {
  const module = child.stderr.match(new RegExp(`jit_${name}_[^\\n]*:\\s*module[\\s\\S]*?endmodule`));
  assert.ok(module, `missing ${name} compilation`);
  const names = Array.from(module[0].matchAll(/i64:(open_upvals_\d+)/g), match => match[1]);
  assert.ok(names.length >= 4, `${name}: missing parameter/local close guards for return and throw`);
  assert.strictEqual(new Set(names).size, names.length, `${name}: duplicate close guard register`);
  return names;
}

assert.deepStrictEqual(sites('first'), sites('second'), 'close guard naming must be local to each compilation');
console.log('jit close guard sites: ok');
