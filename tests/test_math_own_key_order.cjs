const assert = require('node:assert');

{
  const mathKeys = Reflect.ownKeys(Math).filter((key) => key !== 'f16round').map(String);
  assert.deepStrictEqual(mathKeys, [
    'abs', 'acos', 'acosh', 'asin', 'asinh', 'atan', 'atanh', 'atan2', 'ceil', 'cbrt', 'expm1', 'clz32', 'cos', 'cosh', 'exp', 'floor',
    'fround', 'hypot', 'imul', 'log', 'log1p', 'log2', 'log10', 'max', 'min', 'pow', 'random', 'round', 'sign', 'sin', 'sinh', 'sqrt',
    'tan', 'tanh', 'trunc', 'E', 'LN10', 'LN2', 'LOG10E', 'LOG2E', 'PI', 'SQRT1_2', 'SQRT2', 'Symbol(Symbol.toStringTag)',
  ]);
}

console.log('ok');
