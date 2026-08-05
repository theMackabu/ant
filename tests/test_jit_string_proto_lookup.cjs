function callIndexOf(value) {
  return value.indexOf("n");
}

for (let i = 0; i < 20_000; i++) {
  if (callIndexOf("ant") !== 1) throw new Error("hot String.prototype lookup failed");
}

const original = String.prototype.indexOf;
String.prototype.indexOf = function () { return 42; };
if (callIndexOf("ant") !== 42) throw new Error("replaced string method was not observed");

let getterCalls = 0;
Object.defineProperty(String.prototype, "indexOf", {
  configurable: true,
  get() {
    getterCalls++;
    return function () { return 17; };
  },
});
if (callIndexOf("ant") !== 17 || getterCalls !== 1)
  throw new Error("string prototype accessor fallback failed");

Object.defineProperty(String.prototype, "indexOf", {
  configurable: true,
  writable: true,
  value: original,
});

console.log("JIT string prototype lookup tests passed");
