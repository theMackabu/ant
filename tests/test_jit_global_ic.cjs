function assert(condition, message) {
  if (!condition) throw new Error(message || "assertion failed");
}

globalThis.__jitGlobalIC = 1;

function readGlobal() {
  return __jitGlobalIC;
}

function readMissingType() {
  return typeof __jitMissingGlobalIC;
}

function callGuardedMissingGlobal() {
  if (typeof __jitGuardedMissingGlobalIC !== "undefined") {
    return __jitGuardedMissingGlobalIC();
  }
  return "missing";
}

for (let i = 0; i < 200; i++) {
  assert(readGlobal() === 1, "warm global read mismatch");
  assert(readMissingType() === "undefined", "warm missing global mismatch");
  assert(callGuardedMissingGlobal() === "missing", "guarded missing global mismatch");
}

globalThis.__jitGlobalIC = { value: 2 };
assert(readGlobal().value === 2, "global value update was not observed");

delete globalThis.__jitGlobalIC;
let missingThrew = false;
try {
  readGlobal();
} catch (error) {
  missingThrew = error instanceof ReferenceError;
}
assert(missingThrew, "deleted global did not throw ReferenceError");

globalThis.__jitGlobalIC = 3;
globalThis.__jitMissingGlobalIC = true;
assert(readGlobal() === 3, "recreated global was not observed");
assert(readMissingType() === "boolean", "new global was not observed by typeof");

delete globalThis.__jitGlobalIC;
delete globalThis.__jitMissingGlobalIC;
console.log("JIT global IC tests passed");
