const assert = require('assert');
const fs = require('fs');

assert.strictEqual(typeof fs.Stats, 'function');
assert.strictEqual(Object.getOwnPropertyDescriptor(globalThis, 'Stats'), undefined);

globalThis.Stats = undefined;

const stats = fs.statSync(__filename);
assert.strictEqual(Object.getPrototypeOf(stats), fs.Stats.prototype);
assert.ok(stats instanceof fs.Stats);
assert.strictEqual(stats.isFile(), true);
assert.strictEqual(stats.isDirectory(), false);

console.log('fs stats export: ok');
