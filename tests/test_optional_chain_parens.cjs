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

deep('var o = {}; o.a = o; return o' + rep('.a', N) + ' === o', true);
deep('var o = [0]; o[0] = o; return o' + rep('[0]', N) + ' === o', true);
deep('var o = {}; o.a = o; return o' + rep('?.a', N) + ' === o', true);

assert.throws(() => new Function('var a = null; return (a?.b).c')(), TypeError);
assert.throws(() => new Function('var a = null; return (a?.b)()')(), TypeError);
assert.throws(() => new Function('var a = null; return delete (a?.b).c')(), TypeError);
assert.strictEqual(new Function('var a = null; return a?.b.c.d')(), undefined);

assert.ok(Date.now() - started < 20000, 'long chains must compile in linear time');

console.log('ok');
