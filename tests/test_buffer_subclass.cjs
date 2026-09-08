const assert = require('node:assert');

for (const Base of [
  Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array,
  Int32Array, Uint32Array, Float32Array, Float64Array, BigInt64Array, BigUint64Array,
  ...(typeof Float16Array === 'function' ? [Float16Array] : []),
]) {
  class Sub extends Base { marker() { return 'sub'; } }
  class Deep extends Sub {}
  const bigint = Base === BigInt64Array || Base === BigUint64Array;
  const one = bigint ? 1n : 1;
  const two = bigint ? 2n : 2;
  const values = [one, two];
  const buffer = new ArrayBuffer(Base.BYTES_PER_ELEMENT * 4);
  for (const args of [
    [], [2], [values], [new Base(values)], [new Set(values)],
    [{ 0: one, 1: two, length: 2 }], [buffer, Base.BYTES_PER_ELEMENT, 2],
  ]) {
    const value = new Sub(...args);
    assert.strictEqual(Object.getPrototypeOf(value), Sub.prototype, Base.name);
    assert.ok(value instanceof Sub);
    assert.ok(value instanceof Base);
    assert.strictEqual(value.marker(), 'sub');
    if (value.length) {
      value[0] = two;
      assert.strictEqual(value[0], two);
    }
  }
  assert.strictEqual(Object.getPrototypeOf(new Deep(2)), Deep.prototype);
  const Bound = Sub.bind(null, 2);
  assert.strictEqual(Object.getPrototypeOf(new Bound()), Sub.prototype);

  function Alternate() {}
  assert.strictEqual(Object.getPrototypeOf(Reflect.construct(Base, [2], Alternate)), Alternate.prototype);
  Alternate.prototype = 1;
  assert.strictEqual(Object.getPrototypeOf(Reflect.construct(Base, [2], Alternate)), Base.prototype);
}

for (const Base of [ArrayBuffer, DataView]) {
  class Sub extends Base { marker() { return 'sub'; } }
  class Deep extends Sub {}
  const args = Base === ArrayBuffer ? [16] : [new ArrayBuffer(16), 4, 8];
  const value = new Sub(...args);
  assert.strictEqual(Object.getPrototypeOf(value), Sub.prototype);
  assert.ok(value instanceof Sub);
  assert.ok(value instanceof Base);
  assert.strictEqual(value.marker(), 'sub');
  assert.strictEqual(value.byteLength, Base === ArrayBuffer ? 16 : 8);
  if (Base === DataView) {
    value.setUint8(0, 42);
    assert.strictEqual(value.getUint8(0), 42);
    assert.strictEqual(value.byteOffset, 4);
    assert.strictEqual(value.buffer, args[0]);
  } else {
    const view = new Uint8Array(value);
    view[0] = 42;
    assert.strictEqual(view[0], 42);
  }
  assert.strictEqual(Object.getPrototypeOf(new Deep(...args)), Deep.prototype);
  const Bound = Sub.bind(null, ...args);
  assert.strictEqual(Object.getPrototypeOf(new Bound()), Sub.prototype);
  function Alternate() {}
  assert.strictEqual(Object.getPrototypeOf(Reflect.construct(Base, args, Alternate)), Alternate.prototype);
  Alternate.prototype = null;
  assert.strictEqual(Object.getPrototypeOf(Reflect.construct(Base, args, Alternate)), Base.prototype);
}
console.log('buffer:subclass:ok');
