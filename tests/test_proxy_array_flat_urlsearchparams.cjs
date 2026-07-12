function assert(condition, message) {
  if (!condition) {
    console.log("FAIL: " + message);
    throw new Error(message);
  }
}

const proxiedArray = new Proxy([[1, 2], [3, 4]], {});
assert(JSON.stringify(proxiedArray.flat()) === "[1,2,3,4]", "flat should see array proxy elements");
assert(JSON.stringify(proxiedArray.flatMap(function (x) { return x; })) === "[1,2,3,4]", "flatMap should see array proxy elements");
assert(0 in new Proxy([1], {}), "in operator should see array proxy indices");
assert("length" in new Proxy([], {}), "in operator should see array proxy length");

const canonicalIndexArray = [1, 2];
assert(!("00" in canonicalIndexArray), "in operator should not canonicalize leading-zero keys");
canonicalIndexArray["00"] = 3;
assert("00" in canonicalIndexArray, "leading-zero keys should remain ordinary array properties");
assert(canonicalIndexArray.length === 2, "leading-zero properties should not change array length");

const overflowingIndex = "18446744073709551616";
assert(
  !(overflowingIndex in canonicalIndexArray),
  "in operator should reject overflowing array indices"
);
canonicalIndexArray[overflowingIndex] = 4;
assert(overflowingIndex in canonicalIndexArray, "overflowing keys should remain ordinary properties");
assert(canonicalIndexArray.length === 2, "overflowing properties should not change array length");

let inheritedHasCalls = 0;
const inheritedProxyArray = Array(1);
Object.setPrototypeOf(inheritedProxyArray, new Proxy({}, {
  has(target, key) {
    inheritedHasCalls++;
    return key === "0";
  },
}));
assert(0 in inheritedProxyArray, "in operator should invoke an inherited proxy has trap");
assert(inheritedHasCalls === 1, "inherited proxy has trap should run exactly once");

const throwingProxyArray = Array(1);
Object.setPrototypeOf(throwingProxyArray, new Proxy({}, {
  has() {
    throw new Error("inherited has boom");
  },
}));
assertThrows(
  function () { return 0 in throwingProxyArray; },
  "inherited has boom",
  "in operator should propagate inherited proxy has errors"
);

let sawOuterLength = false;
let sawPairLength = false;
const pair = new Proxy(["a", "b"], {
  get(target, key, receiver) {
    if (key === "length") sawPairLength = true;
    return Reflect.get(target, key, receiver);
  },
});
const init = new Proxy([pair], {
  get(target, key, receiver) {
    if (key === "length") sawOuterLength = true;
    return Reflect.get(target, key, receiver);
  },
});

const params = new URLSearchParams(init);
assert(params.toString() === "a=b", "URLSearchParams should accept proxied array pairs");
assert(sawOuterLength, "URLSearchParams should observe outer proxy length");
assert(sawPairLength, "URLSearchParams should observe pair proxy length");

const stringLengthParams = new URLSearchParams(new Proxy([pair], {
  get(target, key, receiver) {
    if (key === "length") return "1";
    return Reflect.get(target, key, receiver);
  },
}));
assert(stringLengthParams.toString() === "a=b", "URLSearchParams should convert proxy length with ToNumber");

assertThrows(
  function () {
    new URLSearchParams(new Proxy([pair], {
      get(target, key, receiver) {
        if (key === "0") throw new Error("outer index boom");
        return Reflect.get(target, key, receiver);
      },
    }));
  },
  "outer index boom",
  "URLSearchParams should propagate outer proxy index errors"
);

assertThrows(
  function () {
    new URLSearchParams([new Proxy(["a", "b"], {
      get(target, key, receiver) {
        if (key === "length") throw new Error("pair length boom");
        return Reflect.get(target, key, receiver);
      },
    })]);
  },
  "pair length boom",
  "URLSearchParams should propagate pair proxy length errors"
);

assertThrows(
  function () {
    new URLSearchParams([new Proxy(["a", "b"], {
      get(target, key, receiver) {
        if (key === "1") throw new Error("pair value boom");
        return Reflect.get(target, key, receiver);
      },
    })]);
  },
  "pair value boom",
  "URLSearchParams should propagate pair proxy value errors"
);

assertThrows(
  function () {
    new URLSearchParams(new Proxy([pair], {
      get(target, key, receiver) {
        if (key === "length") return NaN;
        return Reflect.get(target, key, receiver);
      },
    }));
  },
  "finite",
  "URLSearchParams should reject NaN proxy lengths"
);

assertThrows(
  function () {
    new URLSearchParams(new Proxy([pair], {
      get(target, key, receiver) {
        if (key === "length") return { valueOf() { throw new Error("length convert boom"); } };
        return Reflect.get(target, key, receiver);
      },
    }));
  },
  "length convert boom",
  "URLSearchParams should propagate proxy length conversion errors"
);

console.log("OK");

function assertThrows(fn, expected, message) {
  try {
    fn();
  } catch (err) {
    if (String(err && err.message).indexOf(expected) >= 0) return;
    console.log("FAIL: " + message + ": wrong error " + String(err && err.message));
    throw err;
  }

  console.log("FAIL: " + message + ": did not throw");
  throw new Error(message);
}
