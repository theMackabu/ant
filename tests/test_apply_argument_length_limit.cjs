function assert(condition, message) {
  if (!condition) {
    console.log("FAIL: " + message);
    throw new Error(message);
  }
}

const tooLong = { length: 2147483648 };

let applyThrew = false;
try {
  (function () {}).apply(null, tooLong);
} catch (e) {
  applyThrew = e instanceof RangeError;
}

let reflectThrew = false;
try {
  Reflect.apply(function () {}, null, tooLong);
} catch (e) {
  reflectThrew = e instanceof RangeError;
}

assert(applyThrew, "Function.prototype.apply should reject oversized argument lists");
assert(reflectThrew, "Reflect.apply should reject oversized argument lists");

console.log("OK");
