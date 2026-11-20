const assert = require('node:assert');

const HOT = 400;

function test(name, body) {
  try {
    body();
  } catch (error) {
    error.message = `${name}: ${error.message}`;
    throw error;
  }
}

function reshapingBase(order) {
  class A {}
  Object.defineProperty(A.prototype, 'x', { configurable: true, set(v) {
    Object.defineProperty(A.prototype, 'x', { value: 0, writable: true, enumerable: true, configurable: true });
    if (order === 'attributes first') Object.defineProperty(this, 'n', { enumerable: false, writable: false });
    Object.defineProperty(this, 'x', { value: v, writable: true, enumerable: true, configurable: true });
    if (order === 'key first') Object.defineProperty(this, 'n', { enumerable: false, writable: false });
  }});
  return A;
}

function checkReshaped(B, name) {
  const first = new B(1);
  assert.strictEqual(JSON.stringify(first), '{"a":"A","x":1}', `${name}: first receiver`);
  assert.strictEqual(Object.getOwnPropertyDescriptor(first, 'n').writable, false, `${name}: first receiver`);
  for (let i = 0; i < HOT; i++) {
    const o = new B(i);
    assert.strictEqual(JSON.stringify(o), `{"a":"A","n":"N","x":${i}}`, `${name}: receiver ${i}`);
    assert.deepStrictEqual(
      Object.getOwnPropertyDescriptor(o, 'n'),
      { value: 'N', writable: true, enumerable: true, configurable: true },
      `${name}: receiver ${i}`
    );
  }
}

for (const order of ['attributes first', 'key first']) {
  test(`a setter that reshapes the receiver is not replayed as an add, this store, ${order}`, () => {
    class B extends reshapingBase(order) { constructor(v) { super(); this.a = 'A'; this.n = 'N'; this.x = v; } }
    checkReshaped(B, order);
  });
}

console.log('ok');
