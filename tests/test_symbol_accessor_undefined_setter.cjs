const assert = require('node:assert');

const sym = Symbol('s');
const shapes = [
  { get() { return 1; }, set: undefined },
  { set: undefined },
  { get: undefined, set: undefined },
];

for (const key of ['k', sym]) {
  for (const desc of shapes) {
    const sloppy = {};
    Object.defineProperty(sloppy, key, { ...desc, configurable: true });
    const expected = sloppy[key];
    sloppy[key] = 5;
    assert.strictEqual(sloppy[key], expected);
    assert.strictEqual(Reflect.set(sloppy, key, 6), false);

    const strict = {};
    Object.defineProperty(strict, key, { ...desc, configurable: true });
    assert.throws(() => { 'use strict'; strict[key] = 5; }, TypeError);
  }
}

console.log('ok');
