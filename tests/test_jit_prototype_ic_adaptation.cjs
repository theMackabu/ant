const assert = require('node:assert');

function methodA() { return this.value + 1; }
function methodB() { return this.value + 2; }
function methodC() { return this.value + 3; }
const ordinary = { method: methodA };
function callable() {}
callable.method = methodB;
const array = [];
array.method = methodC;
const receivers = [ordinary, callable, array].map(proto => {
  const receiver = Object.create(proto);
  receiver.value = 10;
  return receiver;
});

function readMethod(receiver) { return receiver.method; }
function invokeMethod(receiver) { return receiver.method(); }
for (let i = 0; i < 300; i++) {
  assert.strictEqual(readMethod(receivers[0]), methodA);
  assert.strictEqual(invokeMethod(receivers[0]), 11);
}
for (let round = 0; round < 30; round++) {
  const index = round % 3;
  const expected = [methodA, methodB, methodC][index];
  for (let i = 0; i < 100; i++) {
    assert.strictEqual(readMethod(receivers[index]), expected);
    assert.strictEqual(invokeMethod(receivers[index]), 11 + index);
  }
}

callable.method = methodC;
assert.strictEqual(invokeMethod(receivers[1]), 13);
Object.setPrototypeOf(receivers[1], ordinary);
assert.strictEqual(invokeMethod(receivers[1]), 11);
receivers[1].method = methodB;
assert.strictEqual(invokeMethod(receivers[1]), 12);
delete receivers[1].method;
assert.strictEqual(invokeMethod(receivers[1]), 11);
let reads = 0;
Object.defineProperty(ordinary, 'method', {
  configurable: true,
  get() { reads++; return methodC; }
});
assert.strictEqual(readMethod(receivers[0]), methodC);
assert.strictEqual(invokeMethod(receivers[0]), 13);
assert.strictEqual(reads, 2);
console.log('PASS IC adapts across ordinary, callable and array prototypes and mutations');
