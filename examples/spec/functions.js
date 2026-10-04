import { test, summary } from './helpers.js';

console.log('Function Tests\n');

function add(a, b) {
  return a + b;
}
test('function declaration', add(2, 3), 5);

const multiply = function(a, b) {
  return a * b;
};
test('function expression', multiply(3, 4), 12);

const arrow = (a, b) => a - b;
test('arrow function', arrow(10, 3), 7);

const arrowSingle = x => x * 2;
test('arrow single param', arrowSingle(5), 10);

const arrowBlock = (a, b) => {
  const sum = a + b;
  return sum * 2;
};
test('arrow block body', arrowBlock(2, 3), 10);

function defaultParam(a, b = 10) {
  return a + b;
}
test('default param used', defaultParam(5), 15);
test('default param overridden', defaultParam(5, 3), 8);

function restParams(first, ...rest) {
  return first + rest.length;
}
test('rest params', restParams(1, 2, 3, 4), 4);
test('rest params length excludes rest', restParams.length, 1);
test('rest-only params length is zero', ((...rest) => rest.length).length, 0);

let restSetterRejected = false;
try {
  eval('({ set value(...rest) {} })');
} catch (e) {
  restSetterRejected = true;
}
test('rest params rejected in setter', restSetterRejected, true);

function spreadCall(a, b, c) {
  return a + b + c;
}
test('spread in call', spreadCall(...[1, 2, 3]), 6);

const obj = {
  value: 100,
  getValue() {
    return this.value;
  }
};
test('method this', obj.getValue(), 100);

const arrowThis = {
  value: 50,
  getArrow() {
    return (() => this.value)();
  }
};
test('arrow this binding', arrowThis.getArrow(), 50);

function outer() {
  let x = 10;
  return function inner() {
    return x;
  };
}
test('closure', outer()(), 10);

const counter = (() => {
  let count = 0;
  return {
    inc() { return ++count; },
    get() { return count; }
  };
})();
test('iife closure inc', counter.inc(), 1);
test('iife closure get', counter.get(), 1);

function factorial(n) {
  if (n <= 1) return 1;
  return n * factorial(n - 1);
}
test('recursion', factorial(5), 120);

test('function name', add.name, 'add');
test('function length', add.length, 2);

function bindTest(greeting) {
  return greeting + ' ' + this.name;
}
const boundFn = bindTest.bind({ name: 'World' });
test('bind', boundFn('Hello'), 'Hello World');

test('call', bindTest.call({ name: 'Call' }, 'Hi'), 'Hi Call');
test('apply', bindTest.apply({ name: 'Apply' }, ['Hey']), 'Hey Apply');

const gen = (function() {
  const fns = [];
  for (let i = 0; i < 3; i++) {
    fns.push(() => i);
  }
  return fns;
})();
test('closure in loop 0', gen[0](), 0);
test('closure in loop 1', gen[1](), 1);
test('closure in loop 2', gen[2](), 2);

const AsyncFunction = (async () => {}).constructor;
const GeneratorFunction = (function* () {}).constructor;
const AsyncGeneratorFunction = (async function* () {}).constructor;
test('empty Function source', String(new Function()), 'function anonymous(\n) {\n\n}');
test('empty AsyncFunction source', String(AsyncFunction()), 'async function anonymous(\n) {\n\n}');
test('empty GeneratorFunction source', String(GeneratorFunction()), 'function* anonymous(\n) {\n\n}');
test('empty AsyncGeneratorFunction source', String(AsyncGeneratorFunction()), 'async function* anonymous(\n) {\n\n}');
test('empty Function still callable', new Function()(), undefined);
test('empty AsyncFunction returns a promise', AsyncFunction()() instanceof Promise, true);
test('empty GeneratorFunction makes generators', typeof GeneratorFunction()().next, 'function');

function syntaxError(...args) {
  try { new Function(...args); return 'ok'; }
  catch (e) { return e instanceof SyntaxError ? 'SyntaxError' : String(e); }
}

test('missing statement separator', syntaxError('a b'), 'SyntaxError');
test('missing separator after return value', syntaxError('return 1 2'), 'SyntaxError');
test('missing separator after declaration', syntaxError('var a = 1 var b'), 'SyntaxError');
test('missing separator after class field', syntaxError('class A { a = 1 b = 2 }'), 'SyntaxError');
test('line break ends a statement', syntaxError('a\nb'), 'ok');
test('closing brace ends a statement', syntaxError('{ a } b'), 'ok');
test('do-while needs no separator', syntaxError('do ; while (0) x'), 'ok');
test('unclosed condition', syntaxError('if (a ;'), 'SyntaxError');
test('missing open paren', syntaxError('if a) ;'), 'SyntaxError');
test('stray token in case', syntaxError('switch (x) { case 1 2: }'), 'SyntaxError');
test('keyword as var name', syntaxError('var if = 1'), 'SyntaxError');
test('number as var name', syntaxError('var 1 = 2'), 'SyntaxError');
test('keyword as parameter', syntaxError('function f(default) {}'), 'SyntaxError');
test('let as let name', syntaxError('let let = 1'), 'SyntaxError');
test('let as const name', syntaxError('const let = 1'), 'SyntaxError');
test('let in lexical pattern', syntaxError('let {let} = {}'), 'SyntaxError');
test('let in for-of declaration', syntaxError('for (let let of []);'), 'SyntaxError');
test('let as var name', syntaxError('var let = 1'), 'ok');
test('let as class name', syntaxError('class let {}'), 'SyntaxError');
test('unnamed function statement', syntaxError('function () {}'), 'SyntaxError');
test('contextual words as names', syntaxError('var async, of, static; function yield() {} class await {}'), 'ok');

