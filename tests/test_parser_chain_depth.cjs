const assert = require('node:assert');

const outcome = (make) => {
  try {
    return { value: make() };
  } catch (error) {
    return { error: error.name };
  }
};

const worksOrRangeError = (label, make, check) => {
  const result = outcome(make);
  if (result.error) assert.strictEqual(result.error, 'RangeError', label);
  else if (check) assert.ok(check(result.value), label);
};

for (const n of [1000, 100000, 1000000]) {
  const nest = (open, inner, close) => open.repeat(n) + inner + close.repeat(n);
  const shapes = {
    'new chain': 'function F(){ return F } return ' + 'new '.repeat(n) + 'F',
    'unary chain': 'return ' + '!'.repeat(n) + '1',
    'arrow chain': 'return ' + '() => '.repeat(Math.min(n, 10000)) + '1',
    'optional chain': 'var a; return a' + '?.b'.repeat(n),
    'optional calls': 'var a; return a' + '?.()'.repeat(n),
    'member chain': 'var a = {}; a.a = a; return a' + '.a'.repeat(n),
    'call chain': 'var f = () => f; return f' + '()'.repeat(n),
    'class heritage': 'return ' + nest('class extends (', 'Object', ') {}'),
    'array pattern': 'var ' + nest('[', 'a', ']') + ' = ' + nest('[', '1', ']') + '; return a',
    'default values': 'return ' + nest('function(a = ', '1', '){}'),
    labels: Array.from({ length: Math.min(n, 100000) }, (_, i) => 'l' + i + ':').join('') + ';',
    comments: '/* '.repeat(n) + '*/ return 1',
    'comma chain': 'return ' + '1, '.repeat(n) + '1',
    'nullish chain': 'return ' + '1 ?? '.repeat(n) + '1',
    'in chain': 'var a = {true: 1, false: 1}; return "true"' + ' in a'.repeat(n),
  };
  for (const [label, source] of Object.entries(shapes)) {
    worksOrRangeError(`${label} ${n}`, () => new Function(source)());
  }
}

console.log('ok');
