const assert = require('node:assert');

function literal(n, hole) {
  const items = [];
  for (let i = 0; i < n; i++) items.push(i === hole ? '' : String(i & 255));
  return items.join(',');
}

for (const n of [33, 65535, 65536, 65537, 102433]) {
  const arr = new Function(`return [${literal(n, 20)}];`)();
  assert.strictEqual(arr.length, n);
  assert.strictEqual(arr[0], 0);
  assert.strictEqual(arr[n - 1], (n - 1) & 255);
  assert.ok(!(20 in arr));

  const u8 = new Function(`return new Uint8Array([${literal(n, -1)}]);`)();
  assert.ok(u8 instanceof Uint8Array);
  assert.strictEqual(u8.length, n);
  assert.strictEqual(u8[n - 1], (n - 1) & 255);

  const call = new Function('f', `return f(1, [${literal(n, -1)}], 2);`)((a, b, c) => [a, b.length, c]);
  assert.deepStrictEqual(call, [1, n, 2]);
}

const mixedSource = `[${Array.from({ length: 40 }, (_, i) => ['-1.5', '"s"', 'true', 'false', 'null', '', '-0', '1e300'][i % 8]).join(',')}]`;
const mixedExpected = eval(mixedSource);
const mixed = new Function(`return ${mixedSource};`);
const first = mixed();
assert.deepStrictEqual(first, mixedExpected);
assert.ok(!(5 in first));
assert.ok(Object.is(first[6], -0));
first[0] = 'changed';
first.push(1);
assert.deepStrictEqual(mixed(), mixedExpected);
assert.notStrictEqual(mixed(), mixed());

const big = new Function(`return [${literal(100, -1)}];`);
const withSpread = new Function('x', `return [${literal(40, -1)}, ...x];`);
const withExpr = new Function('x', `return [${literal(40, -1)}, x];`);
for (let i = 0; i < 20000; i++) {
  const a = big();
  if (a.length !== 100 || a[99] !== 99) throw new Error(`big literal broke at iteration ${i}`);
  a[99] = -1;
  if (withSpread([i]).at(-1) !== i) throw new Error(`spread literal broke at iteration ${i}`);
  if (withExpr(i)[40] !== i) throw new Error(`expression literal broke at iteration ${i}`);
}

console.log('ok');
