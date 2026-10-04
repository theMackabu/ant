import { test, summary } from './helpers.js';

console.log('Module Strictness Tests\n');

test('this in a plain function is undefined', (function () { return this; })(), undefined);
test('assigning an undeclared name throws', (() => { try { undeclared_module_name = 1; return 'ok'; } catch (e) { return e.constructor.name; } })(), 'ReferenceError');
test('writing a frozen property throws', (() => { try { Object.freeze({ a: 1 }).a = 2; return 'ok'; } catch (e) { return e.constructor.name; } })(), 'TypeError');
test('arguments do not alias parameters', (function (a) { arguments[0] = 2; return a; })(1), 1);
test('top-level this is undefined', this, undefined);

test('Function bodies stay sloppy', new Function('return this')() === globalThis, true);
test('indirect eval stays sloppy', (0, eval)('(function () { return this })()') === globalThis, true);
test('with in a sloppy Function body', new Function('with ({ v: 3 }) return v')(), 3);

summary();
