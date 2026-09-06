const assert = require('node:assert');

function assertThrowsName(fn, name) {
  let thrown;
  try {
    fn();
  } catch (error) {
    thrown = error;
  }
  assert(thrown, `expected ${name}`);
  assert.strictEqual(thrown.name, name);
}

for (const Constructor of [ArrayBuffer, SharedArrayBuffer]) {
  assert.strictEqual(new Constructor().byteLength, 0);
  assert.strictEqual(new Constructor(undefined).byteLength, 0);
  assert.strictEqual(new Constructor(NaN).byteLength, 0);
  assert.strictEqual(new Constructor(-0.5).byteLength, 0);
  assert.strictEqual(new Constructor(3.9).byteLength, 3);
  assert.strictEqual(new Constructor('4').byteLength, 4);
  assert.strictEqual(new Constructor({ valueOf: () => 5 }).byteLength, 5);

  assertThrowsName(() => new Constructor(-1), 'RangeError');
  assertThrowsName(() => new Constructor(Infinity), 'RangeError');
  assertThrowsName(() => new Constructor(-Infinity), 'RangeError');
  assertThrowsName(() => new Constructor(2 ** 53), 'RangeError');
  assertThrowsName(() => new Constructor(1n), 'TypeError');
  assertThrowsName(() => new Constructor(Symbol('length')), 'TypeError');
}

for (const Constructor of [Uint8Array, Uint32Array]) {
  assert.strictEqual(new Constructor(undefined).length, 0);
  assert.strictEqual(new Constructor(NaN).length, 0);
  assert.strictEqual(new Constructor(-0.5).length, 0);
  assert.strictEqual(new Constructor(3.9).length, 3);
  assert.strictEqual(new Constructor('4').length, 4);

  assertThrowsName(() => new Constructor(-1), 'RangeError');
  assertThrowsName(() => new Constructor(Infinity), 'RangeError');
  assertThrowsName(() => new Constructor(2 ** 53), 'RangeError');
  assertThrowsName(() => new Constructor(1n), 'TypeError');
  assertThrowsName(() => new Constructor(Symbol('length')), 'TypeError');
}

const arrayLike = new Uint8Array({ length: '3', 0: 7, 1: 8, 2: 9 });
assert.deepStrictEqual([...arrayLike], [7, 8, 9]);
assert.strictEqual(new Uint8Array({ length: -1 }).length, 0);
assert.strictEqual(new Uint8Array({ length: -0.5 }).length, 0);

const typedBacking = new ArrayBuffer(16);
assert.strictEqual(new Uint32Array(typedBacking, '4', '2').length, 2);
assert.strictEqual(new Uint32Array(typedBacking, -0.5).byteOffset, 0);
assert.strictEqual(new Uint32Array(typedBacking, 4, 2.9).length, 2);
assert.strictEqual(new Uint32Array(typedBacking, 4, undefined).length, 3);
assertThrowsName(() => new Uint32Array(typedBacking, -1), 'RangeError');
assertThrowsName(() => new Uint32Array(typedBacking, 2), 'RangeError');
assertThrowsName(() => new Uint32Array(typedBacking, 4, -1), 'RangeError');
assertThrowsName(() => new Uint32Array(new ArrayBuffer(15)), 'RangeError');

const dataViewBacking = new ArrayBuffer(16);
assert.strictEqual(new DataView(dataViewBacking, '4', '2').byteLength, 2);
assert.strictEqual(new DataView(dataViewBacking, -0.5).byteOffset, 0);
assert.strictEqual(new DataView(dataViewBacking, 4, 2.9).byteLength, 2);
assert.strictEqual(new DataView(dataViewBacking, 4, undefined).byteLength, 12);
assertThrowsName(() => new DataView(dataViewBacking, -1), 'RangeError');
assertThrowsName(() => new DataView(dataViewBacking, 4, -1), 'RangeError');
assertThrowsName(() => new DataView(dataViewBacking, 17), 'RangeError');

assert.strictEqual(new ArrayBuffer(8).transfer(undefined).byteLength, 8);
assert.strictEqual(new ArrayBuffer(8).transfer(-0.5).byteLength, 0);
assert.strictEqual(new ArrayBuffer(8).transfer(3.9).byteLength, 3);
assert.strictEqual(new ArrayBuffer(8).transfer('4').byteLength, 4);
assert.strictEqual(new ArrayBuffer(8).transferToFixedLength('4').byteLength, 4);

const failedTransfer = new ArrayBuffer(8);
assertThrowsName(() => failedTransfer.transfer(-1), 'RangeError');
assert.strictEqual(failedTransfer.detached, false);
assertThrowsName(() => failedTransfer.transfer(Infinity), 'RangeError');
assert.strictEqual(failedTransfer.detached, false);
assertThrowsName(() => failedTransfer.transfer(1n), 'TypeError');
assert.strictEqual(failedTransfer.detached, false);

const conversionError = new Error('conversion failed');
let thrown;
try {
  new DataView(dataViewBacking, { valueOf: () => { throw conversionError; } });
} catch (error) {
  thrown = error;
}
assert.strictEqual(thrown, conversionError);

