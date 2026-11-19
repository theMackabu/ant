const assert = require('node:assert');

assert.throws(() => new Function('()'), SyntaxError);
assert.throws(() => new Function('var a = ()'), SyntaxError);
assert.strictEqual(new Function('return (() => 1)()')(), 1);

console.log('ok');
