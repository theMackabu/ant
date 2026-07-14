function assertSame(actual, expected, message) {
  if (!Object.is(actual, expected)) {
    throw new Error(`${message}: expected ${expected}, got ${actual}`);
  }
}

function makeAdder(initial) {
  let value = initial;

  return {
    run(n) {
      let sum = 0;
      for (let i = 0; i < n; i++) {
        sum += value;
      }
      return sum;
    },
    set(next) {
      value = next;
    },
  };
}

const adder = makeAdder(1.25);
for (let i = 0; i < 300; i++) {
  assertSame(adder.run(8), 10, "numeric warmup");
}
assertSame(adder.run(10_000), 12_500, "numeric fast path");

// The numeric feedback guard must deopt before performing the addition when
// the captured RHS changes type after JIT compilation.
adder.set("x");
assertSame(adder.run(3), "0xxx", "string feedback transition");

adder.set(2);
assertSame(adder.run(20), 40, "numeric execution after transition");

adder.set(Symbol("value"));
let threw = false;
try {
  adder.run(1);
} catch (error) {
  threw = error instanceof TypeError;
}
assertSame(threw, true, "symbol addition must still throw");

function makeFastAdder(initial) {
  let value = initial;

  function run(n) {
    let sum = 0;
    for (let i = 0; i < n; i++) {
      sum += value;
    }
    return sum;
  }

  function set(next) {
    value = next;
  }

  return [run, set];
}

const [runFast, setFast] = makeFastAdder(0.5);
for (let i = 0; i < 300; i++) {
  assertSame(runFast(8), 4, "plain-function numeric warmup");
}
assertSame(runFast(10_000), 5_000, "plain-function numeric fast path");
setFast("y");
assertSame(runFast(3), "0yyy", "numeric fast-path deoptimization");

function addFromSeed(seed, value, n) {
  let sum = seed;
  for (let i = 0; i < n; i++) {
    sum += value;
  }
  return sum;
}

for (let i = 0; i < 300; i++) {
  assertSame(addFromSeed(0, 2, 4), 8, "local numeric warmup");
}
assertSame(addFromSeed("", "a", 3), "aaa", "local type transition");

function capturedTarget(n, changeAt) {
  let sum = 0;
  function replace(value) {
    sum = value;
  }

  for (let i = 0; i < n; i++) {
    if (i === changeAt) replace(100);
    sum += 1;
  }
  return sum;
}

for (let i = 0; i < 300; i++) {
  assertSame(capturedTarget(10, -1), 10, "captured target warmup");
}
assertSame(
  capturedTarget(10, 5),
  105,
  "captured local mutation must remain synchronized"
);

function osrFirstCall(n, value) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    sum += value;
  }
  return sum;
}

assertSame(osrFirstCall(10_000, 0.5), 5_000, "OSR numeric fast path");

console.log("PASS");
