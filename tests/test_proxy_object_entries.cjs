const assert = require("node:assert");

let getCalls = 0;

const proxy = new Proxy({}, {
  ownKeys() {
    return ["kimi-for-coding", "hidden", Symbol("skip")];
  },
  getOwnPropertyDescriptor(_target, prop) {
    if (prop === "kimi-for-coding") {
      return { enumerable: true, configurable: true };
    }
    if (prop === "hidden") {
      return { enumerable: false, configurable: true };
    }
    if (typeof prop === "symbol") {
      return { enumerable: true, configurable: true };
    }
  },
  get(_target, prop) {
    getCalls++;
    if (prop === "kimi-for-coding") {
      return { apiKeyEnvVar: "KIMI_API_KEY" };
    }
    if (prop === "hidden") {
      return { apiKeyEnvVar: "HIDDEN_API_KEY" };
    }
    return { apiKeyEnvVar: "SYMBOL_API_KEY" };
  }
});

assert.deepStrictEqual(Object.keys(proxy), ["kimi-for-coding"]);
assert.deepStrictEqual(Object.values(proxy), [{ apiKeyEnvVar: "KIMI_API_KEY" }]);
assert.deepStrictEqual(Object.entries(proxy), [["kimi-for-coding", { apiKeyEnvVar: "KIMI_API_KEY" }]]);
assert.strictEqual(getCalls, 2);

console.log("proxy Object.keys/Object.values/Object.entries respect proxy traps");
