let failed = 0;

function eq(label, actual, expected) {
  if (Object.is(actual, expected)) return;
  failed++;
  console.log(`FAIL ${label}: got ${actual}, expected ${expected}`);
}

function warm(fn, value) {
  for (let i = 0; i < 5000; i++) fn(value);
}

// A warmed true result must not survive replacement of the constructor's
// public prototype property.
function OldCtor() {}
const oldInstance = new OldCtor();
function isOldCtor(value) { return value instanceof OldCtor; }
warm(isOldCtor, oldInstance);
eq('warm true instanceof', isOldCtor(oldInstance), true);
OldCtor.prototype = {};
eq('prototype assignment invalidates true instanceof', isOldCtor(oldInstance), false);
eq('replacement prototype accepts new instances', isOldCtor(new OldCtor()), true);

// Exercise the reverse transition at a separate IC site: false must become
// true when the constructor is pointed at the receiver's existing prototype.
function NewCtor() {}
const matchingPrototype = {};
const futureInstance = Object.create(matchingPrototype);
function isNewCtor(value) { return value instanceof NewCtor; }
warm(isNewCtor, futureInstance);
eq('warm false instanceof', isNewCtor(futureInstance), false);
NewCtor.prototype = matchingPrototype;
eq('prototype assignment invalidates false instanceof', isNewCtor(futureInstance), true);

// Interleave a warmed existing-property store with its instanceof consumer so
// invalidation must also run on cached PUT_FIELD hits, not only cold stores.
function HotStoreCtor() {}
const hotStoreInitialPrototype = HotStoreCtor.prototype;
const hotStoreReplacementPrototype = {};
const hotStoreOldInstance = new HotStoreCtor();
const hotStoreFutureInstance = Object.create(hotStoreReplacementPrototype);
function setHotStorePrototype(proto) { HotStoreCtor.prototype = proto; }
function isHotStoreCtor(value) { return value instanceof HotStoreCtor; }
for (let i = 0; i < 5000; i++) {
  setHotStorePrototype(hotStoreInitialPrototype);
  isHotStoreCtor(hotStoreOldInstance);
}
eq('warm cached prototype store', isHotStoreCtor(hotStoreOldInstance), true);
setHotStorePrototype(hotStoreReplacementPrototype);
eq(
  'cached prototype overwrite invalidates true instanceof',
  isHotStoreCtor(hotStoreOldInstance), false
);
eq(
  'cached prototype overwrite invalidates false instanceof',
  isHotStoreCtor(hotStoreFutureInstance), true
);

// defineProperty reaches a different property-store path from OP_PUT_FIELD.
function DefinedCtor() {}
const definedInstance = new DefinedCtor();
function isDefinedCtor(value) { return value instanceof DefinedCtor; }
warm(isDefinedCtor, definedInstance);
Object.defineProperty(DefinedCtor, 'prototype', { value: {}, writable: true });
eq('defineProperty invalidates instanceof', isDefinedCtor(definedInstance), false);

// A cached add-transition must invalidate instanceof too. Arrow functions
// begin without an own prototype, so this first resolves the inherited one.
const inheritedAddPrototype = {};
const ownAddPrototype = {};
Function.prototype.prototype = inheritedAddPrototype;
function addOwnPrototype(fn, proto) { fn.prototype = proto; }
for (let i = 0; i < 5000; i++) addOwnPrototype(() => {}, {});
const addTarget = () => {};
const addCandidate = Object.create(ownAddPrototype);
function isAddTarget(value) { return value instanceof addTarget; }
warm(isAddTarget, addCandidate);
eq(
  'warm inherited prototype before cached add',
  isAddTarget(addCandidate), false
);
addOwnPrototype(addTarget, ownAddPrototype);
eq(
  'cached prototype add invalidates instanceof',
  isAddTarget(addCandidate), true
);
delete Function.prototype.prototype;

