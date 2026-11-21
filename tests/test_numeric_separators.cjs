const assert = require('node:assert');

const parses = (source) => {
  try {
    new Function(source);
    return true;
  } catch (error) {
    assert.ok(error instanceof SyntaxError, `${source}: ${error}`);
    return false;
  }
};
const run = (source) => new Function(source)();

for (const source of [
  '0_1', '1__0', '1_', '0x_1', '1_.5', '1._5', '1e_5', '0_.1', '01_0', '08_1', '0x1_', '0b_1',
  '1e+_1', '1_e1', '0_0', '010_', '0b1__0', '0o_7', '1_n', '0x_', '1.5_', '1e5_',
]) assert.strictEqual(parses(`return ${source}`), false, source);
for (const [source, value] of [
  ['1_000', 1000], ['0x1_F', 31], ['0b1_0', 2], ['0o1_7', 15], ['1e1_0', 1e10], ['1.0_1', 1.01],
  ['.1_1', 0.11], ['1_0.2_5e1_0', 10.25e10], ['0.0_0', 0], ['09.1_1', 9.11], ['1_2_3', 123], ['0xA_b_C', 0xabc],
]) assert.strictEqual(run(`return ${source}`), value, source);
assert.strictEqual(run('return 1_0n'), 10n);
assert.strictEqual(run('var _1 = 7; return _1'), 7);

console.log('ok');
