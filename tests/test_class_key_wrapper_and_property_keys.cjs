const assert = require('node:assert');

function keyAssign(a) { class C { [a = 9]() {} } return [a, arguments[0]]; }
function keyArrow(a) { class C { [(() => a = 9)()]() {} } return [a, arguments[0]]; }
function heritageAssign(a) { class C extends (a = 9, Object) {} return [a, arguments[0]]; }
function keyUpdate(a) { class C { [a++]() {} } return [a, arguments[0]]; }
function methodWrite(a) { class C { m() { a = 9; } } new C().m(); return [a, arguments[0]]; }
function staticFieldWrite(a) { class C { static x = (a = 9); } return [a, arguments[0]]; }
function staticBlockWrite(a) { class C { static { a = 9; } } return [a, arguments[0]]; }
function argumentsWrite(a) { class C { static s() { a = 3; } } C.s(); arguments[0] = 5; return [a, arguments[0]]; }
function strictCopy(a) { 'use strict'; class C { static { a = 9; } } return [a, arguments[0]]; }
function twoParams(a, b) { class C { static { b = 4; } m() { return () => a++; } } new C().m()(); return [a, b, arguments[0], arguments[1]]; }

function topFrameOfKey() { class C { [(x = new Error('e'), 'k')]() {} } var x; return x.stack.split('\n')[1]; }
function topFrameOfHeritage() { var x; class C extends (x = new Error('e'), Object) {} return x.stack.split('\n')[1]; }
function thrower() { throw new Error('t'); }
function framesOfCallee() {
  try {
    class C { [(globalThis.__wrapperProbe = 1, thrower())]() {} }
  } catch (error) {
    return error.stack.split('\n').slice(1, 3);
  }
}

for (let round = 0; round < 400; round++) {
  assert.deepStrictEqual(keyAssign(1), [9, 9]);
  assert.deepStrictEqual(keyArrow(1), [9, 9]);
  assert.deepStrictEqual(heritageAssign(1), [9, 9]);
  assert.deepStrictEqual(keyUpdate(1), [2, 2]);
  assert.deepStrictEqual(methodWrite(1), [9, 9]);
  assert.deepStrictEqual(staticFieldWrite(1), [9, 9]);
  assert.deepStrictEqual(staticBlockWrite(1), [9, 9]);
  assert.deepStrictEqual(argumentsWrite(1), [5, 5]);
  assert.deepStrictEqual(strictCopy(1), [9, 1]);
  assert.deepStrictEqual(twoParams(1, 2), [2, 4, 2, 4]);

  assert.match(topFrameOfKey(), /^\s+at topFrameOfKey /);
  assert.match(topFrameOfHeritage(), /^\s+at topFrameOfHeritage /);
  const frames = framesOfCallee();
  assert.match(frames[0], /^\s+at thrower /);
  assert.match(frames[1], /^\s+at framesOfCallee /);
}

function nestedGenerator() {
  class C { [(function* () { yield 1; }, undeclaredFromClassKey = 1, 'k')]() {} }
  return 1;
}
assert.throws(nestedGenerator, ReferenceError);
assert.strictEqual(typeof undeclaredFromClassKey, 'undefined');
function nestedFunctionIsStrict() {
  class C extends ((function () { undeclaredFromHeritageFunction = 1; })(), Object) {}
}
assert.throws(nestedFunctionIsStrict, ReferenceError);

function show(v) {
  if (typeof v === 'function') return 'function ' + v.name;
  if (typeof v === 'symbol' || typeof v === 'bigint') return String(v);
  if (v === undefined) return 'undefined';
  try { return JSON.stringify(v) } catch (e) { return 'unserializable' }
}

function outcome(src) {
  let fn;
  try { fn = new Function(src) } catch (e) { return 'compile ' + e.constructor.name }
  try { return 'value ' + show(fn()) } catch (e) { return 'throw ' + (e && e.constructor ? e.constructor.name : String(e)) }
}

const cases = [
  ["return Object.keys(class { static 08 = 1 })", "compile SyntaxError"],
  ["return Object.getOwnPropertyNames(class { static 08() {} }).sort()", "compile SyntaxError"],
  ["return Object.getOwnPropertyNames((class { 08() {} }).prototype)", "compile SyntaxError"],
];

const failed = [];
for (const [src, want] of cases) {
  const got = outcome(src);
  if (got !== want) failed.push(`${src} => ${got}, want ${want}`);
}
assert.deepStrictEqual(failed, []);

console.log('ok');
