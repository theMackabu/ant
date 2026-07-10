function assertEq(actual, expected, message) {
  if (actual !== expected) {
    throw new Error(`${message}: expected ${expected}, got ${actual}`);
  }
}

function assertThrowsTypeError(fn, message) {
  try {
    fn();
  } catch (error) {
    if (error instanceof TypeError) return;
    throw new Error(`${message}: expected TypeError, got ${error}`);
  }
  throw new Error(`${message}: expected TypeError`);
}

function invokeOptional(fn, value) {
  return fn?.(value);
}

function invokeOptionalMethod(obj, value) {
  return obj.method?.(value);
}

function addOne(value) {
  return value + 1;
}

const receiver = {
  offset: 3,
  method(value) {
    return this.offset + value;
  },
};

for (let i = 0; i < 500; i++) {
  assertEq(invokeOptional(addOne, i), i + 1, "warm callable");
  assertEq(invokeOptionalMethod(receiver, i), i + 3, "warm method");
}

assertEq(invokeOptional(addOne, 41), 42, "hot callable");
assertEq(invokeOptional(null, 41), undefined, "hot null callee");
assertEq(invokeOptional(undefined, 41), undefined, "hot undefined callee");
assertThrowsTypeError(() => invokeOptional(0, 41), "hot falsey non-null callee");
assertEq(invokeOptionalMethod(receiver, 39), 42, "hot method receiver");
assertEq(invokeOptionalMethod({ method: null }, 41), undefined, "hot null method");
assertEq(invokeOptionalMethod({}, 41), undefined, "hot missing method");
assertThrowsTypeError(
  () => invokeOptionalMethod({ method: 0 }, 41),
  "hot falsey non-null method",
);

console.log("OK: test_jit_is_undef_or_null");