console.log('buffer:index-conversion:ok');

function assertThrowsValue(fn, expected) {
  let actual;
  try { fn(); } catch (error) { actual = error; }
  assert.strictEqual(actual, expected);
}

for (const Constructor of [DataView, Uint8Array, Uint32Array]) {
  const offsetBacking = new ArrayBuffer(8);
  assertThrowsName(() => new Constructor(offsetBacking, {
    valueOf() { offsetBacking.transfer(); return 0; }
  }), 'TypeError');

  const lengthBacking = new ArrayBuffer(8);
  assertThrowsName(() => new Constructor(lengthBacking, 4, {
    valueOf() { lengthBacking.transfer(); return 1; }
  }), 'TypeError');

  const oversizedBacking = new ArrayBuffer(8);
  assertThrowsName(() => new Constructor(oversizedBacking, 4, {
    valueOf() { oversizedBacking.transfer(); return 1024; }
  }), Constructor === DataView ? 'RangeError' : 'TypeError');
}

for (const method of ['transfer', 'transferToFixedLength']) {
  const backing = new ArrayBuffer(8);
  assertThrowsName(() => backing[method]({
    valueOf() { backing.transfer(); return 1; }
  }), 'TypeError');
  assert.strictEqual(backing.detached, true);
}

for (const Constructor of [Uint8Array, Uint32Array, BigInt64Array]) {
  const expected = Constructor === BigInt64Array ? [7n, 8n] : [7, 8];
  const source = new Constructor(expected);
  Object.defineProperty(source, Symbol.iterator, {
    get() { throw new Error('TypedArray copy must not read iterator'); }
  });
  Object.defineProperty(source, 'length', {
    get() { throw new Error('TypedArray copy must use internal length'); }
  });
  assert.deepStrictEqual([...new Constructor(source)], expected);
}
assert.deepStrictEqual([...new Uint32Array(new Uint8Array([7, 8]))], [7, 8]);
assertThrowsName(() => new Uint8Array(new BigInt64Array(0)), 'TypeError');
assertThrowsName(() => new BigInt64Array(new Uint8Array(0)), 'TypeError');
const detachedSource = new Uint8Array(8);
detachedSource.buffer.transfer();
assertThrowsName(() => new Uint8Array(detachedSource), 'TypeError');

let iteratorGets = 0;
let nextGets = 0;
const iterableSource = {
  get length() { throw new Error('iterator must take precedence'); },
  get [Symbol.iterator]() {
    iteratorGets++;
    return function () {
      assert.strictEqual(this, iterableSource);
      let index = 0;
      const iterator = {
        get next() {
          nextGets++;
          return function () {
            assert.strictEqual(this, iterator);
            const value = ++index;
            return { get done() { return value > 2; }, get value() { return value; } };
          };
        }
      };
      return iterator;
    };
  }
};
assert.deepStrictEqual([...new Uint8Array(iterableSource)], [1, 2]);
assert.strictEqual(iteratorGets, 1);
assert.strictEqual(nextGets, 1);

for (const stage of ['method', 'next-get', 'next-call', 'done', 'value']) {
  const expected = new Error(stage);
  const source = {
    length: 1,
    [Symbol.iterator]() {
      if (stage === 'method') throw expected;
      return {
        get next() {
          if (stage === 'next-get') throw expected;
          return function () {
            if (stage === 'next-call') throw expected;
            return {
              get done() { if (stage === 'done') throw expected; return false; },
              get value() { throw expected; }
            };
          };
        }
      };
    }
  };
  assertThrowsValue(() => new Uint8Array(source), expected);
}
assertThrowsName(() => new Uint8Array({ length: 1, [Symbol.iterator]: 1 }), 'TypeError');
assertThrowsName(() => new Uint8Array({ [Symbol.iterator]() { return 1; } }), 'TypeError');
assertThrowsName(() => new Uint8Array({ [Symbol.iterator]() { return { next() { return 1; } }; } }), 'TypeError');
assert.deepStrictEqual([...new Uint8Array({ length: 1, 0: 9, [Symbol.iterator]: null })], [9]);

const offsetSource = new Uint8Array(new Uint8Array([1, 7, 8, 2]).buffer, 1, 2);
assert.deepStrictEqual([...new Uint8Array(offsetSource)], [7, 8]);
assert.deepStrictEqual([...new Uint32Array(offsetSource)], [7, 8]);

let finishedIterating = false;
const objectValues = {
  *[Symbol.iterator]() {
    for (let i = 0; i < 40; i++) {
      yield { valueOf() { assert.strictEqual(finishedIterating, true); return i; } };
    }
    finishedIterating = true;
  }
};
assert.deepStrictEqual([...new Uint8Array(objectValues)], Array.from({ length: 40 }, (_, i) => i));
assertThrowsName(() => new Uint8Array({ [Symbol.iterator]() { return { next: 1 }; } }), 'TypeError');

console.log('buffer:coercion-reentrancy-and-iteration:ok');
