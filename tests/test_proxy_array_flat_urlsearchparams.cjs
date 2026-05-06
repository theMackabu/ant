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
