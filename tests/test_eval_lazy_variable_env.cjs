const assert = require('node:assert');

function run(src, ...args) {
  return new Function('s', 'a', src)(...args);
}

assert.strictEqual(run("if (false) eval(s); return typeof arguments + arguments.length", 'x', 1), 'object2');
assert.strictEqual(run("if (false) eval(s); arguments = 4; return arguments", 'x'), 4);
assert.strictEqual(typeof globalThis.arguments, 'undefined');
assert.strictEqual(run("if (false) eval(s); return (() => arguments.length)()", 'x', 1), 2);
assert.strictEqual(run("if (false) eval(s); return typeof missingName + typeof console", 'x'), 'undefinedobject');
assert.strictEqual(run("if (false) eval(s); try { return missingName } catch (e) { return e.name }", 'x'), 'ReferenceError');

assert.strictEqual(run("eval(s); return typeof q + typeof arguments", 'var q = 1'), 'numberobject');
assert.strictEqual(run("var g = function () { return typeof q }; eval(s); return g()", 'var q = 1'), 'number');
assert.strictEqual(run("var g = () => q; eval(s); return g()", 'var q = 3'), 3);
assert.strictEqual(run("var b = 1; eval(s); return b + c", 'var c = 2; b = 10'), 12);
assert.strictEqual(run("eval(s); return f()", 'function f() { return 7 }'), 7);
assert.strictEqual(run("return eval(s) + eval(s)", 'var k = (typeof k === "undefined" ? 0 : k) + 1; k'), 3);
assert.strictEqual(typeof globalThis.q, 'undefined');
assert.strictEqual(typeof globalThis.k, 'undefined');

assert.strictEqual(run("a = 5; return eval(s)", 'arguments[1]', 2), 5);
assert.strictEqual(run("eval(s); return a", 'arguments[1] = 9', 2), 9);
assert.strictEqual(new Function('a', 's = 1', "a = 5; return eval(s)")(2, 'arguments[0]'), 2);
assert.strictEqual(new Function('a', '...r', "a = 5; return eval(r[0])")(2, 'arguments[0]'), 2);
assert.strictEqual(run("var f = function () { return eval(s) }; return f(1, 2, 3)", 'arguments.length'), 3);
assert.strictEqual(run("var f = () => eval(s); a = 4; return f()", 'arguments[1]', 2), 4);
assert.strictEqual(run("return a ? arguments.callee(s, a - 1) + eval(s) : 0", 'arguments[1]', 3), 6);
assert.strictEqual(run("let r = []; for (let i = 0; i < 3; i++) r.push(eval(s)); return r.join()", 'arguments.length + i', 0), '2,3,4');
assert.strictEqual(run("try { throw 1 } catch (e) { return eval(s) }", 'e + arguments.length', 0), 3);

function hot(s, x) { if (x < 0) return eval(s); return x + 1 }
let total = 0;
for (let i = 0; i < 20000; i++) total += hot('x', i);
assert.strictEqual(total, 200010000);
assert.strictEqual(hot('var z = 4; z + x', -1), 3);

console.log('ok');
