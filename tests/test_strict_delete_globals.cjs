const assert = require('node:assert');

for (const source of [
  '"use strict"; delete undefined', '"use strict"; delete globalThis',
]) assert.throws(() => new Function(source), SyntaxError, source);
for (const source of [
  'delete undefined',
]) assert.doesNotThrow(() => new Function(source), source);

console.log('ok');
