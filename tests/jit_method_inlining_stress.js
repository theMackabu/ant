function fail(label, expected, actual) {
  throw new Error(label + ": expected " + expected + ", got " + actual);
}

function assertEq(actual, expected, label) {
  if (actual !== expected) fail(label, expected, actual);
}

function warm(fn, n) {
  for (let i = 0; i < (n || 300); i++) fn(i);
}

// Basic direct-slot load/add/store through a prototype method.
function Seq(start) {
  this.item = start;
}

Seq.prototype.next = function() {
  const old = this.item;
  this.item = old + 2;
  return old;
};

function runSeqLoop(n) {
  const seq = new Seq(1);
  let out = 0;
  for (let i = 0; i < n; i++) out = seq.next();
  return out + seq.item;
}

for (let i = 0; i < 200; i++) runSeqLoop(8);
assertEq(runSeqLoop(1000), 4000, "hot loop inline result");

function callSeq(seq) {
  return seq.next();
}

const seq = new Seq(1);
warm(function() { callSeq(seq); });
assertEq(callSeq(seq), 601, "basic inline result");
assertEq(seq.item, 603, "basic inline store");

// Same call site, different receiver shape and different property slot order.
function SlotA() {
  this.item = 1;
}

SlotA.prototype.next = Seq.prototype.next;

function callSlotA(obj) {
  return obj.next();
}

const slotA = new SlotA();
warm(function() { callSlotA(slotA); });

const slotB = { padding: 99, item: 10 };
Object.setPrototypeOf(slotB, SlotA.prototype);
assertEq(callSlotA(slotB), 10, "receiver shape fallback result");
assertEq(slotB.item, 12, "receiver shape fallback store");
assertEq(slotB.padding, 99, "receiver shape fallback preserves other slot");

// Prototype method identity changes after the call site has warmed.
function Replaceable(start) {
  this.item = start;
}

Replaceable.prototype.next = function() {
  const old = this.item;
  this.item = old + 1;
  return old;
};

function callReplaceable(obj) {
  return obj.next();
}

const replaceable = new Replaceable(5);
warm(function() { callReplaceable(replaceable); });
const replaceableBefore = replaceable.item;
Replaceable.prototype.next = function() {
  return this.item + 1000;
};
assertEq(callReplaceable(replaceable), replaceableBefore + 1000, "prototype method replacement");
assertEq(replaceable.item, replaceableBefore, "prototype replacement does not run old body");

// Own method shadowing changes the receiver shape and callee identity.
function Shadowed(start) {
  this.item = start;
}

Shadowed.prototype.next = Seq.prototype.next;

function callShadowed(obj) {
  return obj.next();
}

const shadowed = new Shadowed(7);
warm(function() { callShadowed(shadowed); });
shadowed.next = function() {
  return this.item * 10;
};
assertEq(callShadowed(shadowed), shadowed.item * 10, "own method shadow fallback");

// Accessor conversion after warmup must not use stale direct slot metadata.
function AccessorBox(start) {
  this.item = start;
}

AccessorBox.prototype.next = Seq.prototype.next;

function callAccessor(obj) {
  return obj.next();
}

const accessor = new AccessorBox(3);
warm(function() { callAccessor(accessor); });
let backing = 20;
Object.defineProperty(accessor, "item", {
  get() { return backing; },
  set(v) { backing = v + 100; },
  configurable: true
});
assertEq(callAccessor(accessor), 20, "accessor fallback getter result");
assertEq(backing, 122, "accessor fallback setter result");

// A callee with a side effect before a later shape-dependent read should not be
// inlined; otherwise a late guard failure would rerun the method and duplicate
// the side effect.
function LateGuardBox() {
  this.count = 0;
}

LateGuardBox.prototype.sideThenRead = function(other) {
  this.count = this.count + 1;
  return other.value;
};

function callLateGuard(box, other) {
  return box.sideThenRead(other);
}

const late = new LateGuardBox();
const monoOther = { value: 1 };
warm(function() { callLateGuard(late, monoOther); });
const lateBefore = late.count;
const otherShape = { padding: 0, value: 7 };
assertEq(callLateGuard(late, otherShape), 7, "late guard fallback result");
assertEq(late.count, lateBefore + 1, "late guard does not duplicate side effect");

// Argument mapping and branch returns in an inlineable method body.
function Accum(start) {
  this.item = start;
}

Accum.prototype.addScaled = function(delta, scale) {
  if (delta < 0) return this.item;
  const old = this.item;
  this.item = old + delta * scale;
  return this.item;
};

function callAddScaled(obj, delta, scale) {
  return obj.addScaled(delta, scale);
}

const accum = new Accum(2);
warm(function() { callAddScaled(accum, 1, 3); });
const accumBefore = accum.item;
assertEq(callAddScaled(accum, 2, 5), accumBefore + 10, "argument mapping result");
assertEq(callAddScaled(accum, -1, 5), accumBefore + 10, "branch return result");

// Class method path, including class-created prototype methods.
class ClassSeq {
  constructor(start) {
    this.item = start;
  }

  next() {
    const old = this.item;
    this.item = old + 4;
    return old;
  }
}

function callClassSeq(obj) {
  return obj.next();
}

const classSeq = new ClassSeq(11);
warm(function() { callClassSeq(classSeq); });
assertEq(callClassSeq(classSeq), 1211, "class inline result");
assertEq(classSeq.item, 1215, "class inline store");

console.log("jit method inlining stress: ok");
