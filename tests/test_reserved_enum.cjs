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
  'var enum = 1', 'enum: 1', 'function enum() {}', 'function f(enum) {}', 'return typeof enum',
  'var o = { enum }', 'class enum {}', 'let enum', 'enum = 1', '(enum) => 1', 'try {} catch (enum) {}',
]) assert.strictEqual(parses(source), false, source);
assert.strictEqual(run('var o = { enum: 1 }; return o.enum'), 1);
assert.strictEqual(run('var x = { enum() { return 2 } }; return x.enum()'), 2);
assert.strictEqual(run('class C { static enum = 3 } return C.enum'), 3);

console.log('ok');
