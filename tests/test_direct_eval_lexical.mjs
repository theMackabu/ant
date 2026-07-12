const assert = (condition, message) => {
  if (!condition) throw new Error(message);
};

function createLifter() {
  let parentValue = 'before';
  const lexicalFunction = () => 42;
  return {
    lifter: data => eval(data.eval),
    read: () => parentValue,
  };
}

const { lifter, read } = createLifter();
assert(lifter({ eval: 'parentValue' }) === 'before', 'dynamic direct eval should read a captured lexical binding');
assert(lifter({ eval: 'lexicalFunction' })() === 42, 'dynamic direct eval should read a captured function');

lifter({ eval: 'parentValue=data.value', value: 'after' });
assert(read() === 'after', 'dynamic direct eval should write a mutable captured binding');

const parameterLifter = data => eval(data.eval);
assert(parameterLifter({ eval: 'data.value', value: 7 }) === 7, 'dynamic direct eval should read a parameter binding');

function createEvalClosure() {
  let value = 1;
  return {
    read: () => value,
    write: eval('(next) => value = next'),
  };
}
const evalClosure = createEvalClosure();
assert(evalClosure.write(9) === 9 && evalClosure.read() === 9,
  'functions created by direct eval should retain captured caller bindings');

const shadowedEval = new Function('eval', 'return eval("value")');
assert(shadowedEval(value => `shadowed:${value}`) === 'shadowed:value',
  'a lexical binding named eval should remain an ordinary call');

assert(eval() === undefined, 'eval with no arguments should return undefined');
const nonString = { value: 9 };
assert(eval(nonString) === nonString, 'eval should return a non-string argument unchanged');

function evalAtDistinctBlockSites(source, useLeft) {
  if (useLeft) {
    let left = 11;
    return eval(source);
  }
  let right = 22;
  return eval(source);
}
assert(evalAtDistinctBlockSites('left', true) === 11,
  'direct eval should see a block binding at its exact call site');
assert(evalAtDistinctBlockSites('right', false) === 22,
  'multiple eval sites should retain distinct lexical scopes');

function evalShadowedBlockBinding(source) {
  let value = 'outer';
  {
    let value = 'inner';
    return eval(source);
  }
}
assert(evalShadowedBlockBinding('value') === 'inner',
  'direct eval scope indexing should preserve the innermost visible binding');

const staticBlockSource = 'staticBlockValue';
class StaticBlockEval {
  static {
    let staticBlockValue = 33;
    this.value = eval(staticBlockSource);
  }
}
assert(StaticBlockEval.value === 33,
  'direct eval metadata should be initialized for synthetic static block functions');

const staticFieldSource = 'staticFieldOuter';
const staticFieldOuter = 44;
class StaticFieldEval {
  static value = eval(staticFieldSource);
}
assert(StaticFieldEval.value === 44,
  'direct eval metadata should be initialized for synthetic static field functions');

console.log('dynamic direct eval lexical tests passed');