// A property merely named "prototype" on a non-callable object must remain a
// normal cached store even though it shares the dedicated invalidation epoch.
const holder = { prototype: 0 };
function storePrototype(value) { holder.prototype = value; }
for (let i = 0; i < 10000; i++) storePrototype(i);
eq('non-callable prototype store', holder.prototype, 9999);

// C2 remains independently sound: replacing the global epoch bump must not
// let a primitive negative IC miss an addition to its prototype chain.
function primitivePrototype(value) { return value.prototype; }
warm(primitivePrototype, 'x');
eq('warm primitive miss', primitivePrototype('x'), undefined);
String.prototype.prototype = 7;
eq('primitive prototype addition observed', primitivePrototype('x'), 7);
delete String.prototype.prototype;
eq('primitive prototype deletion observed', primitivePrototype('x'), undefined);

// Primitive receiver lookup uses the realm's intrinsic prototype, not the
// mutable global constructor binding. Warm and fresh sites must both ignore a
// replacement constructor and later writes to its public prototype property.
const RealString = String;
RealString.prototype.rebindProbe = 1;
function warmStringRebind(value) { return value.rebindProbe; }
warm(warmStringRebind, 'x');
globalThis.String = function StringReplacement() {};
globalThis.String.prototype = { rebindProbe: 2 };
function freshStringRebind(value) { return value.rebindProbe; }
eq('String rebind preserves warm intrinsic lookup', warmStringRebind('x'), 1);
eq('String rebind preserves fresh intrinsic lookup', freshStringRebind('x'), 1);
globalThis.String.prototype = { rebindProbe: 3 };
eq('replacement String prototype stays unrelated', warmStringRebind('x'), 1);
globalThis.String = RealString;
delete RealString.prototype.rebindProbe;

const RealNumber = Number;
RealNumber.prototype.rebindProbe = 1;
function warmNumberRebind(value) { return value.rebindProbe; }
warm(warmNumberRebind, 1);
globalThis.Number = function NumberReplacement() {};
globalThis.Number.prototype = { rebindProbe: 2 };
function freshNumberRebind(value) { return value.rebindProbe; }
eq('Number rebind preserves warm intrinsic lookup', warmNumberRebind(1), 1);
eq('Number rebind preserves fresh intrinsic lookup', freshNumberRebind(1), 1);
globalThis.Number.prototype = { rebindProbe: 3 };
eq('replacement Number prototype stays unrelated', warmNumberRebind(1), 1);
globalThis.Number = RealNumber;
delete RealNumber.prototype.rebindProbe;

const RealBoolean = Boolean;
RealBoolean.prototype.rebindProbe = 1;
function warmBooleanRebind(value) { return value.rebindProbe; }
warm(warmBooleanRebind, true);
globalThis.Boolean = function BooleanReplacement() {};
globalThis.Boolean.prototype = { rebindProbe: 2 };
function freshBooleanRebind(value) { return value.rebindProbe; }
eq('Boolean rebind preserves warm intrinsic lookup', warmBooleanRebind(true), 1);
eq('Boolean rebind preserves fresh intrinsic lookup', freshBooleanRebind(true), 1);
globalThis.Boolean.prototype = { rebindProbe: 3 };
eq('replacement Boolean prototype stays unrelated', warmBooleanRebind(true), 1);
globalThis.Boolean = RealBoolean;
delete RealBoolean.prototype.rebindProbe;

const RealBigInt = BigInt;
RealBigInt.prototype.rebindProbe = 1;
function warmBigIntRebind(value) { return value.rebindProbe; }
warm(warmBigIntRebind, 1n);
globalThis.BigInt = function BigIntReplacement() {};
globalThis.BigInt.prototype = { rebindProbe: 2 };
function freshBigIntRebind(value) { return value.rebindProbe; }
eq('BigInt rebind preserves warm intrinsic lookup', warmBigIntRebind(1n), 1);
eq('BigInt rebind preserves fresh intrinsic lookup', freshBigIntRebind(1n), 1);
globalThis.BigInt = RealBigInt;
delete RealBigInt.prototype.rebindProbe;

