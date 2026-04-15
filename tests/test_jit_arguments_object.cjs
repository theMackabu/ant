function assertEq(actual, expected, msg) {
  if (actual !== expected) {
    throw new Error(`${msg}: expected ${expected}, got ${actual}`);
  }
}

function mappedRead(a) {
  a = 2;
  return arguments[0];
}

function mappedWrite(a) {
  arguments[0] = 2;
  return a;
}

for (let i = 0; i < 500; i++) {
  assertEq(mappedRead(1), 2, "warm mapped read");
  assertEq(mappedWrite(1), 2, "warm mapped write");
}

assertEq(mappedRead(1), 2, "hot mapped read");
assertEq(mappedWrite(1), 2, "hot mapped write");

console.log("OK: test_jit_arguments_object");
