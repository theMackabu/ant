const assert = require('node:assert');
const { inspect } = require('node:util');

const content = (promise) => inspect(promise).replace(/,?\s*Symbol\([a-z_]+\): \d+/g, '').replace(/\s+/g, ' ');

const rejected = Promise.reject('error message');
rejected.catch(() => {});

let resolvePending;
const pending = new Promise((resolve) => { resolvePending = resolve; });

const cases = [
  [Promise.resolve(42), 'Promise { 42 }'],
  [Promise.resolve('hello'), "Promise { 'hello' }"],
  [Promise.resolve(true), 'Promise { true }'],
  [Promise.resolve(false), 'Promise { false }'],
  [Promise.resolve(null), 'Promise { null }'],
  [Promise.resolve(undefined), 'Promise { undefined }'],
  [Promise.resolve({ x: 1, y: 2 }), 'Promise { { x: 1, y: 2 } }'],
  [Promise.resolve([1, 2, 3]), 'Promise { [ 1, 2, 3 ] }'],
  [rejected, "Promise { <rejected> 'error message' }"],
  [pending, 'Promise { <pending> }'],
  [(async () => 999)(), 'Promise { 999 }'],
  [(async () => ({ value: 42, name: 'test' }))(), "Promise { { value: 42, name: 'test' } }"],
  [(async () => [10, 20, 30])(), 'Promise { [ 10, 20, 30 ] }'],
];

for (const [promise, expected] of cases) {
  assert.strictEqual(String(promise), '[object Promise]');
  assert.strictEqual(`${promise}`, '[object Promise]');
  assert.strictEqual(content(promise), expected);
}

(async () => {
  const inner = await Promise.resolve(5).then((v) => {
    const next = Promise.resolve(v * 2);
    assert.strictEqual(String(next), '[object Promise]');
    return next;
  });
  assert.strictEqual(inner, 10);

  resolvePending(100);
  await pending;
  assert.strictEqual(content(pending), 'Promise { 100 }');
  console.log('promise display ok');
})();
