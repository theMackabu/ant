const assert = require('node:assert');

for (const source of [
  'for (let [x];;) ;', 'for (var [x];;) ;',
]) assert.throws(() => new Function(source), SyntaxError, source);
for (const source of [
  'for (let [x] = [1];;) break;', 'for (let [x] of [[1]]) ;',
]) assert.doesNotThrow(() => new Function(source), source);

console.log('ok');
