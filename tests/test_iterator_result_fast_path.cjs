function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

function assertEq(actual, expected, msg) {
  if (actual !== expected) {
    throw new Error(msg + " (expected " + expected + ", got " + actual + ")");
  }
}

function makeOwnDataIterable(limit) {
  return {
    [Symbol.iterator]() {
      let i = 0;
      const result = { done: false, value: 0 };
      return {
        next() {
          if (i < limit) {
            result.done = false;
            result.value = i++;
          } else {
            result.done = true;
            result.value = undefined;
          }
          return result;
        }
      };
    }
  };
}

function makeInheritedIterable(limit) {
  return {
    [Symbol.iterator]() {
      let i = 0;
      const proto = { done: false, value: 0 };
      const result = Object.create(proto);
      return {
        next() {
          if (i < limit) {
            proto.done = false;
            proto.value = i++;
          } else {
            proto.done = true;
            proto.value = undefined;
          }
          return result;
        }
      };
    }
  };
}

function makeAccessorIterable(limit, counts) {
  return {
    [Symbol.iterator]() {
      let i = 0;
      let done = false;
      let value = undefined;
      const result = {};

      Object.defineProperty(result, "done", {
        get() {
          counts.done++;
          return done;
        },
        enumerable: true,
        configurable: true,
      });

      Object.defineProperty(result, "value", {
        get() {
          counts.value++;
          return value;
        },
        enumerable: true,
        configurable: true,
      });

      return {
        next() {
          if (i < limit) {
            done = false;
            value = i++;
          } else {
            done = true;
            value = undefined;
          }
          return result;
        }
      };
    }
  };
}

async function collectAsync(iterable) {
  let sum = 0;
  for await (const v of iterable) sum += v;
  return sum;
}

function makeAsyncOwnDataIterable(limit) {
  return {
    [Symbol.asyncIterator]() {
      let i = 0;
      const result = { done: false, value: 0 };
      return {
        async next() {
          if (i < limit) {
            result.done = false;
            result.value = i++;
          } else {
            result.done = true;
            result.value = undefined;
          }
          return result;
        }
      };
    }
  };
}

async function main() {
  console.log("iterator result fast-path regression");

  console.log("\nTest 1: own data properties stay fast-path compatible");
  {
    let sum = 0;
    for (const v of makeOwnDataIterable(8)) sum += v;
    assertEq(sum, 28, "own data result object should iterate correctly");
  }
  console.log("PASS");

  console.log("\nTest 2: inherited done/value still fall back correctly");
  {
    let sum = 0;
    for (const v of makeInheritedIterable(8)) sum += v;
    assertEq(sum, 28, "inherited iterator result properties should still work");
  }
  console.log("PASS");

  console.log("\nTest 3: accessor done/value still invoke getters");
  {
    const counts = { done: 0, value: 0 };
    let sum = 0;
    for (const v of makeAccessorIterable(5, counts)) sum += v;
    assertEq(sum, 10, "accessor-backed iterator result should still iterate correctly");
    assert(counts.done >= 6, "done getter should be observed for each step");
    assert(counts.value >= 5, "value getter should be observed for yielded steps");
  }
  console.log("PASS");

  console.log("\nTest 4: array destructuring still consumes reusable results");
  {
    const [a, b, c] = makeOwnDataIterable(3);
    assertEq(a, 0, "first destructured value should match");
    assertEq(b, 1, "second destructured value should match");
    assertEq(c, 2, "third destructured value should match");
  }
  console.log("PASS");

  console.log("\nTest 5: async iterator reusable result still works");
  {
    const sum = await collectAsync(makeAsyncOwnDataIterable(6));
    assertEq(sum, 15, "async iterator result object should still work");
  }
  console.log("PASS");

  console.log("\nAll iterator result fast-path tests passed");
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  throw err;
});
