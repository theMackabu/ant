const assert = require('node:assert');

const rep = (s, n, sep = '') => Array(n).fill(s).join(sep);
const N = 70000;
const started = Date.now();

assert.strictEqual(new Function('var x = 1; return ' + rep('x', N, '+'))(), N);
assert.strictEqual(new Function('return ' + rep('1', N, '+'))(), N);
assert.strictEqual(new Function('let y = 2; return ' + rep('y', N, '*') + ' === Infinity')(), true);
assert.strictEqual(new Function('var x = 3; return ' + rep('x', N, '-'))(), 3 - 3 * (N - 1));
assert.strictEqual(new Function("return (" + rep("'a'", N, '+') + ').length')(), N);
assert.strictEqual(new Function('let s = "b"; return (' + rep('s', N, '+') + ').length')(), N);
assert.strictEqual(new Function('var x = 1; return ' + rep('x + 2 * x - x / 2', 2000, ' + '))(), 5000);
assert.strictEqual(new Function('var x = 1, s = "s"; return ' + rep('x + s + x', 500, ' + '))(), '1s1' + rep('1s1', 499));

const list = (n, f, sep = ',') => {
  const parts = new Array(n);
  for (let i = 0; i < n; i++) parts[i] = f(i);
  return parts.join(sep);
};

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

for (const op of ['%', '&', '|', '^', '<<', '>>', '>>>', '&&', '||', '??', '<', '==', '-']) {
  const n = 150000;
  const source = 'var a = 3; return ' + list(n, (i) => (i % 7 === 3 ? 'a' : String(1 + i % 3)), ' ' + op + ' ');
  const reference = () => {
    let acc = 1;
    for (let i = 1; i < n; i++) {
      const v = i % 7 === 3 ? 3 : 1 + i % 3;
      switch (op) {
        case '%': acc = acc % v; break;
        case '&': acc = acc & v; break;
        case '|': acc = acc | v; break;
        case '^': acc = acc ^ v; break;
        case '<<': acc = acc << v; break;
        case '>>': acc = acc >> v; break;
        case '>>>': acc = acc >>> v; break;
        case '&&': acc = acc && v; break;
        case '||': acc = acc || v; break;
        case '??': acc = acc ?? v; break;
        case '<': acc = acc < v; break;
        case '==': acc = acc == v; break;
        default: acc = acc - v;
      }
    }
    return acc;
  };
  if (op === '<' || op === '==') worksOrRangeError(`flat ${op} chain`, () => new Function(source)(), (value) => value === reference());
  else assert.strictEqual(new Function(source)(), reference(), `flat ${op} chain`);
}

assert.ok(Date.now() - started < 20000, 'long chains must compile in linear time');

console.log('ok');
