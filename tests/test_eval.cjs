const assert = require('node:assert');

// Expressions and return values.
assert.strictEqual(eval('1 + 2'), 3);
assert.strictEqual(eval('10 * 5'), 50);
assert.strictEqual(eval('"Hello" + " " + "World"'), 'Hello World');
assert.strictEqual(eval('1 + 1; 2 + 2; 3 + 3'), 6);
assert.strictEqual(eval('let temp = 5; temp * temp + 10'), 35);

// Literals.
assert.deepStrictEqual(eval('({a: 1, b: 2})'), { a: 1, b: 2 });
assert.deepStrictEqual(eval('[1, 2, 3, 4, 5]'), [1, 2, 3, 4, 5]);

// Non-string arguments come back unchanged; no argument yields undefined.
assert.strictEqual(eval(42), 42);
assert.strictEqual(eval(true), true);
assert.strictEqual(eval(), undefined);

// Direct eval sees and can assign the caller's bindings.
let outerVar = 100;
assert.strictEqual(eval('outerVar + 50'), 150);
let modVar = 10;
eval('modVar = modVar * 2');
assert.strictEqual(modVar, 20);

// Lexical declarations stay scoped to the eval body.
eval('let scoped = 42');
assert.throws(() => scoped, ReferenceError);

// In sloppy mode, var and function declarations leak into the caller's scope.
eval('var leaked = 42');
assert.strictEqual(leaked, 42);
eval('function add(a, b) { return a + b; }');
assert.strictEqual(add(3, 4), 7);

// Indirect eval runs in global scope and cannot see local bindings.
assert.strictEqual((0, eval)('typeof outerVar'), 'undefined');

console.log('eval tests passed');
