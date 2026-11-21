const assert = require('node:assert');

assert.deepStrictEqual(Reflect.ownKeys(class {}), ['length', 'name', 'prototype']);
assert.deepStrictEqual(Reflect.ownKeys(class Named { static m() {} }), ['length', 'name', 'prototype', 'm']);
assert.strictEqual((class {}).name, '');

console.log('ok');
