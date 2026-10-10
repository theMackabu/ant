const assert = require('node:assert');

function sloppyCallback() { return this; }
function strictCallback() { 'use strict'; return this; }

class TestClass {
  constructor(name) { this.name = name; }
  methodThatReturnsPromise() {
    return Promise.resolve('value').then(function (val) { return { self: this, val }; });
  }
}

(async () => {
  assert.strictEqual(await Promise.resolve().then(sloppyCallback), globalThis);
  assert.strictEqual(await Promise.resolve().then(strictCallback), undefined);

  const obj1 = new TestClass('Object1');
  const obj2 = new TestClass('Object2');
  for (const obj of [obj1, obj2]) {
    const result = await obj.methodThatReturnsPromise();
    assert.strictEqual(result.self, undefined);
    assert.strictEqual(result.val, 'value');
  }

  const bound = await Promise.resolve().then(sloppyCallback.bind(obj1));
  assert.strictEqual(bound, obj1);

  console.log('async this binding ok');
})();
