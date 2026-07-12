const assert = (condition, message) => {
  if (!condition) throw new Error(message);
};

function sloppyGlobalEval(source) {
  return eval(source);
}

function strictGlobalEval(source) {
  'use strict';
  return eval(source);
}

globalThis.evalWriteProbe = 1;
sloppyGlobalEval('evalWriteProbe = 2');
assert(globalThis.evalWriteProbe === 2,
  'dynamic direct eval should update an existing realm-global binding');

globalThis.evalStrictWriteProbe = 1;
strictGlobalEval('evalStrictWriteProbe = 2');
assert(globalThis.evalStrictWriteProbe === 2,
  'strict dynamic direct eval should update an existing realm-global binding');

globalThis.evalUndefinedProbe = undefined;
assert(sloppyGlobalEval('evalUndefinedProbe') === undefined,
  'an undefined realm-global value should still count as a resolved binding');

globalThis.evalDeleteProbe = 1;
assert(sloppyGlobalEval('delete evalDeleteProbe') === true,
  'dynamic direct eval should report a configurable global deletion');
assert(!('evalDeleteProbe' in globalThis),
  'dynamic direct eval should delete the realm-global property');

delete globalThis.evalCreatedProbe;
sloppyGlobalEval('evalCreatedProbe = 9');
assert(globalThis.evalCreatedProbe === 9,
  'an unresolved sloppy eval assignment should create a realm-global property');

let strictUnresolvedThrew = false;
try {
  strictGlobalEval('evalStrictMissingProbe = 1');
} catch (error) {
  strictUnresolvedThrew = error instanceof ReferenceError;
}
assert(strictUnresolvedThrew,
  'an unresolved strict eval assignment should throw ReferenceError');

Object.prototype.evalInheritedProbe = 1;
sloppyGlobalEval('evalInheritedProbe = 2');
assert(globalThis.evalInheritedProbe === 2,
  'eval assignment should use the realm global as receiver for inherited bindings');
assert(Object.prototype.evalInheritedProbe === 1,
  'eval assignment should not overwrite an inherited data property');
delete globalThis.evalInheritedProbe;
delete Object.prototype.evalInheritedProbe;

Object.prototype.evalInheritedUndefinedProbe = undefined;
assert(sloppyGlobalEval('evalInheritedUndefinedProbe') === undefined,
  'an inherited undefined global value should still count as a resolved binding');
delete Object.prototype.evalInheritedUndefinedProbe;

let setterReceiver;
let setterValue;
Object.defineProperty(globalThis, 'evalSetterProbe', {
  configurable: true,
  set(value) {
    setterReceiver = this;
    setterValue = value;
  },
});
sloppyGlobalEval('evalSetterProbe = 5');
assert(setterReceiver === globalThis && setterValue === 5,
  'eval assignment should invoke global setters with the realm global receiver');
delete globalThis.evalSetterProbe;

function capturedBindingSemantics(source) {
  let mutable = 1;
  const immutable = 2;
  let undefinedLocal;
  return [eval(source), mutable, immutable, undefinedLocal];
}
assert(capturedBindingSemantics('undefinedLocal')[0] === undefined,
  'an undefined captured value should still count as a resolved binding');
assert(capturedBindingSemantics('delete mutable')[0] === false,
  'captured lexical bindings should not be deletable');

function evalBeforeInitialization(source) {
  return eval(source);
  let pending;
}
let tdzReadThrew = false;
try {
  evalBeforeInitialization('pending');
} catch (error) {
  tdzReadThrew = error instanceof ReferenceError;
}
assert(tdzReadThrew,
  'dynamic direct eval should preserve the temporal dead zone');

let tdzTypeofThrew = false;
try {
  evalBeforeInitialization('typeof pending');
} catch (error) {
  tdzTypeofThrew = error instanceof ReferenceError;
}
assert(tdzTypeofThrew,
  'typeof in dynamic direct eval should preserve the temporal dead zone');