test('parameters separated by space', syntaxError('a b', ''), 'SyntaxError');
test('parameters closing early', syntaxError('x) { (function(', '})'), 'SyntaxError');
test('body closing the function', syntaxError('})({'), 'SyntaxError');
test('line comment ends a parameter', new Function('a // c', 'return a')(3), 3);
test('line comment ends the body', new Function('return 4 // c')(), 4);
test('Function source keeps the parts', String(new Function('a', 'b', 'return a + b')), 'function anonymous(a,b\n) {\nreturn a + b\n}');
test('Function length', new Function('a', 'b', '').length, 2);
test('Function length with default', new Function('a', 'b = 1', 'c', '').length, 1);
test('empty Function length', new Function().length, 0);
test('Function length is configurable only', JSON.stringify(Object.getOwnPropertyDescriptor(new Function('a', ''), 'length')), '{"value":1,"writable":false,"enumerable":false,"configurable":true}');

test('export in Function body', syntaxError('export const a = 1'), 'SyntaxError');
test('import in Function body', syntaxError('import x from "y"'), 'SyntaxError');
test('nested export', syntaxError('if (1) export default 1'), 'SyntaxError');
test('await in Function body', syntaxError('await 1'), 'SyntaxError');
test('for await in Function body', syntaxError('for await (const x of []);'), 'SyntaxError');
test('for await in plain function', syntaxError('function f() { for await (const x of []); }'), 'SyntaxError');
test('for await in async function', syntaxError('async function f() { for await (const x of []); }'), 'ok');
test('for await in eval', (() => { try { eval('for await (const x of []);'); return 'ok'; } catch (e) { return e.constructor.name; } })(), 'SyntaxError');

function indirectEval(src) {
  try { return (0, eval)(src); }
  catch (e) { return e instanceof SyntaxError ? 'SyntaxError' : String(e); }
}

test('await is a name in scripts', indirectEval('var await = 4; await'), 4);
test('await names a function in scripts', indirectEval('function await() { return 5 } await()'), 5);
test('await as a call in scripts', indirectEval('function await(x) { return x } await (6)'), 6);
test('await is reserved in async functions', indirectEval('async function f() { var await = 1 }'), 'SyntaxError');
test('await cannot name a function in async code', indirectEval('async function f() { function await() {} }'), 'SyntaxError');
test('await cannot name a class in async code', indirectEval('async function f() { class await {} }'), 'SyntaxError');
test('await operand in a plain function', indirectEval('function f() { await x }'), 'SyntaxError');
test('await in a plain arrow inside async', indirectEval('async function f() { () => await 1 }'), 'SyntaxError');
test('await in an async arrow', typeof indirectEval('async x => await x'), 'function');
test('await name in Function body', new Function('var await = 7; return await')(), 7);
test('await operator in AsyncFunction body', (async () => {}).constructor('x', 'return await x') instanceof Function, true);

test('parameters cannot leave the parameter list', syntaxError('a) { return 1 } (function (', ''), 'SyntaxError');
test('body cannot close the function', syntaxError('}; (function () {'), 'SyntaxError');
test('rest parameter must be last', syntaxError('...a, b', ''), 'SyntaxError');
test('trailing comma in parameters', syntaxError('a, b,', ''), 'ok');
test('use strict body applies', (() => { try { new Function('"use strict"; undeclared_strict_fn = 1')(); return 'ok'; } catch (e) { return e.constructor.name; } })(), 'ReferenceError');

test('undefined parameter shadows', new Function('undefined', 'return undefined')(5), 5);
test('undefined var shadows', (function () { var undefined = 7; return undefined; })(), 7);
test('captured undefined shadows', (function () { let undefined = 8; return () => undefined; })()(), 8);
test('eval sees shadowed undefined', (function () { var undefined = 9; return eval('undefined'); })(), 9);
test('with object shadows undefined', new Function('with ({ undefined: 11 }) return undefined')(), 11);
test('globalThis var shadows', (function () { var globalThis = 12; return globalThis; })(), 12);
test('unshadowed undefined', (function () { return undefined; })(), undefined);
test('unshadowed globalThis', (function () { return globalThis; })() === globalThis, true);

summary();
