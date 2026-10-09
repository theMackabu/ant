const assert = require('node:assert');

// A small function first compiled through OSR before its callers warmed up
// calls the concat helper outside its loop; once calls pick up it recompiles
// with the inline fast path. Both versions must build the same strings; the
// function is too big to inline, so its callers run its own code.
const parts = { a: 'a', b: 'bb', c: 'ccc' };
function build(items) {
  let s = '<';
  s += parts.a; s += parts.b; s += parts.c; s += parts.a; s += parts.b; s += parts.c;
  s += parts.a; s += parts.b; s += parts.c; s += parts.a; s += parts.b; s += parts.c;
  for (let i = 0; i < items.length; i++) s += items[i];
  s += `>${items.length}`;
  return s;
}
const head = '<' + 'abbccc'.repeat(4);

const big = Array.from({ length: 2000 }, (_, i) => String(i % 10));
for (let i = 0; i < 5; i++) assert.strictEqual(build(['x']), `${head}x>1`);
assert.strictEqual(build(big), `${head}${big.join('')}>2000`);
for (let i = 0; i < 1000; i++) {
  const items = i % 3 ? ['y', String(i)] : [];
  assert.strictEqual(build(items), `${head}${items.join('')}>${items.length}`, `call ${i}`);
}
console.log('jit concat retier: ok');
