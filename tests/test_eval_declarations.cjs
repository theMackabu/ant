const assert = require('node:assert');

function declarations(source) {
  assert.strictEqual(typeof evalVar, 'undefined');
  const before = () => evalVar;
  eval(source);
  assert.strictEqual(evalVar, 42);
  assert.strictEqual(before(), 42);
  assert.strictEqual(evalFunction(3, 4), 7);
  eval('var evalVar;');
  assert.strictEqual(evalVar, 42);
  eval('evalVar = 43;');
  assert.strictEqual(before(), 43);
  return () => ++evalVar;
}
const escaped = declarations('var evalVar = 42; function evalFunction(a, b) { return a + b; }');
assert.strictEqual(escaped(), 44);
assert.strictEqual(globalThis.evalVar, undefined);
assert.strictEqual(globalThis.evalFunction, undefined);
const other = declarations('var evalVar = 42; function evalFunction(a, b) { return a + b; }');
assert.strictEqual(other(), 44);
assert.strictEqual(escaped(), 45);

function existing(parameter) {
  var value = 1;
  eval('var parameter = 2; var value = 3;');
  assert.strictEqual(parameter, 2);
  assert.strictEqual(arguments[0], 2);
  assert.strictEqual(value, 3);
}
existing(0);

let outer = 10;
function shadow() {
  const before = () => outer;
  assert.strictEqual(before(), 10);
  eval('var outer = 20;');
  assert.strictEqual(before(), 20);
  assert.strictEqual(outer, 20);
  return () => outer;
}
assert.strictEqual(shadow()(), 20);
assert.strictEqual(outer, 10);

function collisions() {
  let mutable = 1;
  const immutable = 2;
  class LocalClass {}
  for (const name of ['mutable', 'immutable', 'LocalClass']) {
    for (const source of [`var ${name};`, `function ${name}() {}`]) {
      let error;
      try { eval(source); } catch (caught) { error = caught; }
      assert.ok(error instanceof SyntaxError);
    }
  }
  assert.strictEqual(mutable, 1);
  assert.strictEqual(immutable, 2);
  let sideEffect = false;
  let error;
  try { eval('sideEffect = true; var mutable;'); } catch (caught) { error = caught; }
  assert.ok(error instanceof SyntaxError);
  assert.strictEqual(sideEffect, false);
}
collisions();

function blockCollision() {
  {
    let blocked = 1;
    let error;
    try { eval('var blocked = 2'); } catch (caught) { error = caught; }
    assert.ok(error instanceof SyntaxError);
    assert.strictEqual(blocked, 1);
  }
  eval('var blocked = 3');
  assert.strictEqual(blocked, 3);
}
blockCollision();

function strictCaller() {
  'use strict';
  eval('var isolated = 1; function isolatedFunction() {}');
  assert.strictEqual(typeof isolated, 'undefined');
  assert.strictEqual(typeof isolatedFunction, 'undefined');
}
strictCaller();
function strictBody() {
  eval('"use strict"; var isolated = 1; function isolatedFunction() {}');
  assert.strictEqual(typeof isolated, 'undefined');
  assert.strictEqual(typeof isolatedFunction, 'undefined');
  eval('let lexical = 1; const constant = 2; class Class {}');
  assert.strictEqual(typeof lexical, 'undefined');
  assert.strictEqual(typeof constant, 'undefined');
  assert.strictEqual(typeof Class, 'undefined');
}
strictBody();

function hoisting() {
  let error;
  try { eval('throw new Error("stop"); var hoisted; function ready() { return 7; }'); }
  catch (caught) { error = caught; }
  assert.strictEqual(error.message, 'stop');
  assert.strictEqual(hoisted, undefined);
  assert.strictEqual(ready(), 7);
  eval('var [first, second] = [1, 2]; var {third} = {third: 3};');
  assert.strictEqual(first + second + third, 6);
  eval('for (var index = 0; index < 3; index++) {}');
  assert.strictEqual(index, 3);
  eval('for (var item of [2, 4]) {} for (var key in {a: 1}) {}');
  assert.strictEqual(item, 4);
  assert.strictEqual(key, 'a');
}
hoisting();

