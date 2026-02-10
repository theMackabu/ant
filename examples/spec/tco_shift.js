import { test, summary } from './helpers.js';

console.log('TCO Shift/Comparison Operator Tests\n');

function identity(x) {
  return x;
}

function shiftLeft(n) {
  if (n <= 0) return identity(1) << 3;
  return shiftLeft(n - 1);
}
test('f() << N result', shiftLeft(5), 8);

function shiftRight(n) {
  if (n <= 0) return identity(64) >> 2;
  return shiftRight(n - 1);
}
test('f() >> N result', shiftRight(5), 16);

function shiftRightUnsigned(n) {
  if (n <= 0) return identity(-1) >>> 24;
  return shiftRightUnsigned(n - 1);
}
test('f() >>> N result', shiftRightUnsigned(5), 255);

function lessThan(n) {
  if (n <= 0) return identity(3) < 5;
  return lessThan(n - 1);
}
test('f() < N result', lessThan(3), true);

function greaterThan(n) {
  if (n <= 0) return identity(10) > 5;
  return greaterThan(n - 1);
}
test('f() > N result', greaterThan(3), true);

function lessEq(n) {
  if (n <= 0) return identity(5) <= 5;
  return lessEq(n - 1);
}
test('f() <= N result', lessEq(3), true);

function greaterEq(n) {
  if (n <= 0) return identity(5) >= 10;
  return greaterEq(n - 1);
}
test('f() >= N result', greaterEq(3), false);

function deepShiftLeft(n) {
  if (n <= 0) return identity(1) << 3;
  return deepShiftLeft(n - 1);
}
test('deep f()<<N value correct', deepShiftLeft(500), 8);

function deepShiftRight(n) {
  if (n <= 0) return identity(64) >> 2;
  return deepShiftRight(n - 1);
}
test('deep f()>>N value correct', deepShiftRight(500), 16);

function deepUnsignedShift(n) {
  if (n <= 0) return identity(-1) >>> 24;
  return deepUnsignedShift(n - 1);
}
test('deep f()>>>N value correct', deepUnsignedShift(500), 255);

function recurseShiftArg(n) {
  if (n <= 0) return 'done';
  return recurseShiftArg((n - 1) >> 0);
}
test('shift inside arg (tail-eligible)', recurseShiftArg(100000), 'done');

function ternaryShift(n) {
  return n <= 0 ? identity(1) << 4 : ternaryShift(n - 1);
}
test('ternary with shift in then-branch', ternaryShift(5), 16);

function ternaryBothShift(flag) {
  return flag ? identity(1) << 2 : identity(1) >> 1;
}
test('ternary both shift true', ternaryBothShift(true), 4);
test('ternary both shift false', ternaryBothShift(false), 0);

summary();
