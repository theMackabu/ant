const assert = require('assert');

const key = 'a\0b';

const mapClone = structuredClone(new Map([[key, 42]]));
assert.strictEqual(mapClone.size, 1);
assert.strictEqual(mapClone.has(key), true);
assert.strictEqual(mapClone.get(key), 42);

const setClone = structuredClone(new Set([key]));
assert.strictEqual(setClone.size, 1);
assert.strictEqual(setClone.has(key), true);
