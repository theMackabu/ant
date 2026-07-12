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

function manyCapturedBindings(source) {
  let a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8;
  let i = 9, j = 10, k = 11, l = 12, m = 13, n = 14, o = 15, p = 16, q = 17;
  return eval(source);
}
assert(manyCapturedBindings('a + q') === 18,
  'dynamic eval should support scopes with many captured bindings');

delete globalThis.evalWriteProbe;
delete globalThis.evalStrictWriteProbe;
delete globalThis.evalUndefinedProbe;
delete globalThis.evalCreatedProbe;

console.log('dynamic direct eval environment tests passed');
