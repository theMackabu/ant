'use strict';
const assert = require('node:assert');
function optional(object) { return object?.laterMissing; }
function readLoop(object, n) {
  let missing = 0;
  for (let i = 0; i < n; i++) if (optional(object) === undefined) missing++;
  return missing;
}
// Compile before this site has any object feedback. It must subsequently
// handle changing IC kinds without assuming the first receiver/prototype.
for (let i = 0; i < 200; i++) assert.strictEqual(readLoop(null, 100), 100);
for (const prototype of [null, {}, function callable() {}, []]) {
  const object = Object.create(prototype);
  for (let i = 0; i < 1000; i++) assert.strictEqual(readLoop(object, 100), 100);
  object.laterMissing = 7;
  assert.strictEqual(readLoop(object, 100), 0);
  delete object.laterMissing;
  assert.strictEqual(readLoop(object, 100), 100);
  if (prototype !== null) {
    prototype.laterMissing = 8;
    assert.strictEqual(readLoop(object, 100), 0);
    delete prototype.laterMissing;
    assert.strictEqual(readLoop(object, 100), 100);
  }
  let gets = 0;
  Object.defineProperty(object, 'laterMissing', { configurable: true, get() { gets++; return undefined; } });
  assert.strictEqual(readLoop(object, 100), 100);
  assert.strictEqual(gets, 100, 'getters must not be mistaken for cached absence');
}
const object = {};
assert.strictEqual(readLoop(object, 1000), 1000);
let proxyGets = 0;
Object.setPrototypeOf(object, new Proxy({}, { get(target, key) {
  if (key === 'laterMissing') { proxyGets++; return 9; }
  return Reflect.get(target, key);
} }));
assert.strictEqual(readLoop(object, 100), 0);
assert.strictEqual(proxyGets, 100);
assert.strictEqual(readLoop(undefined, 100), 100);
console.log('PASS missing IC adapts after nullish warmup');