function nested() {
  eval('eval("var inner = 8;");');
  assert.strictEqual(inner, 8);
  eval('var read = () => inner; let own = 9; var readLexical = () => own;');
  assert.strictEqual(read(), 8);
  assert.strictEqual(readLexical(), 9);
  assert.strictEqual(typeof own, 'undefined');
}
nested();

function blockFunction() {
  eval('{ function blockLeaked() { return 9; } }');
  assert.strictEqual(blockLeaked(), 9);
}
blockFunction();

function loops() {
  for (let loop = 0; loop < 1; loop++) {
    let error;
    try { eval('var loop;'); } catch (caught) { error = caught; }
    assert.ok(error instanceof SyntaxError);
  }
  const readers = [];
  for (let i = 0; i < 3; i++) {
    readers.push(() => [i, shared]);
    eval('var shared = 10;');
  }
  assert.deepStrictEqual(readers.map(read => read()), [[0, 10], [1, 10], [2, 10]]);
}
loops();

function catchBinding() {
  try { throw 1; } catch (caught) {
    eval('var caught = 2;');
    assert.strictEqual(caught, 2);
  }
  assert.strictEqual(caught, undefined);
  try { throw 3; } catch (fn) {
    eval('function fn() { return 4; }');
    assert.strictEqual(fn, 3);
  }
  assert.strictEqual(fn(), 4);
  try { throw {lexicalCatch: 1}; } catch ({lexicalCatch}) {
    let error;
    try { eval('var lexicalCatch;'); } catch (caught) { error = caught; }
    assert.ok(error instanceof SyntaxError);
  }
}
catchBinding();

function parameters({value}, other = 1) {
  eval('var value = 5; var other = 6;');
  return value + other;
}
assert.strictEqual(parameters({value: 0}), 11);

function lexicalEvalCallee() {
  const eval = value => value + 1;
  return function () { return eval(4); };
}
assert.strictEqual(lexicalEvalCallee()(), 5);

function removable() {
  eval('var disposable = 1;');
  assert.strictEqual(delete disposable, true);
  assert.strictEqual(typeof disposable, 'undefined');
  eval('var disposable = 2;');
  return disposable;
}
assert.strictEqual(removable(), 2);

function hotShadow() {
  const read = () => outer;
  for (let i = 0; i < 1000; i++) assert.strictEqual(read(), 10);
  eval('var outer = 99;');
  for (let i = 0; i < 1000; i++) assert.strictEqual(read(), 99);
}
hotShadow();

const retained = [];
for (let i = 0; i < 1000; i++) {
  retained.push((function (value) {
    eval('var retainedValue = value;');
    return () => retainedValue;
  })(i));
}
for (let i = 0; i < retained.length; i++) assert.strictEqual(retained[i](), i);

function annexBConflicts() {
  let blockedFunction = 1;
  eval('{ function blockedFunction() {} }');
  assert.strictEqual(blockedFunction, 1);
  eval('if (true) function blockedFunction() {}');
  assert.strictEqual(blockedFunction, 1);
  eval('let evalLexical = 1; { function evalLexical() {} }');
  assert.strictEqual(typeof evalLexical, 'undefined');
  let error;
  try { eval('let conflict = 1; var conflict;'); } catch (caught) { error = caught; }
  assert.ok(error instanceof SyntaxError);
}
annexBConflicts();

function evalArguments(value) {
  const original = arguments;
  const read = () => arguments;
  eval('var arguments;');
  assert.strictEqual(arguments, original);
  assert.strictEqual(arguments[0], value);
  assert.strictEqual((() => eval('arguments[0]'))(), value);
  eval('var arguments = 3;');
  assert.strictEqual(arguments, 3);
  assert.strictEqual(read(), 3);
}
evalArguments(17);

function nestedLiteralEval(source) {
  eval('eval(source)');
  return nestedLiteralVar;
}
assert.strictEqual(nestedLiteralEval('var nestedLiteralVar = 8;'), 8);

(0, eval)('var indirectEvalDeclaration = 12;');
assert.strictEqual(globalThis.indirectEvalDeclaration, 12);
delete globalThis.indirectEvalDeclaration;
console.log('eval declaration tests passed');
