const assert = require('node:assert');

const misplaced = [
  'class C extends Object { m() { super = 5; } }',
  'class C extends Object { m() { super += 1; } }',
  'class C extends Object { m() { [super] = [1]; } }',
  'class C extends Object { m() { return typeof super; } }',
  'class C extends Object { x = super(); }',
  'class C extends Object { x = () => super(); }',
  'super;',
  'class C extends Object { constructor() { var s = super; } }',
  'class C extends Object { constructor() { new super(); } }',
  'class C extends Object { m() { return super?.x; } }',
  'function f() { return super.x; }',
  'super.x',
  '(() => super.x)',
  'class C { static m() { return super(); } }',
  'class C { m() { super(); } }',
  'class C { constructor() { super(); } }',
  'class C extends Object { constructor() { function g() { return super.x; } super(); } }',
  '({ m: function () { return super.x; } })',
];
for (const source of misplaced) assert.throws(() => new Function(source), SyntaxError, source);

const allowed = [
  'class C extends Object { x = super.toString; constructor() { const call = () => super(); call(); } }',
  'class C extends Object { m() { return typeof super.x + super["y"] + (super.z = 1) + super.q++; } }',
  'class C extends Object { m() { return super.x`a`; } }',
  '({ m() { return super.x; } })',
  'class C extends Object { constructor() { const a = () => super(); a(); } m() { return () => super.x; } static s = super.x; x = super.y; static { super.z; } }',
  'class C { x = () => super.y; static #p() { return super.q; } }',
];
for (const source of allowed) assert.doesNotThrow(() => new Function(source), source);

const dynamic = (source) => eval(String(source));
class EvalScopeBase { x() { return 'x'; } static sx() { return 'sx'; } constructor() { this.made = true; } }
class EvalScope extends EvalScopeBase {
  viaEval() { return eval(String('super.x()')); }
  viaArrow() { return (() => eval(String('super.x()')))(); }
  viaNestedEval() { return eval(String('eval("super.x()")')); }
  viaNestedArrow() { return eval(String('(() => super.x())()')); }
  static viaStatic() { return eval(String('super.sx()')); }
  superCallInMethod() { return eval(String('super()')); }
  functionInEval() { return eval(String('(function () { return super.x; })')); }
}
const evalScope = new EvalScope();
assert.strictEqual(evalScope.viaEval(), 'x');
assert.strictEqual(evalScope.viaArrow(), 'x');
assert.strictEqual(evalScope.viaNestedEval(), 'x');
assert.strictEqual(evalScope.viaNestedArrow(), 'x');
assert.strictEqual(EvalScope.viaStatic(), 'sx');
assert.throws(() => evalScope.superCallInMethod(), SyntaxError);
assert.throws(() => evalScope.functionInEval(), SyntaxError);
assert.strictEqual(new (class extends EvalScopeBase { constructor() { eval(String('super()')); } })().made, true);
assert.throws(() => new (class { constructor() { eval(String('super()')); } })(), SyntaxError);
assert.throws(() => (function () { return eval(String('super.x')); })(), SyntaxError);
assert.throws(() => (function () { return eval(String('super()')); })(), SyntaxError);
assert.throws(() => dynamic('super.x'), SyntaxError);
assert.throws(() => (0, eval)('super.x'), SyntaxError);
assert.strictEqual(({ __proto__: { x: 1 }, m() { return eval(String('super.x')); } }).m(), 1);

let reached = false;
function neverCalled() { return eval("super.x"); }
reached = true;
assert.strictEqual(reached, true);
assert.throws(() => neverCalled(), SyntaxError);
assert.throws(() => eval("super.x"), SyntaxError);

const source = 'super.x';
class NestedFunctionEval extends EvalScopeBase {
  plain() { return (function () { return eval(source); })(); }
  generator() { return (function* () { yield eval(source); })().next(); }
  arrow() { return (() => eval(source))(); }
}
assert.throws(() => new NestedFunctionEval().plain(), SyntaxError);
assert.throws(() => new NestedFunctionEval().generator(), SyntaxError);
assert.strictEqual(typeof new NestedFunctionEval().arrow(), 'function');

assert.throws(() => new Function('class X { static { function h() { return super.x; } } }'), SyntaxError);

console.log('ok');
