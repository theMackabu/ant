const assert = require('node:assert');

const rep = (s, n, sep = '') => Array(n).fill(s).join(sep);
const N = 70000;
const started = Date.now();

function deep(src, expected) {
  let value;
  try { value = new Function(src)() } catch (e) {
    assert.ok(e instanceof RangeError, String(e));
    return;
  }
  assert.strictEqual(value, expected);
}

deep(rep('{', N) + 'var nested = 7;' + rep('}', N) + 'return nested', 7);
deep('return ' + rep('()=>', N) + '1', undefined);
assert.strictEqual(new Function('var a = 1; return [' + rep('a', N, ',') + '].length')(), N);

let script = '';
for (let i = 0; i < N; i++) script += `var linearScriptVar${i} = ${i};`;
assert.strictEqual((0, eval)(script + `linearScriptVar${N - 1}`), N - 1);

let captured = '';
for (let i = 0; i < 30000; i++) captured += `var v${i} = ${i};`;
captured += 'return function () { return 0';
for (let i = 0; i < 30000; i++) captured += `+v${i}`;
assert.strictEqual(new Function(captured + ' }()')(), 30000 * 29999 / 2);

assert.ok(Date.now() - started < 20000, 'long chains must compile in linear time');

console.log('ok');
