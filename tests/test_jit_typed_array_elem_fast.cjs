function assertEq(actual, expected, message) {
  if (actual !== expected && !(Number.isNaN(actual) && Number.isNaN(expected))) {
    throw new Error(`${message}: expected ${expected}, got ${actual}`);
  }
}

function read(array, index) {
  return array[index];
}

function write(array, index, value) {
  array[index] = value;
}

const int32 = new Int32Array(4);
for (let i = 0; i < 500; i++) write(int32, 1, i);
assertEq(read(int32, 1), 499, "hot Int32Array read/write");

const uint8 = new Uint8Array(2);
write(uint8, 0, 258);
assertEq(read(uint8, 0), 2, "Uint8Array conversion");

const clamped = new Uint8ClampedArray(3);
write(clamped, 0, -1);
write(clamped, 1, 2.5);
write(clamped, 2, 300);
assertEq(read(clamped, 0), 0, "clamped lower bound");
assertEq(read(clamped, 1), 2, "clamped tie-to-even");
assertEq(read(clamped, 2), 255, "clamped upper bound");

const float32 = new Float32Array(2);
write(float32, 0, 1.5);
write(float32, 1, NaN);
assertEq(read(float32, 0), 1.5, "Float32Array value");
assertEq(read(float32, 1), NaN, "Float32Array NaN");

let coercions = 0;
write(int32, 2, {
  valueOf() {
    coercions++;
    return 42;
  },
});
assertEq(coercions, 1, "typed-array value coercion count");
assertEq(read(int32, 2), 42, "typed-array coerced value");

write(int32, 99, 7);
assertEq(read(int32, 99), undefined, "out-of-bounds read");
assertEq(int32.length, 4, "out-of-bounds write preserves length");

const bigint = new BigInt64Array(1);
write(bigint, 0, 9n);
assertEq(read(bigint, 0), 9n, "BigInt64Array read/write");
let bigintError = "none";
try {
  write(bigint, 0, 1);
} catch (error) {
  bigintError = error.name;
}
assertEq(bigintError, "TypeError", "BigInt64Array rejects Number");

let proxyWrites = 0;
const target = new Int32Array(1);
const proxy = new Proxy(target, {
  set() {
    proxyWrites++;
    return true;
  },
});
write(proxy, 0, 10);
assertEq(proxyWrites, 1, "typed-array proxy trap");

console.log("OK: test_jit_typed_array_elem_fast");
