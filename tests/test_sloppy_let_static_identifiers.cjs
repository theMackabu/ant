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

assert.strictEqual(run('var let = 4; return let'), 4);
assert.strictEqual(run('var let = { x: 3 }; return let.x'), 3);
assert.strictEqual(run('var static = 1; return static + 1'), 2);
assert.strictEqual(run('var let = 2; return [let, typeof let, let * 2]').join(), '2,number,4');
for (const source of ['"use strict"; var let = 1', '"use strict"; var static = 1', '"use strict"; return let', '"use strict"; return static', 'let let = 1', 'const let = 1'])
  assert.strictEqual(parses(source), false, source);

assert.strictEqual(parses("'use strict'; let: 1"), false);
assert.strictEqual(parses("'use strict'; static: 1"), false);

console.log('ok');
