const assert = require('node:assert');

class AsyncRegularFunctionTest {
  constructor(name) {
    this.name = name;
    this.value = 100;
  }

  withRegularFunction() {
    return Promise.resolve(42).then(function (val) {
      return { self: this, val: val + 1 };
    });
  }

  withArrowFunction() {
    return Promise.resolve(42).then((val) => ({ self: this, val: val + 1 }));
  }

  withCapturedSelf() {
    const self = this;
    return Promise.resolve(1).then(function () {
      return { self: this, value: self.value };
    });
  }
}

const obj = new AsyncRegularFunctionTest('TestObject');

(async () => {
  const regular = await obj.withRegularFunction();
  assert.strictEqual(regular.self, undefined);
  assert.strictEqual(regular.val, 43);

  const arrow = await obj.withArrowFunction();
  assert.strictEqual(arrow.self, obj);
  assert.strictEqual(arrow.val, 43);

  const captured = await obj.withCapturedSelf();
  assert.strictEqual(captured.self, undefined);
  assert.strictEqual(captured.value, 100);

  console.log('async regular function this ok');
})();
