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

function denseGet(array, index) {
  return array[index];
}

const getArray = [1.25, 2.5, 3.75, 5];
for (let i = 0; i < 300; i++) {
  assertSame(denseGet(getArray, i & 3), getArray[i & 3], "dense get warmup");
}
assertSame(denseGet(getArray, -0), 1.25, "negative zero index");
const frozenGetArray = Object.freeze([13, 17, 19, 23]);
assertSame(denseGet(frozenGetArray, 2), 19, "frozen dense array read");

Array.prototype[1] = 91;
try {
  delete getArray[1];
  assertSame(denseGet(getArray, 1), 91, "hole must consult the prototype");
} finally {
  delete Array.prototype[1];
}

function densePut(array, index, value) {
  "use strict";
  array[index] = value;
  return array[index];
}

const putArray = [0.5, 1.5, 2.5, 3.5];
for (let i = 0; i < 300; i++) {
  const index = i & 3;
  const value = i + 0.25;
  assertSame(densePut(putArray, index, value), value, "dense put warmup");
}

let setterTotal = 0;
Object.defineProperty(putArray, "1", {
  configurable: true,
  enumerable: true,
  get() { return 77; },
  set(value) { setterTotal += value; },
});
assertSame(densePut(putArray, 1, 5), 77, "numeric accessor result");
assertSame(setterTotal, 5, "numeric accessor setter");

function frozenPut(array, index, value) {
  "use strict";
  array[index] = value;
}

const frozenArray = [1, 2, 3, 4];
for (let i = 0; i < 300; i++) frozenPut(frozenArray, i & 3, i);
Object.freeze(frozenArray);
assertThrowsTypeError(
  () => frozenPut(frozenArray, 0, 9),
  "frozen dense array write must throw"
);
assertSame(frozenArray[0], 296, "frozen dense array write must not store");

function sloppyFrozenUndefinedPut(array) {
  array[0] = undefined;
}

const frozenUndefinedArray = Object.freeze([41]);
let inheritedSetterCalls = 0;
Object.defineProperty(Array.prototype, "0", {
  configurable: true,
  set() { inheritedSetterCalls++; },
});
try {
  sloppyFrozenUndefinedPut(frozenUndefinedArray);
  assertSame(
    inheritedSetterCalls,
    0,
    "sloppy frozen undefined write must not invoke an inherited setter",
  );
  assertSame(
    frozenUndefinedArray[0],
    41,
    "sloppy frozen undefined write must leave the dense element unchanged",
  );
} finally {
  delete Array.prototype[0];
}

function fillFreshDenseArray(size) {
  "use strict";
  const array = [];
  for (let i = 0; i < size; i++) array[i] = i + 0.25;
  return array.length + array[0] + array[size - 1];
}

for (let i = 0; i < 300; i++) {
  assertSame(
    fillFreshDenseArray(24),
    47.5,
    "numeric fallback must grow a fresh dense array",
  );
}

function numericFallbackPut(target, index, value) {
  "use strict";
  target[index] = value;
  return target[index];
}

for (let i = 0; i < 300; i++) {
  const array = [];
  assertSame(numericFallbackPut(array, 0, i), i, "numeric fallback warmup");
}

const negativeZeroPutArray = [];
assertSame(numericFallbackPut(negativeZeroPutArray, -0, 7), 7, "negative zero put");
assertSame(negativeZeroPutArray.length, 1, "negative zero must grow index zero");

const fractionalPutArray = [];
assertSame(numericFallbackPut(fractionalPutArray, 1.5, 9), 9, "fractional numeric put");
assertSame(fractionalPutArray.length, 0, "fractional key must not grow an array");
assertSame(fractionalPutArray[1.5], 9, "fractional numeric property value");

const sparsePutArray = [];
assertSame(numericFallbackPut(sparsePutArray, 128, 11), 11, "sparse numeric put");
assertSame(sparsePutArray.length, 129, "sparse numeric put must update length");

