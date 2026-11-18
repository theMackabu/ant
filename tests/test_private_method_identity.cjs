const assert = require('node:assert');

const computedKeys = [];
var keyIndex = 0;
while (keyIndex < 3) computedKeys.push(class { ['k' + keyIndex] = keyIndex }), keyIndex++;
assert.deepStrictEqual(computedKeys.map((Class) => Object.keys(new Class())[0]), ['k0', 'k1', 'k2']);

console.log('ok');
