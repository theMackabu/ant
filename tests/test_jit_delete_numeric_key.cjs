function assertEq(actual, expected, message) {
  if (actual !== expected) {
    throw new Error(`${message}: expected ${expected}, got ${actual}`);
  }
}

function deleteKey(object, key) {
  return delete object[key];
}

for (let i = 0; i < 500; i++) {
  const object = { 123: i };
  assertEq(deleteKey(object, 123), true, "warm numeric delete result");
  assertEq(object[123], undefined, "warm numeric property deleted");
}

const array = [1, 2, 3];
assertEq(deleteKey(array, 1), true, "array index delete result");
assertEq(array[1], undefined, "array index deleted");
assertEq(array.length, 3, "array delete preserves length");

const negativeZero = { 0: true };
assertEq(deleteKey(negativeZero, -0), true, "negative zero delete result");
assertEq(negativeZero[0], undefined, "negative zero maps to zero key");

const fractional = { "1.5": true };
assertEq(deleteKey(fractional, 1.5), true, "fractional delete result");
assertEq(fractional["1.5"], undefined, "fractional key uses coercion path");

const fixed = {};
Object.defineProperty(fixed, "7", {
  value: true,
  configurable: false,
});
assertEq(deleteKey(fixed, 7), false, "non-configurable numeric property");
assertEq(fixed[7], true, "non-configurable property preserved");

let coercions = 0;
const coercingKey = {
  toString() {
    coercions++;
    return "value";
  },
};
const coerced = { value: true };
assertEq(deleteKey(coerced, coercingKey), true, "object key delete result");
assertEq(coercions, 1, "object key coercion count");
assertEq(coerced.value, undefined, "coerced property deleted");

let trapKey;
const proxy = new Proxy({ 9: true }, {
  deleteProperty(target, key) {
    trapKey = key;
    return Reflect.deleteProperty(target, key);
  },
});
assertEq(deleteKey(proxy, 9), true, "proxy numeric delete result");
assertEq(trapKey, "9", "proxy receives string property key");

console.log("OK: test_jit_delete_numeric_key");
