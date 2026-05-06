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

console.log("OK");
