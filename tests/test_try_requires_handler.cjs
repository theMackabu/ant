const assert = require('node:assert');

for (const source of [
  'try {}',
]) assert.throws(() => new Function(source), SyntaxError, source);
for (const source of [
  'try {} catch {}', 'try {} finally {}',
]) assert.doesNotThrow(() => new Function(source), source);

console.log('ok');
