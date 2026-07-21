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

console.log("PASS");
