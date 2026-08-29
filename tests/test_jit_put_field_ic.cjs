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

// Repeated constructor sites should reuse exact from-shape -> to-shape
// transitions. Reference-valued fields are safe without a remembered-set
// insertion while the freshly allocated receiver is still young.
function Node(id, next, callback, items) {
  this.id = id;
  this.next = next;
  this.callback = callback;
  this.items = items;
}

function identity(value) {
  return value;
}

let chain = null;
for (let i = 0; i < 2000; i++) {
  chain = new Node(i, chain, identity, [i, i + 1]);
}
assertEq(chain.id, 1999, "shape-transition numeric field");
assertEq(chain.next.id, 1998, "shape-transition object field");
assertEq(chain.callback(17), 17, "shape-transition function field");
assertEq(chain.items[1], 2000, "shape-transition array field");

let chainDepth = 0;
for (let node = chain; node !== null; node = node.next) chainDepth++;
assertEq(chainDepth, 2000, "shape-transition references survive allocation pressure");

function addLate(object, value) {
  object.late = value;
}

function strictAddLate(object, value) {
  "use strict";
  object.late = value;
}

for (let i = 0; i < 500; i++) addLate({}, i);

// A negative lookup guard installed after the transition was learned must be
// invalidated when another receiver takes that transition.
function readLate(object) {
  return object.late;
}
for (let i = 0; i < 500; i++) assertEq(readLate({}), undefined, "negative lookup warmup");
const lateTarget = {};
addLate(lateTarget, 77);
assertEq(readLate(lateTarget), 77, "transition invalidates negative lookup");

const nonExtensible = Object.preventExtensions({});
addLate(nonExtensible, 1);
assertEq(nonExtensible.late, undefined, "sloppy non-extensible add");
let nonExtensibleError = "none";
try {
  strictAddLate(nonExtensible, 2);
} catch (error) {
  nonExtensibleError = error.name;
}
assertEq(nonExtensibleError, "TypeError", "strict non-extensible add");

const sealed = Object.seal({});
addLate(sealed, 3);
assertEq(sealed.late, undefined, "sealed add excluded");
const frozen = Object.freeze({});
addLate(frozen, 4);
assertEq(frozen.late, undefined, "frozen add excluded");

const arrayProperty = [];
addLate(arrayProperty, 5);
assertEq(arrayProperty.late, 5, "array add stays semantic");

function writeProto(object, proto) {
  object.__proto__ = proto;
}
for (let i = 0; i < 500; i++) writeProto({}, null);
const proto = { inherited: 91 };
const protoTarget = {};
writeProto(protoTarget, proto);
assertEq(Object.getPrototypeOf(protoTarget), proto, "__proto__ setter excluded");
assertEq(protoTarget.inherited, 91, "__proto__ inheritance preserved");

console.log("OK: test_jit_put_field_ic");
