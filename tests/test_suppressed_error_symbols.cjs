function assert(condition, message) {
  if (!condition) throw new Error(message);
}

assert(typeof Symbol.dispose === "symbol", "Symbol.dispose exists");
assert(typeof Symbol.asyncDispose === "symbol", "Symbol.asyncDispose exists");
assert(Symbol.dispose.description === "Symbol.dispose", "Symbol.dispose description");
assert(Symbol.asyncDispose.description === "Symbol.asyncDispose", "Symbol.asyncDispose description");

const error = new Error("inner");
const suppressed = new TypeError("outer");
const err = new SuppressedError(error, suppressed, "cleanup failed");

assert(err instanceof SuppressedError, "instanceof SuppressedError");
assert(err instanceof Error, "instanceof Error");
assert(Error.isError(err), "Error.isError recognizes SuppressedError");
assert(err.name === "SuppressedError", "name is SuppressedError");
assert(err.message === "cleanup failed", "message is set");
assert(err.error === error, "error field is set");
assert(err.suppressed === suppressed, "suppressed field is set");
assert(typeof err.stack === "string", "stack is captured");

const called = SuppressedError(error, suppressed);
assert(called instanceof SuppressedError, "SuppressedError is callable");
assert(called.error === error, "callable error field");
assert(called.suppressed === suppressed, "callable suppressed field");

console.log("SuppressedError and dispose symbol tests completed!");
