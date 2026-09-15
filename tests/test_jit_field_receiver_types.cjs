const assert = require('node:assert');

function fromObject(value) { return value.receiverTagField; }
function fromArray(value) { return value.receiverTagField; }
function fromFunction(value) { return value.receiverTagField; }
function fromPromise(value) { return value.receiverTagField; }

const object = { receiverTagField: 10 };
const array = [];
array.receiverTagField = 20;
const fn = function receiver() {};
fn.receiverTagField = 30;
const promise = Promise.resolve(0);
promise.receiverTagField = 40;
const receivers = [object, array, fn, promise];
const readers = [fromObject, fromArray, fromFunction, fromPromise];
for (let i = 0; i < readers.length; i++) {
  for (let n = 0; n < 500; n++) assert.strictEqual(readers[i](receivers[i]), (i + 1) * 10);
}

let getterCalls = 0;
const getter = { get receiverTagField() { getterCalls++; return 50; } };
for (let n = 0; n < 100; n++) {
  for (const read of readers) {
    for (let i = 0; i < receivers.length; i++)
      assert.strictEqual(read(receivers[i]), (i + 1) * 10);
    assert.strictEqual(read(Object.create(object)), 10);
    assert.strictEqual(read(getter), 50);
    assert.strictEqual(read({}), undefined);
    assert.strictEqual(read(1), undefined);
    assert.strictEqual(read('x'), undefined);
    assert.throws(() => read(null), TypeError);
    assert.throws(() => read(undefined), TypeError);
  }
}
assert.strictEqual(getterCalls, 400);
console.log('PASS guarded field receiver types');
