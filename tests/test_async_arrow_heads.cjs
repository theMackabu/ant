const assert = require('node:assert');

for (const source of [
  'async ()\n=> 1', 'async (x)\n=> x', 'async (x = await) => 1', 'async (await) => 1',
]) assert.throws(() => new Function(source), SyntaxError, source);
for (const source of [
  'async () => 1', 'async (x = 1) => x', 'var async; async\n(1)',
]) assert.doesNotThrow(() => new Function(source), source);

console.log('ok');
