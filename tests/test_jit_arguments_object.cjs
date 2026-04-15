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

function strictSlice(path) {
  "use strict";
  return {
    argc: arguments.length,
    tail: Array.prototype.slice.call(arguments, 1),
  };
}

function directRead(a) {
  return arguments[0] + a;
}

function arrowCapture(a) {
  const read = () => arguments[0] + a;
  return read();
}

for (let i = 0; i < 500; i++) {
  assertEq(mappedRead(1), 2, "warm mapped read");
  assertEq(mappedWrite(1), 2, "warm mapped write");
  assertEq(directRead(3), 6, "warm direct read");
  assertEq(arrowCapture(4), 8, "warm arrow capture");
  const warmStrict = strictSlice("/", "mw");
  assertEq(warmStrict.argc, 2, "warm strict argc");
  assertEq(warmStrict.tail.length, 1, "warm strict tail length");
  assertEq(warmStrict.tail[0], "mw", "warm strict tail first");
}

assertEq(mappedRead(1), 2, "hot mapped read");
assertEq(mappedWrite(1), 2, "hot mapped write");
assertEq(directRead(3), 6, "hot direct read");
assertEq(arrowCapture(4), 8, "hot arrow capture");

const hotStrict = strictSlice("/route", "mw1", "mw2");
assertEq(hotStrict.argc, 3, "hot strict argc");
assertEq(hotStrict.tail.length, 2, "hot strict tail length");
assertEq(hotStrict.tail[0], "mw1", "hot strict tail first");
assertEq(hotStrict.tail[1], "mw2", "hot strict tail second");

console.log("OK: test_jit_arguments_object");
