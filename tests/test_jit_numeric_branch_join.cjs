function same(actual, expected, name) {
  if (!Object.is(actual, expected)) throw new Error(name + ': ' + actual + ' != ' + expected);
}

function smallConstant(flag) { return flag ? 7 : 1; }
function pooledConstant(flag) { return flag ? 7 : 700; }
function fractionalConstant(flag) { return flag ? 7 : 1.5; }
function integerExpression(flag, x) { return 0.5 + (flag ? 7 : (x & 255) + 3); }
function shortCircuit(x) { return x || 1; }

// Exercise both incoming paths after compilation, including a live Number
// below the conditional result and a range-proven integer expression.
for (let i = 0; i < 1000; i++) {
  const flag = (i & 1) === 0;
  same(smallConstant(flag), flag ? 7 : 1, 'small constant join at call ' + i);
  same(pooledConstant(flag), flag ? 7 : 700, 'pooled constant join at call ' + i);
  same(fractionalConstant(flag), flag ? 7 : 1.5, 'fractional constant join at call ' + i);
  same(integerExpression(flag, i), flag ? 7.5 : (i & 255) + 3.5, 'integer expression join at call ' + i);
  same(shortCircuit(flag ? 7 : 0), flag ? 7 : 1, 'short circuit join at call ' + i);
}

console.log('JIT numeric branch join tests passed');
