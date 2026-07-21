function assert(condition, message) {
  if (!condition) throw new Error(message);
}

globalThis.__antJitGlobalRead = 1;

function readGlobal() {
  return __antJitGlobalRead;
}

for (let i = 0; i < 300; i++)
  assert(readGlobal() === 1, "warm global read");

globalThis.__antJitGlobalRead = 2;
assert(readGlobal() === 2, "same-shape global write must be observed");

delete globalThis.__antJitGlobalRead;
globalThis.__antJitGlobalRead = 3;
assert(readGlobal() === 3, "delete and recreate must invalidate the read IC");

let getterCalls = 0;
Object.defineProperty(globalThis, "__antJitGlobalRead", {
  configurable: true,
  get() {
    getterCalls++;
    return 4;
  },
});
assert(readGlobal() === 4, "accessor replacement must use the slow path");
assert(getterCalls === 1, "global accessor must run exactly once");

Object.defineProperty(globalThis, "__antJitGlobalRead", {
  configurable: true,
  writable: true,
  value: undefined,
});
assert(readGlobal() === undefined, "an existing undefined binding must resolve");

function readMissingWithTypeof() {
  return typeof __antJitDefinitelyMissingGlobal;
}

for (let i = 0; i < 300; i++)
  assert(readMissingWithTypeof() === "undefined", "typeof missing global");

const originalMath = Math;
globalThis.Math = { min() { return 123; } };
function readReplacedBuiltin() {
  return Math.min();
}
assert(readReplacedBuiltin() === 123, "replaced builtin global must be observed");
globalThis.Math = originalMath;

delete globalThis.__antJitGlobalRead;
console.log("PASS");
