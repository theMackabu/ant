function assertSame(actual, expected, message) {
  if (!Object.is(actual, expected)) {
    throw new Error(`${message}: expected ${String(expected)}, got ${String(actual)}`);
  }
}

let fn = function () { return 1; };
const caller = function () { return fn() + 1; };
for (let i = 0; i < 300; i++) {
  assertSame(caller(), 2, "inline warmup");
}

fn = function () { throw new RangeError("slow boom"); };
let caught = null;
try {
  caller();
} catch (e) {
  caught = e;
}
assertSame(caught instanceof RangeError, true, "inlined slow-path call error must throw");
assertSame(caught.message, "slow boom", "inlined slow-path error message");

fn = function () { return 40; };
assertSame(caller(), 41, "inlined site recovers after deopt");

let tryFn = function () { return 3; };
const tryCaller = function () {
  try {
    return tryFn() + 1;
  } catch (e) {
    return "caught:" + e.message;
  }
};
for (let i = 0; i < 300; i++) {
  assertSame(tryCaller(), 4, "try warmup");
}
tryFn = function () { throw new RangeError("try boom"); };
assertSame(tryCaller(), "caught:try boom", "inlined error routes to enclosing catch");

const inlineElemCaller = function (obj, key) {
  const inner = function (target, prop) { return target[prop]; };
  // Keep the call non-tail so the direct-call inline emitter owns `inner`.
  return inner(obj, key) + 0;
};
for (let i = 0; i < 400; i++) {
  assertSame(inlineElemCaller({value: 1}, "value"), 1, "element inline warmup");
}
let elemTrapCount = 0;
const elemThrowingProxy = new Proxy({}, {
  get() {
    elemTrapCount++;
    throw new RangeError("element boom");
  }
});
caught = null;
try {
  inlineElemCaller(elemThrowingProxy, "value");
} catch (e) {
  caught = e;
}
assertSame(caught instanceof RangeError, true, "inlined element error must throw");
assertSame(elemTrapCount, 1, "inlined element proxy trap runs once");

const inlineElemThenAdd = function (obj, key, rhs) {
  const inner = function (target, prop, value) {
    return target[prop] + value;
  };
  return inner(obj, key, rhs) + 0;
};
for (let i = 0; i < 400; i++) {
  assertSame(inlineElemThenAdd({value: 1}, "value", 2), 3, "post-read inline warmup");
}
let elemGetterCount = 0;
const elemGetter = {};
Object.defineProperty(elemGetter, "value", {
  get() {
    elemGetterCount++;
    return 1;
  }
});
assertSame(
  inlineElemThenAdd(elemGetter, "value", "s"),
  "1s0",
  "post-read bailout result"
);
assertSame(elemGetterCount, 1, "post-read bailout runs getter once");

let elemKeyCoercionCount = 0;
const elemObjectKey = {
  toString() {
    elemKeyCoercionCount++;
    return "value";
  }
};
assertSame(
  inlineElemThenAdd({value: 1}, elemObjectKey, "s"),
  "1s0",
  "post-read bailout with object key result"
);
assertSame(
  elemKeyCoercionCount,
  1,
  "post-read bailout coerces object key once"
);

const inlineFieldCaller = function (obj) {
  const inner = function (target) { return target.value; };
  return inner(obj) + 0;
};
for (let i = 0; i < 400; i++) {
  assertSame(inlineFieldCaller({value: 1}), 1, "field inline warmup");
}
let fieldTrapCount = 0;
const fieldThrowingProxy = new Proxy({}, {
  get() {
    fieldTrapCount++;
    throw new RangeError("field boom");
  }
});
try {
  inlineFieldCaller(fieldThrowingProxy);
} catch (_) {}
assertSame(fieldTrapCount, 1, "inlined field proxy trap runs once");

const inlineFieldThenAdd = function (obj, rhs) {
  const inner = function (target, value) { return target.value + value; };
  return inner(obj, rhs) + 0;
};
for (let i = 0; i < 400; i++) {
  assertSame(inlineFieldThenAdd({value: 1}, 2), 3, "field post-read warmup");
}
let fieldGetterCount = 0;
const fieldGetterObj = {};
Object.defineProperty(fieldGetterObj, "value", {
  get() {
    fieldGetterCount++;
    return 1;
  }
});
assertSame(
  inlineFieldThenAdd(fieldGetterObj, "s"),
  "1s0",
  "field post-read bailout result"
);
assertSame(fieldGetterCount, 1, "field post-read bailout runs getter once");

const inlineMethodCaller = function (obj) {
  const inner = function (target) { return target.m(); };
  return inner(obj) + 0;
};
for (let i = 0; i < 400; i++) {
  assertSame(inlineMethodCaller({m() { return 1; }}), 1, "field2 inline warmup");
}
let field2TrapCount = 0;
const field2Proxy = new Proxy({}, {
  get() {
    field2TrapCount++;
    throw new RangeError("field2 boom");
  }
});
try {
  inlineMethodCaller(field2Proxy);
} catch (_) {}
assertSame(field2TrapCount, 1, "inlined method-receiver proxy trap runs once");

