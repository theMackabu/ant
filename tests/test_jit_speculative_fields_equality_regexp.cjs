function assertSame(actual, expected, message) {
  if (!Object.is(actual, expected)) {
    throw new Error(`${message}: expected ${String(expected)}, got ${String(actual)}`);
  }
}

function assertThrowsTypeError(fn, message) {
  let threw = false;
  try {
    fn();
  } catch (error) {
    threw = error instanceof TypeError;
  }
  assertSame(threw, true, message);
}

function abstractEqual(left, right) { return left == right; }
function abstractNotEqual(left, right) { return left != right; }
function strictEqual(left, right) { return left === right; }
function strictNotEqual(left, right) { return left !== right; }
function knownNumericLooseEqual(left, right) { return left + 1 == right; }
function callKnownNumericLooseEqual(left, right) {
  return knownNumericLooseEqual(left, right);
}

for (let i = 0; i < 300; i++) {
  assertSame(abstractEqual(i, i), true, "numeric abstract equality");
  assertSame(abstractNotEqual(i, i + 1), true, "numeric abstract inequality");
  assertSame(strictEqual(i, i), true, "numeric strict equality");
  assertSame(strictNotEqual(i, i + 1), true, "numeric strict inequality");
  assertSame(callKnownNumericLooseEqual(i, i + 1), true,
    "inlined known-number equality fast path");
}

assertSame(abstractEqual(NaN, NaN), false, "NaN abstract equality");
assertSame(strictEqual(NaN, NaN), false, "NaN strict equality");
assertSame(abstractEqual(-0, 0), true, "negative zero abstract equality");
assertSame(strictEqual(-0, 0), true, "negative zero strict equality");
assertSame(callKnownNumericLooseEqual(1, "2"), true,
  "inlined known-number equality reboxes before slow helper");

function isUndefined(value) { return value === undefined; }
function isDefined(value) { return value !== undefined; }
function isNull(value) { return value === null; }
function isTrue(value) { return value === true; }
function mixedStrictEqual(left, right) { return left === right; }

for (let i = 0; i < 300; i++) {
  assertSame(isUndefined(i), false, "number is not undefined");
  assertSame(isDefined(i), true, "number is defined");
  assertSame(isNull(i), false, "number is not null");
  assertSame(isTrue(i), false, "number is not true");
  assertSame(mixedStrictEqual(i, undefined), false, "mixed strict equality");
}
assertSame(isUndefined(undefined), true, "undefined singleton equality");
assertSame(isDefined(undefined), false, "undefined singleton inequality");
assertSame(isNull(null), true, "null singleton equality");
assertSame(isTrue(true), true, "boolean singleton equality");
assertSame(mixedStrictEqual(undefined, 1), false, "tagged versus Number");

let coercions = 0;
const coercible = {
  valueOf() {
    coercions++;
    return 8;
  },
};
assertSame(abstractEqual(coercible, 8), true, "abstract equality slow exit");
assertSame(coercions, 1, "abstract equality coercion count");
assertSame(strictEqual(coercible, 8), false, "strict equality slow exit");
assertSame(coercions, 1, "strict equality must not coerce");

function sameReference(left, right) { return left === right; }
function differentReference(left, right) { return left !== right; }
function sameText(left, right) { return left === right; }
function sameBigInt(left, right) { return left === right; }

const identityObject = { marker: 1 };
for (let i = 0; i < 300; i++) {
  assertSame(sameReference(identityObject, identityObject), true, "object identity equality");
  assertSame(differentReference(identityObject, { marker: 1 }), true, "distinct object inequality");
}

for (let i = 0; i < 300; i++) {
  const left = `strict-${i % 7}`;
  const right = `strict-${i % 7}`.slice(0);
  assertSame(sameText(left, right), true, "distinct equal string values");
}
assertSame(sameText("strict-a", "strict-b"), false, "unequal string values");

for (let i = 0; i < 300; i++) {
  assertSame(sameBigInt(BigInt(i % 11), BigInt(i % 11)), true, "equal BigInt values");
}
assertSame(sameBigInt(101n, 102n), false, "unequal BigInt values");

const sharedSymbol = Symbol("shared");
assertSame(sameReference(sharedSymbol, sharedSymbol), true, "symbol identity equality");
assertSame(sameReference(Symbol("shared"), Symbol("shared")), false, "distinct symbol inequality");

function readX(object) { return object.x; }
const proto = { x: 41 };
const receiver = Object.create(proto);
for (let i = 0; i < 300; i++) assertSame(readX(receiver), 41, "prototype field warmup");
proto.x = 42;
assertSame(readX(receiver), 42, "field IC reads updated slot");
delete proto.x;
assertSame(readX(receiver), undefined, "field IC slow exit after deletion");

function optionalReadTarget(object) { return object?.x; }
function optionalReadCaller(object) { return optionalReadTarget(object); }
const optionalReceiver = { x: 73 };
for (let i = 0; i < 200; i++) optionalReadTarget(optionalReceiver);
for (let i = 0; i < 300; i++) {
  assertSame(optionalReadCaller(optionalReceiver), 73, "inline optional field IC");
}
assertSame(optionalReadCaller(null), undefined, "inline optional nullish path");
delete optionalReceiver.x;
assertSame(optionalReadCaller(optionalReceiver), undefined, "inline optional IC miss");

function methodReadTarget(object) { return object.read(); }
function methodReadCaller(object) { return methodReadTarget(object); }
const methodProto = {
  read() { return this.value; },
};
const methodReceiver = Object.create(methodProto);
methodReceiver.value = 11;
for (let i = 0; i < 200; i++) methodReadTarget(methodReceiver);
for (let i = 0; i < 300; i++) {
  assertSame(methodReadCaller(methodReceiver), 11, "inline method field IC");
}
methodProto.read = function () { return this.value + 1; };
assertSame(methodReadCaller(methodReceiver), 12, "inline method IC miss");

function writeX(object, value) {
  "use strict";
  object.x = value;
  return object.x;
}
const writable = { x: 0 };
for (let i = 0; i < 300; i++) assertSame(writeX(writable, i), i, "field store warmup");
Object.freeze(writable);
assertThrowsTypeError(() => writeX(writable, 999), "frozen field store slow exit");
assertSame(writable.x, 299, "frozen field store must not mutate");

function addY(object, value) {
  "use strict";
  object.y = value;
  return object.y;
}
for (let i = 0; i < 300; i++) {
  assertSame(addY({ x: i }, i + 1), i + 1, "field add transition warmup");
}
const addReceiver = { x: 1 };
assertSame(addY(addReceiver, 73), 73, "field add transition");
assertSame(addY(addReceiver, 74), 74, "field store after add transition");

function makeLiteral() { return /a+/gi; }
for (let i = 0; i < 300; i++) {
  const regexp = makeLiteral();
  assertSame(regexp.source, "a+", "regexp source");
  assertSame(regexp.flags, "gi", "regexp flags");
}
const first = makeLiteral();
const second = makeLiteral();
first.lastIndex = 7;
assertSame(first === second, false, "regexp literal allocation identity");
assertSame(second.lastIndex, 0, "regexp literal state isolation");

console.log("PASS");
