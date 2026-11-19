const assert = require('node:assert');

function run(src, ...args) {
  return new Function('s', src)(...args);
}

assert.strictEqual(run("return (() => eval('typeof arguments'))()"), 'object');
assert.strictEqual(run("return (() => eval('arguments.length'))()", 1, 2), 2);
assert.strictEqual(run("return (() => () => eval('arguments[1]'))()()", 1, 7), 7);
assert.strictEqual(run("return eval('(() => arguments.length)()')", 1, 2), 2);
assert.strictEqual(run("return (() => eval('(() => arguments.length)()'))()", 1, 2), 2);
assert.strictEqual(run("return (() => eval('eval(\"arguments.length\")'))()", 1, 2), 2);
assert.strictEqual(run("return ((x = eval('arguments.length')) => x)()", 1, 2), 2);
assert.deepStrictEqual(run("return [1, 2].map(() => eval('arguments.length'))", 1, 2, 3), [3, 3]);
assert.strictEqual(run("return arguments.length + (() => eval('arguments.length'))()", 1, 2), 4);

assert.strictEqual(run("return (() => eval(s))()", 'typeof arguments'), 'object');
assert.strictEqual(run("return (() => eval(s))()", 'arguments.length', 2), 2);
assert.strictEqual(run("return (() => eval(s))() === arguments", 'arguments'), true);

assert.strictEqual(run("'use strict'; return (() => eval(s))()", 'typeof arguments'), 'object');
assert.strictEqual(run("'use strict'; return (() => eval(s))()", 'arguments.length', 2), 2);
assert.strictEqual(run("'use strict'; return (() => () => eval(s))()()", 'arguments[1]', 7), 7);
assert.strictEqual(run("'use strict'; return (() => eval(s))()", '(() => arguments.length)()', 2), 2);
assert.strictEqual(run("'use strict'; return (() => eval(s))()", 'eval("arguments.length")', 2), 2);
assert.strictEqual(run("'use strict'; return (() => eval(s))() === arguments", 'arguments'), true);
assert.strictEqual(run("'use strict'; return (() => eval(...[s]))()", 'arguments.length', 2), 2);
assert.strictEqual(run("return (() => { 'use strict'; return eval(s) })()", 'arguments.length', 2), 2);
assert.strictEqual(run("return (function () { return (() => eval(s))() })(1, 2, 3, 4)", 'arguments.length'), 4);
assert.strictEqual(run("return ({ m() { return (() => eval(s))() } }).m(1, 2, 3)", 'arguments.length'), 3);
assert.strictEqual(run("return new (class { constructor() { this.v = (() => eval(s))() } })(1, 2).v", 'arguments.length'), 2);
assert.strictEqual(run("return function* () { yield (() => eval(s))() }(1, 2, 3).next().value", 'arguments.length'), 3);

assert.strictEqual(run("return ((arguments) => eval(s))(7)", 'arguments'), 7);
assert.strictEqual(run("return (() => { var arguments = 3; return eval(s) })()", 'arguments'), 3);
assert.strictEqual(run("return (() => (0, eval)(s))()", 'typeof arguments'), 'undefined');

assert.strictEqual(run("arguments = 8; return arguments"), 8);
assert.strictEqual(run("arguments++; return arguments"), NaN);
assert.strictEqual(run("[arguments] = [4]; return arguments"), 4);
assert.strictEqual(run("(() => { arguments = 8 })(); return arguments"), 8);
assert.strictEqual(run("return (() => { arguments = 8; return arguments })()"), 8);
assert.strictEqual(run("(() => eval('arguments = 5'))(); return arguments"), 5);
assert.strictEqual(run("(() => eval(s))(); return arguments", 'arguments = 5'), 5);
assert.strictEqual(typeof globalThis.arguments, 'undefined');

assert.strictEqual(run("return delete arguments"), false);
assert.strictEqual(run("return (() => delete arguments)()"), false);
assert.strictEqual(run("return (() => eval(s))()", 'delete arguments'), false);

assert.strictEqual(run("var arguments; return typeof arguments"), 'object');
assert.strictEqual(run("var arguments = 3; return arguments"), 3);
assert.strictEqual(run("return typeof arguments; function arguments() {}"), 'function');
assert.strictEqual(run("let arguments = 3; return arguments"), 3);
assert.strictEqual(run("return (function (arguments) { return (() => arguments)() })(5)"), 5);
assert.strictEqual(run("return (function (a, b = () => arguments.length) { return b() })(1)"), 1);
assert.strictEqual(run("arguments; return (function () { return (() => arguments.length)() })(1, 2, 3)"), 3);
assert.strictEqual(run("arguments; return (function () { return arguments.length })(1, 2, 3)"), 3);

assert.strictEqual(typeof arguments, 'object');
assert.strictEqual((() => typeof arguments)(), 'object');
assert.strictEqual((() => arguments.length)(), 5);

console.log('ok');