const inlineOptCaller = function (obj) {
  const inner = function (target) { return target?.value; };
  return inner(obj);
};
for (let i = 0; i < 400; i++) {
  assertSame(inlineOptCaller({value: 1}), 1, "optional field inline warmup");
  assertSame(inlineOptCaller(null), undefined, "optional field nullish inline");
}
let optTrapCount = 0;
const optProxy = new Proxy({}, {
  get() {
    optTrapCount++;
    throw new RangeError("opt boom");
  }
});
try {
  inlineOptCaller(optProxy);
} catch (_) {}
assertSame(optTrapCount, 1, "inlined optional-field proxy trap runs once");

let inlineLengthTarget = [1];
const inlineLengthRead = function () { return inlineLengthTarget.length; };
const inlineLengthCaller = function () { return inlineLengthRead() + 0; };
for (let i = 0; i < 200; i++) assertSame(inlineLengthRead(), 1, "length target warmup");
for (let i = 0; i < 400; i++) assertSame(inlineLengthCaller(), 1, "length inline warmup");
let lengthTrapCount = 0;
inlineLengthTarget = new Proxy({}, {
  get() {
    lengthTrapCount++;
    throw new RangeError("length boom");
  }
});
try {
  inlineLengthCaller();
} catch (_) {}
assertSame(lengthTrapCount, 1, "inlined length proxy trap runs once");

let largeDirectCalls = 0;
function largeDirectTarget(
  a0, a1, a2, a3, a4, a5, a6, a7, a8,
  a9, a10, a11, a12, a13, a14, a15, a16
) {
  largeDirectCalls++;
  return a0 + a16;
}

function inlineLargeCallBody(box, fn) {
  box.writes = box.writes + 1;
  fn(1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2);
  return 3;
}
function inlineLargeCallCaller(box) {
  return inlineLargeCallBody(box, largeDirectTarget) + 0;
}

const largeCallBox = {writes: 0};
for (let i = 0; i < 300; i++) {
  const writesBefore = largeCallBox.writes;
  const callsBefore = largeDirectCalls;
  assertSame(inlineLargeCallCaller(largeCallBox), 3, "large nested CALL result");
  assertSame(largeCallBox.writes, writesBefore + 1, "large nested CALL store runs once");
  assertSame(largeDirectCalls, callsBefore + 1, "large nested CALL runs once");
}

function inlineLargeTailCallBody(box, fn) {
  box.writes = box.writes + 1;
  return fn(1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2);
}
function inlineLargeTailCallCaller(box) {
  return inlineLargeTailCallBody(box, largeDirectTarget) + 0;
}

const largeTailCallBox = {writes: 0};
for (let i = 0; i < 300; i++) {
  const writesBefore = largeTailCallBox.writes;
  const callsBefore = largeDirectCalls;
  assertSame(inlineLargeTailCallCaller(largeTailCallBox), 3, "large nested TAIL_CALL result");
  assertSame(largeTailCallBox.writes, writesBefore + 1, "large nested TAIL_CALL store runs once");
  assertSame(largeDirectCalls, callsBefore + 1, "large nested TAIL_CALL runs once");
}

let largeMethodCalls = 0;
const largeMethodTarget = {
  call(
    a0, a1, a2, a3, a4, a5, a6, a7, a8,
    a9, a10, a11, a12, a13, a14, a15, a16
  ) {
    largeMethodCalls++;
    return a0 + a16;
  }
};

function inlineLargeMethodBody(box, target) {
  target.call(
    box.writes = box.writes + 1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2
  );
  return 3;
}
function inlineLargeMethodCaller(box) {
  return inlineLargeMethodBody(box, largeMethodTarget) + 0;
}

const largeMethodBox = {writes: 0};
for (let i = 0; i < 300; i++) {
  largeMethodBox.writes = 0;
  const callsBefore = largeMethodCalls;
  assertSame(inlineLargeMethodCaller(largeMethodBox), 3, "large nested CALL_METHOD result");
  assertSame(largeMethodBox.writes, 1, "large nested CALL_METHOD store runs once");
  assertSame(largeMethodCalls, callsBefore + 1, "large nested CALL_METHOD runs once");
}

function inlineLargeTailMethodBody(box, target) {
  return target.call(
    box.writes = box.writes + 1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2
  );
}
function inlineLargeTailMethodCaller(box) {
  return inlineLargeTailMethodBody(box, largeMethodTarget) + 0;
}

const largeTailMethodBox = {writes: 0};
for (let i = 0; i < 300; i++) {
  largeTailMethodBox.writes = 0;
  const callsBefore = largeMethodCalls;
  assertSame(inlineLargeTailMethodCaller(largeTailMethodBox), 3, "large nested TAIL_CALL_METHOD result");
  assertSame(largeTailMethodBox.writes, 1, "large nested TAIL_CALL_METHOD store runs once");
  assertSame(largeMethodCalls, callsBefore + 1, "large nested TAIL_CALL_METHOD runs once");
}

console.log("PASS");
