const assert = require("node:assert");

let passed = 0;
assert.strictEqual([].includes.call({
  get "0"() {
    passed = NaN;
    return "foo";
  },
  get "11"() {
    passed += 1;
    return 0;
  },
  get "19"() {
    passed += 1;
    return "foo";
  },
  get "21"() {
    passed = NaN;
    return "foo";
  },
  get length() {
    passed += 1;
    return 24;
  }
}, "foo", 6), true);
assert.strictEqual(passed, 3);

assert.strictEqual([,].includes(), true);
assert.strictEqual(Array(1).includes(), true);

let getLog = [];
let proxy = new Proxy({ length: 3, 0: "", 1: "", 2: "", 3: "" }, {
  get(target, key) {
    getLog.push(String(key));
    return target[key];
  }
});
Array.prototype.includes.call(proxy, {});
assert.deepStrictEqual(getLog, ["length", "0", "1", "2"]);

getLog = [];
proxy = new Proxy({ length: 4, 0: NaN, 1: "", 2: NaN, 3: "" }, {
  get(target, key) {
    getLog.push(String(key));
    return target[key];
  }
});
Array.prototype.includes.call(proxy, NaN, 1);
assert.deepStrictEqual(getLog, ["length", "1", "2"]);

[
  Int8Array,
  Uint8Array,
  Uint8ClampedArray,
  Int16Array,
  Uint16Array,
  Int32Array,
  Uint32Array,
  Float32Array,
  Float64Array
].forEach((TypedArray) => {
  const view = new TypedArray([1, 2, 3]);
  assert.strictEqual(view.includes(1), true);
  assert.strictEqual(view.includes(4), false);
  assert.strictEqual(view.includes(1, 1), false);
});

let threw = false;
try {
  Function("function bar(...a) {'use strict';}")();
} catch (_err) {
  threw = true;
}
assert.strictEqual(threw, true);

console.log("ES2016 includes and strict-mode regressions pass");
