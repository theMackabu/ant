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
    template: 'return ' + nest('`${', '1', '}`'),
    'template with text': 'return ' + nest('`a${', '1', '}b`'),
    'template in key': 'return ' + nest('({[`${', '1', '}`]: 1})'),
    'regex in template': 'return ' + nest('`${/`/.source + ', '1', '}`'),
  };
  for (const [label, source] of Object.entries(shapes)) {
    worksOrRangeError(`${label} ${n}`, () => new Function(source)());
  }
}

assert.strictEqual(new Function('return ' + '`${'.repeat(500) + '7' + '}`'.repeat(500))(), '7');
assert.strictEqual(new Function('return `a${`b${`c${1}`}${`d${"`"}`}`}e`')(), 'abc1d`e');

console.log('ok');