const RealSymbol = Symbol;
RealSymbol.prototype.rebindProbe = 1;
const symbolValue = RealSymbol('value');
function warmSymbolRebind(value) { return value.rebindProbe; }
warm(warmSymbolRebind, symbolValue);
globalThis.Symbol = function SymbolReplacement() {};
globalThis.Symbol.prototype = { rebindProbe: 2 };
function freshSymbolRebind(value) { return value.rebindProbe; }
eq(
  'Symbol rebind preserves warm intrinsic lookup',
  warmSymbolRebind(symbolValue), 1
);
eq(
  'Symbol rebind preserves fresh intrinsic lookup',
  freshSymbolRebind(symbolValue), 1
);
globalThis.Symbol = RealSymbol;
delete RealSymbol.prototype.rebindProbe;

const RealFunction = Function;
RealFunction.prototype.rebindProbe = 1;
function warmFunctionRebind(value) { return value.rebindProbe; }
warm(warmFunctionRebind, parseInt);
globalThis.Function = function FunctionReplacement() {};
globalThis.Function.prototype = { rebindProbe: 2 };
eq(
  'Function rebind preserves intrinsic cfunc lookup',
  warmFunctionRebind(parseInt), 1
);
globalThis.Function = RealFunction;
delete RealFunction.prototype.rebindProbe;

const RealPromise = Promise;
RealPromise.prototype.rebindProbe = 1;
globalThis.Promise = function PromiseReplacement() {};
globalThis.Promise.prototype = { rebindProbe: 2 };
eq(
  'Promise rebind preserves intrinsic promise allocation',
  RealPromise.resolve(1).rebindProbe, 1
);
globalThis.Promise = RealPromise;
delete RealPromise.prototype.rebindProbe;

// Cacheability belongs to the semantics at the start of instanceof. A custom
// hook may delete itself while producing its result; that must not let the IC
// stamp the custom result afterward as if ordinary instanceof had produced it.
function SelfDeletingHasInstance() {}
const selfDeletingValue = Object.create(
  Object.create(SelfDeletingHasInstance.prototype)
);
Object.defineProperty(SelfDeletingHasInstance, Symbol.hasInstance, {
  configurable: true,
  value() {
    delete SelfDeletingHasInstance[Symbol.hasInstance];
    return false;
  }
});
function coldSelfDeletingCheck(value) {
  return value instanceof SelfDeletingHasInstance;
}
eq('cold self-deleting hasInstance custom result', coldSelfDeletingCheck(selfDeletingValue), false);
eq('cold self-deleting hasInstance becomes ordinary', coldSelfDeletingCheck(selfDeletingValue), true);
eq('cold self-deleting hasInstance stays ordinary', coldSelfDeletingCheck(selfDeletingValue), true);

function WarmSelfDeletingHasInstance() {}
const warmSelfDeletingValue = Object.create(
  Object.create(WarmSelfDeletingHasInstance.prototype)
);
function warmSelfDeletingCheck(value) {
  return value instanceof WarmSelfDeletingHasInstance;
}
warm(warmSelfDeletingCheck, warmSelfDeletingValue);
Object.defineProperty(WarmSelfDeletingHasInstance, Symbol.hasInstance, {
  configurable: true,
  value() {
    delete WarmSelfDeletingHasInstance[Symbol.hasInstance];
    return false;
  }
});
eq('warm self-deleting hasInstance custom result', warmSelfDeletingCheck(warmSelfDeletingValue), false);
eq('warm self-deleting hasInstance becomes ordinary', warmSelfDeletingCheck(warmSelfDeletingValue), true);
eq('warm self-deleting hasInstance stays ordinary', warmSelfDeletingCheck(warmSelfDeletingValue), true);

if (failed) process.exit(1);
console.log('OK: prototype-write epoch');
