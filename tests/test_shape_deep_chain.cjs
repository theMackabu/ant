const assert = require('node:assert');

function target() {}
const a = function () {};
const b = function () {};
for (let i = 0; i < 1000000; i++) target.__proto__ = i & 1 ? a : b;
assert.strictEqual(Object.getPrototypeOf(target), a);

const garbage = [];
for (let i = 0; i < 200000; i++) garbage.push({ i });
assert.strictEqual(garbage.length, 200000);

console.log('ok');