let tdzWriteThrew = false;
try {
  evalBeforeInitialization('pending = 1');
} catch (error) {
  tdzWriteThrew = error instanceof ReferenceError;
}
assert(tdzWriteThrew,
  'dynamic direct eval should reject writes before lexical initialization');

let tdzWithFallbackThrew = false;
try {
  evalBeforeInitialization('with ({}) { pending }');
} catch (error) {
  tdzWithFallbackThrew = error instanceof ReferenceError;
}
assert(tdzWithFallbackThrew,
  'with fallback inside eval should preserve the temporal dead zone');

let constAssignmentThrew = false;
try {
  capturedBindingSemantics('immutable = 3');
} catch (error) {
  constAssignmentThrew = error instanceof TypeError;
}
assert(constAssignmentThrew,
  'dynamic direct eval should reject assignment to a captured const binding');

function closureMutationDuringEval(source) {
  let value = 1;
  function update() {
    value = 2;
  }
  eval(source);
  return value;
}
assert(closureMutationDuringEval('update()') === 2,
  'eval should observe mutations made through closures');

function orderedClosureMutation(source) {
  let value = 1;
  function update(next) {
    value = next;
  }
  eval(source);
  return value;
}
assert(orderedClosureMutation('update(2); value = 1') === 1,
  'an eval write after a closure write should win even when restoring the initial value');
assert(orderedClosureMutation('value = 2; update(3)') === 3,
  'a closure write after an eval write should win');
assert(orderedClosureMutation('value = 4; (() => value)()') === 4,
  'closures called by eval should observe earlier eval writes immediately');

function nestedCapturedWrite(source) {
  let nestedLocal = 1;
  const nestedSource = 'nestedLocal = 3';
  eval(source);
  return nestedLocal;
}
assert(nestedCapturedWrite('eval(nestedSource)') === 3,
  'nested dynamic eval should resolve through outer eval environments');

function normalFunctionNestedEval() {
  let innerOnly = 2;
  return eval('innerOnly; typeof outerOnly');
}
function callNormalFunctionFromEval() {
  let outerOnly = 1;
  return eval('outerOnly; normalFunctionNestedEval()');
}
assert(callNormalFunctionFromEval() === 'undefined',
  'eval in a normal callee should not inherit its caller eval environment');

const directEvalReceiver = {
  readThis(source) {
    return eval(source);
  },
};
assert(directEvalReceiver.readThis('this') === directEvalReceiver,
  'direct eval should inherit the caller frame this value');

function createEvalNestedEvalClosure() {
  let retainedValue = 17;
  return eval("() => eval('retainedValue')");
}
assert(createEvalNestedEvalClosure()() === 17,
  'an eval-created closure should retain its environment for nested eval');

function createEscapingEvalClosure() {
  let retainedValue = 19;
  const source = '() => ++retainedValue';
  return eval(source);
}
const escapingEvalClosure = createEscapingEvalClosure();
assert(escapingEvalClosure() === 20 && escapingEvalClosure() === 21,
  'an eval-created closure should retain and mutate caller bindings after return');

function createSharedEvalClosures() {
  let sharedValue = 1;
  const ordinary = () => sharedValue;
  const source = '() => ++sharedValue';
  return [ordinary, eval(source)];
}
const [readSharedEvalValue, updateSharedEvalValue] = createSharedEvalClosures();
assert(updateSharedEvalValue() === 2 && readSharedEvalValue() === 2,
  'eval and ordinary closures should share the same captured binding cell');

function createBlockEvalClosure() {
  let closure;
  {
    let blockValue = 29;
    const source = '() => blockValue';
    closure = eval(source);
  }
  {
    let reusedSlot = 31;
    assert(reusedSlot === 31, 'later block should initialize its local');
  }
  return closure;
}
assert(createBlockEvalClosure()() === 29,
  'an eval-captured block binding should close before its stack slot is reused');

