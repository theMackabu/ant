const assert = require('node:assert');

function parameterShadowing() {
  const value = 17;
  function middle(eval, source) {
    eval(source);
    return () => value;
  }
  return middle(source => assert.strictEqual(source, 'ignored'), 'ignored');
}
assert.strictEqual(parameterShadowing()(), 17);

function defaultParameterShadowing() {
  const value = 23;
  function middle(eval = source => source, result = eval('ignored')) {
    assert.strictEqual(result, 'ignored');
    return () => value;
  }
  return middle();
}
assert.strictEqual(defaultParameterShadowing()(), 23);

function enclosingShadowing(eval) {
  const value = 31;
  function middle(source) {
    eval(source);
    return () => value;
  }
  return middle('ignored');
}
assert.strictEqual(enclosingShadowing(source => source)(), 31);

function spreadEval() {
  const value = 47;
  function middle(source) {
    eval(...[source]);
    return () => value;
  }
  return middle('void 0; void 0;');
}
assert.strictEqual(spreadEval()(), 47);


for (const [params, body, args] of [
  ['{eval}, source', 'eval(source);', [{eval: x => x}, 'ignored']],
  ['[eval], source', 'eval(source);', [[x => x], 'ignored']],
  ['{eval = x => x} = {}, source = "ignored"', 'eval(source);', []],
  ['...{0: eval}', 'eval(arguments[1]);', [x => x, 'ignored']],
  ['source', 'var eval = x => x; eval(source);', ['ignored']],
  ['source', 'let eval = x => x; eval(source);', ['ignored']],
  ['source', 'function eval(x) { return x; } eval(source);', ['ignored']],
  ['source', '{ const eval = x => x; eval(source); }', ['ignored']],
  ['source', '{ var eval = x => x; } eval(source);', ['ignored']],
  ['source', 'for (let eval = x => x; eval;) { eval(source); break; }', ['ignored']],
  ['source', 'try { throw x => x; } catch (eval) { eval(source); }', ['ignored']],
  ['source', 'switch (0) { case 0: let eval = x => x; eval(source); }', ['ignored']],
]) {
  const outer = Function(`const value = 53; return function middle(${params}) { ${body} return () => value; };`);
  assert.strictEqual(outer()(...args)(), 53, `${params}: ${body}`);
}

function parameterEvalBeforeBodyBinding(value = eval('var parameterValue = 7; parameterValue')) {
  var eval = x => x;
  return value;
}
assert.strictEqual(parameterEvalBeforeBodyBinding(), 7);

function evalOutsideShadowedBlock(source) {
  { let eval = x => x; eval('ignored'); }
  eval(source);
  return () => introduced;
}
assert.strictEqual(evalOutsideShadowedBlock('var introduced = 61;')(), 61);

function evalInSwitchDiscriminant(source) {
  switch (eval(source)) {
    case 1: let eval = x => x; break;
  }
  return fromDiscriminant;
}
assert.strictEqual(evalInSwitchDiscriminant('var fromDiscriminant = 73; 1;'), 73);

console.log('eval eligibility tests passed');