let proxySetCount = 0;
let proxySetKey;
const proxyTarget = [];
const numericPutProxy = new Proxy(proxyTarget, {
  set(target, key, value) {
    proxySetCount++;
    proxySetKey = key;
    target[key] = value;
    return true;
  },
});
assertSame(numericFallbackPut(numericPutProxy, 0, 13), 13, "proxy numeric put");
assertSame(proxySetCount, 1, "proxy numeric put must invoke the set trap once");
assertSame(proxySetKey, "0", "proxy numeric key must be canonicalized");

const frozenAppendArray = Object.freeze([]);
assertThrowsTypeError(
  () => numericFallbackPut(frozenAppendArray, 0, 17),
  "frozen numeric append must throw",
);

const nonExtensiblePutArray = [];
Object.preventExtensions(nonExtensiblePutArray);
assertThrowsTypeError(
  () => numericFallbackPut(nonExtensiblePutArray, 0, 19),
  "non-extensible numeric append must throw",
);

function writeMappedArgument(initial, replacement) {
  arguments[0] = replacement;
  return initial;
}

for (let i = 0; i < 300; i++) {
  assertSame(writeMappedArgument(1, i), i, "mapped argument numeric put");
}

function bitAnd(left, right) { return left & right; }
function bitOr(left, right) { return left | right; }
function bitXor(left, right) { return left ^ right; }
function shiftLeft(left, right) { return left << right; }
function shiftRight(left, right) { return left >> right; }
function shiftRightUnsigned(left, right) { return left >>> right; }
function bitNot(value) { return ~value; }

const binaryCases = [
  [bitAnd, -1, 0xf0f0f0f0, -252645136],
  [bitOr, 0x80000000, 7, -2147483641],
  [bitXor, 0xffffffff, 0x55555555, -1431655766],
  [shiftLeft, 0x40000001, 1, -2147483646],
  [shiftRight, 0x80000000, 4, -134217728],
  [shiftRightUnsigned, 0x80000000, 4, 134217728],
];

for (const [operation, left, right, expected] of binaryCases) {
  for (let i = 0; i < 300; i++) {
    assertSame(operation(left, right), expected, "word32 warmup");
  }
}
for (let i = 0; i < 300; i++) assertSame(bitNot(0x80000000), 2147483647, "bitnot warmup");

assertSame(bitAnd(5.75, 3.2), 1, "fractional ToInt32 fallback");
assertSame(bitOr(NaN, Infinity), 0, "non-finite ToInt32 fallback");
assertSame(shiftLeft(3, 35), 24, "shift count mask");
assertSame(shiftRightUnsigned(-1, 0), 4294967295, "unsigned result");
assertSame(bitNot(-0), -1, "negative zero ToInt32");

function word32Chain(left, right, shift, mask) {
  return ((((left ^ right) >>> shift) & mask) | 0x40);
}
function word32ChainThenNumber(left, right) {
  return ((left ^ right) >>> 1) + 0.5;
}
function word32ChainWithBailout(left, right, mask) {
  return ((left ^ right) & mask) >>> 1;
}
function doubleBitNot(value) {
  return ~(~value);
}

for (let i = 0; i < 300; i++) {
  assertSame(
    word32Chain(0xffffffff, 0x55555555, 1, 0x7fffffff),
    1431655765,
    "word32 chain",
  );
  assertSame(
    word32ChainThenNumber(0xffffffff, 0x55555555),
    1431655765.5,
    "word32 chain converted to Number",
  );
  assertSame(doubleBitNot(0x80000000), -2147483648, "chained bitnot");
  assertSame(
    word32ChainWithBailout(0xffffffff, 0x55555555, 3),
    1,
    "word32 bailout warmup",
  );
}
assertSame(
  word32ChainWithBailout(0xffffffff, 0x55555555, 3.5),
  1,
  "word32 chain bailout must reconstruct an integer-resident operand",
);

let coercions = 0;
const coercible = {
  valueOf() {
    coercions++;
    return 8;
  },
};
assertSame(bitOr(coercible, 1), 9, "object coercion fallback");
assertSame(coercions, 1, "object coercion count");
assertSame(bitAnd(7n, 3n), 3n, "BigInt fallback");
assertThrowsTypeError(() => bitAnd(7n, 3), "mixed BigInt must throw");

