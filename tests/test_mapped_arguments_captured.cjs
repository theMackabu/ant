const assert = require('node:assert');

const run = (source) => (0, eval)(source);

assert.strictEqual(run('(function f(a) { class C { m() { return a; } } arguments[0] = 9; return new C().m(); })(1)'), 9);
assert.strictEqual(run('(function f(a) { const g = () => { a = 5; }; g(); return arguments[0]; })(1)'), 5);
assert.strictEqual(run('(function f(a) { (function () { a = 6; })(); return arguments[0]; })(1)'), 6);
assert.strictEqual((function f(a) { class C { m() { return a; } } arguments[0] = 9; return new C().m(); })(1), 9);

console.log('ok');
