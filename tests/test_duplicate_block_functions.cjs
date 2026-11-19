const assert = require('node:assert');

for (const source of [
  '{ async function f() {} async function f() {} }', '{ function* g() {} function* g() {} }', '{ async function f() {} function f() {} }', 'switch (1) { case 1: async function f() {} async function f() {} }',
]) assert.throws(() => new Function(source), SyntaxError, source);
for (const source of [
  '{ function f() {} function f() {} }', 'function* g() {} function* g() {}',
]) assert.doesNotThrow(() => new Function(source), source);

console.log('ok');