const inlineGetTarget = function (array, index) {
  return array[index];
};
const inlineGetCaller = function (array, index) {
  return inlineGetTarget(array, index) + 0;
};
const inlineArray = [3, 5, 7, 11];
for (let i = 0; i < 200; i++) inlineGetTarget(inlineArray, i & 3);
for (let i = 0; i < 400; i++) {
  assertSame(inlineGetCaller(inlineArray, i & 3), inlineArray[i & 3], "inline dense get");
}
Array.prototype[2] = 101;
try {
  delete inlineArray[2];
  assertSame(inlineGetCaller(inlineArray, 2), 101, "inline dense get slow exit");
} finally {
  delete Array.prototype[2];
}

const inlineWordTarget = function (left, right) {
  return (left ^ right) >>> 1;
};
const inlineWordCaller = function (left, right) {
  return inlineWordTarget(left, right) + 0;
};
for (let i = 0; i < 200; i++) inlineWordTarget(0xffffffff, 0x55555555);
for (let i = 0; i < 400; i++) {
  assertSame(inlineWordCaller(0xffffffff, 0x55555555), 1431655765, "inline word32");
}
assertSame(inlineWordCaller(7.5, 3.25), 2, "inline word32 slow exit");

let inlineReplayReads = 0;
Object.defineProperty(globalThis, "inlineReplayValue", {
  configurable: true,
  get() {
    inlineReplayReads++;
    return 1;
  },
});
globalThis.inlineReplayData = 7;

try {
  const repeatedGlobalTarget = function () {
    return inlineReplayData;
  };
  const repeatedGlobalCaller = function () {
    return repeatedGlobalTarget() + repeatedGlobalTarget();
  };
  for (let i = 0; i < 200; i++) repeatedGlobalTarget();
  for (let i = 0; i < 400; i++) {
    assertSame(
      repeatedGlobalCaller(),
      14,
      "repeated inline global sites need distinct MIR registers",
    );
  }

  const effectThenGetTarget = function (array, index) {
    const observed = inlineReplayValue;
    return array[index] + observed;
  };
  const effectThenGetCaller = function (array, index) {
    return effectThenGetTarget(array, index) + 0;
  };
  const replayArray = [3, 5, 7, 11];
  for (let i = 0; i < 200; i++) effectThenGetTarget(replayArray, i & 3);
  for (let i = 0; i < 400; i++) {
    assertSame(
      effectThenGetCaller(replayArray, i & 3),
      replayArray[i & 3] + 1,
      "effect before inline dense get",
    );
  }
  Array.prototype[2] = 101;
  try {
    delete replayArray[2];
    const readsBeforeGetMiss = inlineReplayReads;
    assertSame(
      effectThenGetCaller(replayArray, 2),
      102,
      "inline dense get miss after effect",
    );
    assertSame(
      inlineReplayReads,
      readsBeforeGetMiss + 1,
      "inline dense get miss must not replay an earlier effect",
    );
  } finally {
    delete Array.prototype[2];
  }

  const effectThenWordTarget = function (left, right) {
    const observed = inlineReplayValue;
    return ((left ^ right) >>> 1) + observed;
  };
  const effectThenWordCaller = function (left, right) {
    return effectThenWordTarget(left, right) + 0;
  };
  for (let i = 0; i < 200; i++) {
    effectThenWordTarget(0xffffffff, 0x55555555);
  }
  for (let i = 0; i < 400; i++) {
    assertSame(
      effectThenWordCaller(0xffffffff, 0x55555555),
      1431655766,
      "effect before inline word32",
    );
  }
  const readsBeforeWordMiss = inlineReplayReads;
  assertSame(
    effectThenWordCaller(7.5, 3.25),
    3,
    "inline word32 miss after effect",
  );
  assertSame(
    inlineReplayReads,
    readsBeforeWordMiss + 1,
    "inline word32 miss must not replay an earlier effect",
  );
} finally {
  delete globalThis.inlineReplayValue;
  delete globalThis.inlineReplayData;
}

console.log("PASS");
