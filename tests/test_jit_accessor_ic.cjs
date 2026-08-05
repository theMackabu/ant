let getterCalls = 0;
const proto = {};
Object.defineProperty(proto, "value", {
  configurable: true,
  get() {
    getterCalls++;
    return this.base + 1;
  },
});

const object = Object.create(proto);
object.base = 40;

function readDot(target) {
  return target.value;
}

function readComputed(target, key) {
  return target[key];
}

for (let i = 0; i < 20_000; i++) {
  if (readDot(object) !== 41) throw new Error("dot accessor IC returned the wrong value");
  if (readComputed(object, "value") !== 41)
    throw new Error("computed accessor IC returned the wrong value");
}
if (getterCalls !== 40_000) throw new Error("accessor IC skipped getter calls");

Object.defineProperty(proto, "value", {
  configurable: true,
  get() {
    return this.base + 2;
  },
});
if (readDot(object) !== 42 || readComputed(object, "value") !== 42)
  throw new Error("accessor IC did not observe a replaced getter");

Object.defineProperty(proto, "value", {
  configurable: true,
  writable: true,
  value: 17,
});
if (readDot(object) !== 17 || readComputed(object, "value") !== 17)
  throw new Error("accessor IC did not transition to a data property");

Object.defineProperty(proto, "value", {
  configurable: true,
  get() {
    throw new RangeError("cached getter failure");
  },
});
for (const read of [() => readDot(object), () => readComputed(object, "value")]) {
  let threw = false;
  try {
    read();
  } catch (error) {
    threw = error instanceof RangeError && error.message === "cached getter failure";
  }
  if (!threw) throw new Error("accessor IC did not propagate a getter exception");
}

console.log("JIT accessor IC tests passed");
