function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const nativeMin = Math.min;
function directNative(a, b) {
  return nativeMin(a, b);
}

for (let i = 0; i < 300; i++)
  assert(directNative(i, 100) === (i < 100 ? i : 100), "direct native call");

const objectValueOf = Object.prototype.valueOf;
function detachedNativeThis() {
  return objectValueOf();
}
assert(detachedNativeThis() === globalThis, "detached native call normalizes this");

function pushValue(array, value) {
  return array.push(value);
}

const array = [];
for (let i = 0; i < 300; i++)
  assert(pushValue(array, i) === i + 1, "native method receiver");
assert(array[299] === 299, "native method writes to its receiver");

const overridden = [];
for (let i = 0; i < 300; i++) pushValue(overridden, i);
overridden.push = function (value) {
  this.seen = value;
  return 777;
};
assert(pushValue(overridden, 42) === 777, "overwritten method bypasses native fast path");
assert(overridden.seen === 42, "overwritten method keeps its receiver");

let getterCalls = 0;
const accessor = {};
Object.defineProperty(accessor, "min", {
  configurable: true,
  get() {
    getterCalls++;
    return nativeMin;
  },
});
function callAccessor() {
  return accessor.min(8, 3);
}
for (let i = 0; i < 300; i++) assert(callAccessor() === 3, "accessor native call");
assert(getterCalls === 300, "method accessor runs once per call");

let proxyCalls = 0;
function proxyTarget(a, b) {
  return nativeMin(a, b);
}
const nativeProxy = new Proxy(proxyTarget, {
  apply(target, thisArg, args) {
    proxyCalls++;
    return Reflect.apply(target, thisArg, args) + 1;
  },
});
function callProxy() {
  return nativeProxy(9, 4);
}
for (let i = 0; i < 300; i++) assert(callProxy() === 5, "callable proxy fallback");
assert(proxyCalls === 300, "callable proxy apply trap");

const nativeParse = JSON.parse;
function parseJson(text) {
  return nativeParse(text);
}
for (let i = 0; i < 300; i++) assert(parseJson("1") === 1, "native return value");
let threw = false;
try {
  parseJson("{");
} catch (error) {
  threw = error instanceof SyntaxError;
}
assert(threw, "native errors propagate through the JIT");

console.log("PASS");
