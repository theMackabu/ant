import { test, summary } from './helpers.js';
import { parseJavaScript } from 'ant:syntax';

console.log('Module Strictness Tests\n');

test('this in a plain function is undefined', (function () { return this; })(), undefined);
test('assigning an undeclared name throws', (() => { try { undeclared_module_name = 1; return 'ok'; } catch (e) { return e.constructor.name; } })(), 'ReferenceError');
test('writing a frozen property throws', (() => { try { Object.freeze({ a: 1 }).a = 2; return 'ok'; } catch (e) { return e.constructor.name; } })(), 'TypeError');
test('arguments do not alias parameters', (function (a) { arguments[0] = 2; return a; })(1), 1);
test('top-level this is undefined', this, undefined);

test('Function bodies stay sloppy', new Function('return this')() === globalThis, true);
test('indirect eval stays sloppy', (0, eval)('(function () { return this })()') === globalThis, true);
test('with in a sloppy Function body', new Function('with ({ v: 3 }) return v')(), 3);

function parsesAs(src, sourceType) {
  try { return parseJavaScript(src, sourceType ? { sourceType } : undefined).sourceType; }
  catch (e) { return e instanceof SyntaxError ? 'SyntaxError' : String(e); }
}

test('await cannot name a function in a module', parsesAs('function await() {}', 'module'), 'SyntaxError');
test('await cannot name a class in a module', parsesAs('class await {}', 'module'), 'SyntaxError');
test('with is a syntax error in a module', parsesAs('with ({}) {}', 'module'), 'SyntaxError');
test('unambiguous import is a module', parsesAs('import x from "x"'), 'module');
test('unambiguous top-level await is a module', parsesAs('await Promise.resolve(1)'), 'module');
test('unambiguous plain code is a script', parsesAs('var a = 1'), 'script');

summary();