function createGcRetainedEvalClosure() {
  let retainedObject = { marker: 37 };
  const source = '() => retainedObject.marker';
  return eval(source);
}
const gcRetainedEvalClosure = createGcRetainedEvalClosure();
for (let round = 0; round < 4; round++) {
  const churn = [];
  for (let i = 0; i < 100000; i++) churn.push({ i });
}
assert(gcRetainedEvalClosure() === 37,
  'an eval-captured object should remain live across garbage collection');

function createBoundEvalClosure() {
  let boundValue = 23;
  return eval('(() => boundValue).bind(null)');
}
assert(createBoundEvalClosure()() === 23,
  'binding an eval-created closure should preserve its environment');

function manyCapturedBindings(source) {
  let a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8;
  let i = 9, j = 10, k = 11, l = 12, m = 13, n = 14, o = 15, p = 16, q = 17;
  return eval(source);
}
assert(manyCapturedBindings('a + q') === 18,
  'dynamic eval should support scopes with many captured bindings');

globalThis.evalReentrantColdProbe = 1;
function coldOrdinaryGlobalReadWrite() {
  evalReentrantColdProbe = 2;
  globalThis.evalReentrantColdProbe = 3;
  return evalReentrantColdProbe;
}
function callColdOrdinaryGlobalReadWriteFromEval() {
  let evalScopeMarker = true;
  return eval('evalScopeMarker; coldOrdinaryGlobalReadWrite()');
}
assert(callColdOrdinaryGlobalReadWriteFromEval() === 3,
  'interpreted ordinary functions called by eval should use the realm global');
assert(globalThis.evalReentrantColdProbe === 3,
  'interpreted ordinary global writes should not be trapped in the eval environment');

globalThis.evalReentrantJitProbe = 1;
function jitOrdinaryGlobalReadWrite() {
  evalReentrantJitProbe = 2;
  globalThis.evalReentrantJitProbe = 3;
  return evalReentrantJitProbe;
}
for (let i = 0; i < 110; i++) jitOrdinaryGlobalReadWrite();
globalThis.evalReentrantJitProbe = 1;
function callJitOrdinaryGlobalReadWriteFromEval() {
  let evalScopeMarker = true;
  return eval('evalScopeMarker; jitOrdinaryGlobalReadWrite()');
}
assert(callJitOrdinaryGlobalReadWriteFromEval() === 3,
  'JIT ordinary functions called by eval should read and write the realm global');
assert(globalThis.evalReentrantJitProbe === 3,
  'JIT ordinary global writes should not be trapped in the eval environment');

globalThis.evalReentrantStrictProbe = 1;
function strictOrdinaryGlobalWrite() {
  'use strict';
  evalReentrantStrictProbe = 2;
}
for (let i = 0; i < 110; i++) strictOrdinaryGlobalWrite();
globalThis.evalReentrantStrictProbe = 1;
function callStrictOrdinaryGlobalWriteFromEval() {
  let evalScopeMarker = true;
  return eval('evalScopeMarker; strictOrdinaryGlobalWrite()');
}
callStrictOrdinaryGlobalWriteFromEval();
assert(globalThis.evalReentrantStrictProbe === 2,
  'strict ordinary functions called by eval should update existing realm globals');

globalThis.evalReentrantCreatedProbe = 1;
function ordinaryGlobalDelete() {
  return delete evalReentrantCreatedProbe;
}
function callOrdinaryGlobalDeleteFromEval() {
  let evalScopeMarker = true;
  return eval('evalScopeMarker; ordinaryGlobalDelete()');
}
assert(callOrdinaryGlobalDeleteFromEval() === true,
  'ordinary global deletion during eval should report success');
assert(!('evalReentrantCreatedProbe' in globalThis),
  'ordinary global deletion during eval should delete from the realm global');

delete globalThis.evalWriteProbe;
delete globalThis.evalStrictWriteProbe;
delete globalThis.evalUndefinedProbe;
delete globalThis.evalCreatedProbe;
delete globalThis.evalReentrantColdProbe;
delete globalThis.evalReentrantJitProbe;
delete globalThis.evalReentrantStrictProbe;
delete globalThis.evalReentrantCreatedProbe;

console.log('dynamic direct eval environment tests passed');
