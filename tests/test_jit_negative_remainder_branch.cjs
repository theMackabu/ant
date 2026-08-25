const assert = require('assert');

function remainderMagnitude(value) {
  var remainder = value % 4;
  return remainder < 0 ? -remainder : remainder;
}

function localMagnitude(value) {
  var local = value;
  return local < 0 ? -local : local;
}

function negateAfterCompare(value) {
  var local = value;
  local < 0;
  return -local;
}

function bitwiseNotAfterCompare(value) {
  var local = value;
  local < 0;
  return ~local;
}

function logicalNotAfterCompare(value) {
  var local = value;
  local < 0;
  return !local;
}

function typeofAfterCompare(value) {
  var local = value;
  local < 0;
  return typeof local;
}

function returnAfterCompare(value) {
  var local = value;
  local < 0;
  return local;
}

function warmAndAssert(fn, input, expected) {
  var actual;
  for (var call = 1; call <= 101; call++) {
    actual = fn(input);
  }
  assert.strictEqual(actual, expected);
}

warmAndAssert(remainderMagnitude, -5, 1);
warmAndAssert(localMagnitude, -5, 5);
warmAndAssert(negateAfterCompare, -5, 5);

// Adjacent unary consumers must materialize a double-only local before
// reading its boxed stack register.
warmAndAssert(bitwiseNotAfterCompare, -5, 4);
warmAndAssert(logicalNotAfterCompare, 5, false);
warmAndAssert(typeofAfterCompare, -5, 'number');
warmAndAssert(returnAfterCompare, -5, -5);

assert.ok(Object.is(negateAfterCompare(0), -0));
assert.ok(Object.is(negateAfterCompare(-0), 0));
assert.strictEqual(negateAfterCompare(Infinity), -Infinity);
assert.ok(Number.isNaN(negateAfterCompare(NaN)));

console.log('OK: test_jit_negative_remainder_branch');
