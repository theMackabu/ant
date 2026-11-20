const assert = require('node:assert');

function outcome(src) {
  try { new Function(src); } catch (e) { return e.constructor.name; }
  return 'ok';
}

const rejected = [
  'class A { x = arguments }',
  'class A { static x = arguments }',
  'class A { #x = arguments }',
  'class A { static #x = arguments }',
  'class A { [k] = arguments }',
  'class A { x = () => arguments }',
  'class A { x = async () => arguments }',
  'class A { x = () => () => arguments.length }',
  'class A { x = [arguments] }',
  'class A { x = { arguments } }',
  'class A { x = { a: arguments } }',
  'class A { x = { [arguments]: 1 } }',
  'class A { x = arguments.length }',
  'class A { x = typeof arguments }',
  'class A { x = f(arguments) }',
  'class A { x = `${arguments}` }',
  'class A { x = (1, arguments) }',
  'class A { x = y ? arguments : 0 }',
  'class A { x = class { [arguments]() {} } }',
  'class A { x = class extends arguments {} }',
  'class A { x = class { y = arguments } }',
  'class A { static { arguments } }',
  'class A { static { arguments.length } }',
  'class A { static { var a = arguments } }',
  'class A { static { () => arguments } }',
  'class A { static { { arguments } } }',
  'class A { static { if (x) arguments } }',
  'class A { static { class B { [arguments] = 1 } } }',
  'function f() { class A { x = arguments } }',
  'function f() { return () => class { x = arguments } }',
  'class A { m() { class B { x = arguments } } }',
];

const accepted = [
  'class A { [arguments] = 1 }',
  'class A { static [arguments.length] = 1 }',
  'class A { [arguments]() {} }',
  'class A { x = function () { return arguments } }',
  'class A { x = function* () { yield arguments } }',
  'class A { x = async function () { return arguments } }',
  'class A { x = { m() { return arguments } } }',
  'class A { x = { get g() { return arguments }, set g(v) { arguments } } }',
  'class A { x = { async m() { return arguments }, *n() { arguments } } }',
  'class A { x = class { m() { return arguments } } }',
  'class A { x = class { static m() { return arguments } constructor() { arguments } } }',
  'class A { x = this.arguments }',
  'class A { x = a.b.arguments }',
  'class A { x = a?.arguments }',
  'class A { x = { arguments: 1 } }',
  'class A { x = { "arguments": 1 }; arguments = 2; static arguments = 3 }',
  'class A { arguments() { return arguments } }',
  'class A { x = () => function () { return arguments } }',
  'class A { static { function f() { return arguments } } }',
  'class A { static { ({ m() { arguments } }) } }',
  'class A { static { this.arguments = 1 } }',
  'class A { m() { return arguments } static s() { return () => arguments } }',
  'class A { constructor() { arguments } x = 1 }',
  'class A { x = 1 } arguments',
  'class A extends (arguments, Object) { x = 1 }',
  '(() => arguments); class A { x = 1 }',
  'class A { x = (function () { return class { [arguments.length] = 1 } })() }',
];

const wrong = [];
for (const src of rejected) if (outcome(src) !== 'SyntaxError') wrong.push('accepted: ' + src);
for (const src of accepted) if (outcome(src) !== 'ok') wrong.push('rejected: ' + src);
assert.deepStrictEqual(wrong, []);

function outer() {
  class A {
    x = function () { return arguments.length; };
    static y = function () { return arguments[0]; };
    [arguments.length] = 'key';
    arguments = 5;
  }
  const a = new A();
  return [a.x(1, 2, 3), A.y('s'), a[2], a.arguments];
}
assert.deepStrictEqual(outer(1, 2), [3, 's', 'key', 5]);

try {
  new Function('class A { x = arguments }');
  assert.fail('expected a SyntaxError');
} catch (e) {
  assert.ok(e instanceof SyntaxError);
  assert.match(e.message, /arguments/);
}

console.log('ok');
