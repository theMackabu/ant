function assertSame(actual, expected, message) {
  if (!Object.is(actual, expected)) {
    throw new Error(`${message}: expected ${String(expected)}, got ${String(actual)}`);
  }
}

let convertedGets = 0;
globalThis.__antConverted = 7;
Object.defineProperty(globalThis, "__antConverted", {
  configurable: true,
  get() { convertedGets++; return 42; },
});
assertSame(__antConverted, 42, "converted accessor identifier read");
assertSame(globalThis.__antConverted, 42, "converted accessor property read");
assertSame(convertedGets, 2, "converted accessor getter invocations");
delete globalThis.__antConverted;

let freshGets = 0;
Object.defineProperty(globalThis, "__antFresh", {
  configurable: true,
  get() { freshGets++; return "fresh"; },
});
assertSame(__antFresh, "fresh", "fresh accessor identifier read");
assertSame(freshGets, 1, "fresh accessor getter invocations");
delete globalThis.__antFresh;

let hotGets = 0;
globalThis.__antHot = 1;
const readHot = function () {
  return __antHot;
};
for (let i = 0; i < 300; i++) {
  assertSame(readHot(), 1, "hot warmup");
}
Object.defineProperty(globalThis, "__antHot", {
  configurable: true,
  get() { hotGets++; return "swapped"; },
});
for (let i = 0; i < 50; i++) {
  assertSame(readHot(), "swapped", "hot accessor read");
}
assertSame(hotGets, 50, "hot accessor getter invocations");
delete globalThis.__antHot;

globalThis.__antThrows = 1;
const readThrows = function () {
  return __antThrows;
};
for (let i = 0; i < 300; i++) {
  assertSame(readThrows(), 1, "throwing warmup");
}
Object.defineProperty(globalThis, "__antThrows", {
  configurable: true,
  get() { throw new RangeError("getter boom"); },
});
let caught = null;
try {
  readThrows();
} catch (e) {
  caught = e;
}
assertSame(caught instanceof RangeError, true, "hot throwing getter propagates");
assertSame(caught.message, "getter boom", "hot throwing getter message");
delete globalThis.__antThrows;

let typeofGets = 0;
globalThis.__antTypeof = 1;
const readTypeof = function () {
  return typeof __antTypeof;
};
for (let i = 0; i < 300; i++) {
  assertSame(readTypeof(), "number", "typeof warmup");
}
Object.defineProperty(globalThis, "__antTypeof", {
  configurable: true,
  get() { typeofGets++; return "str"; },
});
assertSame(readTypeof(), "string", "typeof accessor read");
assertSame(typeofGets, 1, "typeof accessor getter invocations");
delete globalThis.__antTypeof;

Object.defineProperty(globalThis, "__antTypeofThrows", {
  configurable: true,
  get() { throw new RangeError("typeof boom"); },
});
let typeofCaught = null;
try {
  void typeof __antTypeofThrows;
} catch (e) {
  typeofCaught = e;
}
assertSame(typeofCaught instanceof RangeError, true, "typeof throwing getter propagates");
delete globalThis.__antTypeofThrows;

let hotTypeofGets = 0;
globalThis.__antHotTypeof = 1;
const readHotTypeof = function () {
  return typeof __antHotTypeof;
};
for (let i = 0; i < 300; i++) {
  assertSame(readHotTypeof(), "number", "hot typeof warmup");
}
Object.defineProperty(globalThis, "__antHotTypeof", {
  configurable: true,
  get() { hotTypeofGets++; throw new RangeError("hot typeof boom"); },
});
let hotTypeofCaught = null;
try {
  readHotTypeof();
} catch (e) {
  hotTypeofCaught = e;
}
assertSame(hotTypeofCaught instanceof RangeError, true, "hot typeof throwing getter propagates");
assertSame(hotTypeofGets, 1, "hot typeof getter invoked exactly once");
delete globalThis.__antHotTypeof;

let inlinedGets = 0;
globalThis.__antInlined = 5;
const readInlined = function () {
  return __antInlined;
};
const callInlined = function () {
  return readInlined();
};
for (let i = 0; i < 300; i++) {
  assertSame(callInlined(), 5, "inlined warmup");
}
Object.defineProperty(globalThis, "__antInlined", {
  configurable: true,
  get() { inlinedGets++; throw new RangeError("inlined boom"); },
});
let inlinedCaught = null;
try {
  callInlined();
} catch (e) {
  inlinedCaught = e;
}
assertSame(inlinedCaught instanceof RangeError, true, "inlined throwing getter propagates");
assertSame(inlinedGets, 1, "inlined getter invoked exactly once");
delete globalThis.__antInlined;

console.log("PASS");
