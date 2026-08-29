const assert = require('assert');

const key = 'a\0b';

const mapClone = structuredClone(new Map([[key, 42]]));
assert.strictEqual(mapClone.size, 1);
assert.strictEqual(mapClone.has(key), true);
assert.strictEqual(mapClone.get(key), 42);

const setClone = structuredClone(new Set([key]));
assert.strictEqual(setClone.size, 1);
assert.strictEqual(setClone.has(key), true);

const objectKey = { id: 1 };
const objectMapClone = structuredClone(new Map([[objectKey, 42]]));
const clonedObjectKey = [...objectMapClone.keys()][0];
assert.notStrictEqual(clonedObjectKey, objectKey);
assert.deepStrictEqual(clonedObjectKey, objectKey);
assert.strictEqual(objectMapClone.has(objectKey), false);
assert.strictEqual(objectMapClone.has(clonedObjectKey), true);
assert.strictEqual(objectMapClone.get(clonedObjectKey), 42);

const selfKeyedMap = new Map();
selfKeyedMap.set(selfKeyedMap, 'self');
const selfKeyedMapClone = structuredClone(selfKeyedMap);
assert.strictEqual(selfKeyedMapClone.has(selfKeyedMap), false);
assert.strictEqual(selfKeyedMapClone.get(selfKeyedMapClone), 'self');
