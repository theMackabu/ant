function assertEq(actual, expected, message) {
  if (actual !== expected) {
    throw new Error(`${message}: expected ${expected}, got ${actual}`);
  }
}

function writeValue(object, value) {
  object.value = value;
}

function strictWriteValue(object, value) {
  "use strict";
  object.value = value;
}

function caughtStrictWrite(object, value) {
  try {
    strictWriteValue(object, value);
    return "no error";
  } catch (error) {
    return error.name;
  }
}

function writeLength(object, value) {
  object.length = value;
}

const hot = { value: 0 };
for (let i = 0; i < 500; i++) writeValue(hot, i);
assertEq(hot.value, 499, "hot own-property write");

// A cache entry belongs to a bytecode site, not to one receiver forever.
const sibling = { value: 0 };
writeValue(sibling, 501);
writeValue(sibling, 502);
assertEq(sibling.value, 502, "same-shape sibling receiver");

// A descriptor/shape change must leave the direct-store path before invoking
// an accessor. The setter must also observe the original receiver.
let setterReceiver;
let setterValue;
Object.defineProperty(hot, "value", {
  configurable: true,
  set(value) {
    setterReceiver = this;
    setterValue = value;
  },
});
writeValue(hot, 600);
assertEq(setterReceiver, hot, "accessor receiver after warmup");
assertEq(setterValue, 600, "accessor value after warmup");

// Strict assignment failures must still escape the JIT helper path.
const readonly = {};
Object.defineProperty(readonly, "value", {
  value: 1,
  writable: false,
  configurable: true,
});
assertEq(caughtStrictWrite(readonly, 2), "TypeError", "readonly strict write");
assertEq(readonly.value, 1, "readonly value preserved");

// Deleting and recreating the property invalidates the old slot assumptions.
delete sibling.value;
writeValue(sibling, 700);
assertEq(sibling.value, 700, "write after delete");

// Exercise an overflow property slot and the non-number write-barrier path.
const wide = {};
for (let i = 0; i < 40; i++) wide[`field${i}`] = i;
wide.value = null;
const retained = { marker: 42 };
for (let i = 0; i < 500; i++) writeValue(wide, retained);
assertEq(wide.value, retained, "overflow object write");
assertEq(wide.value.marker, 42, "written object retained");

// Proxies/exotic receivers must remain on the semantic slow path.
let proxyReceiver;
let proxyValue;
const target = { value: 0 };
const proxy = new Proxy(target, {
  set(object, key, value, receiver) {
    proxyReceiver = receiver;
    proxyValue = value;
    return Reflect.set(object, key, value, receiver);
  },
});
writeValue(proxy, 800);
assertEq(proxyReceiver, proxy, "proxy receiver");
assertEq(proxyValue, 800, "proxy trap value");
assertEq(target.value, 800, "proxy target value");

const array = [];
for (let i = 0; i < 500; i++) array[i] = i;
for (let i = 0; i < 500; i++) writeLength(array, 500 - i);
assertEq(array.length, 1, "hot array length shrink");
assertEq(array[1], undefined, "array shrink clears elements");
writeLength(array, 10);
assertEq(array.length, 10, "array length growth");
assertEq(array[9], undefined, "array growth creates holes");

let lengthError = "none";
try {
  writeLength(array, -1);
} catch (error) {
  lengthError = error.name;
}
assertEq(lengthError, "RangeError", "invalid array length");

let lengthSetterValue;
const lengthObject = {};
Object.defineProperty(lengthObject, "length", {
  set(value) {
    lengthSetterValue = value;
  },
});
writeLength(lengthObject, 12);
assertEq(lengthSetterValue, 12, "non-array length setter");

console.log("OK: test_jit_put_field_ic");
