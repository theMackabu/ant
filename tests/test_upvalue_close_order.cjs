const assert = require('node:assert');

// Keep many outer VM captures alive while short interpreted calls return and
// close block captures. Mutating the outer cells must remain visible.
const count = 128;
const names = Array.from({ length: count }, (_, i) => `cell${i}`);
const make = Function(`
  let ${names.map((name, i) => `${name}=${i}`).join(',')};
  const readOuter = () => [${names.join(',')}];
  function visit(value, fail) {
    let result;
    for (let i = 0; i < 2; i++) {
      let local = +value + i;
      result = () => local;
      if (fail) throw result;
    }
    return result;
  }
  let last;
  for (let i = 0; i < 400; i++) {
    +i;
    last = visit(i, false);
    try { visit(i, true); } catch (read) {
      if (read() !== i) throw new Error('throw closed the wrong capture');
    }
    cell0 = i;
    if (readOuter()[0] !== i || last() !== i + 1) throw new Error('capture changed');
  }
  return [readOuter, last];
`);
const [readOuter, last] = make();
assert.deepStrictEqual(readOuter(), [399, ...Array.from({ length: count - 1 }, (_, i) => i + 1)]);
assert.strictEqual(last(), 400);

// Resume a suspended activation while its caller has live captures, then close
// its inner block and finally return its own escaped closure.
function* suspended(seed) {
  let state = seed;
  const read = () => state;
  yield read;
  for (let i = 0; i < 3; i++) {
    let block = state + i;
    yield () => block;
  }
  state += 10;
  return read;
}
function caller(seed) {
  let outer = seed;
  const read = () => outer;
  const iter = suspended(seed);
  const inner = iter.next().value;
  const blocks = [iter.next().value, iter.next().value, iter.next().value];
  const final = iter.next();
  outer += 100;
  assert.strictEqual(read(), seed + 100);
  assert.strictEqual(inner(), seed + 10);
  assert.strictEqual(final.value(), seed + 10);
  assert.strictEqual(final.done, true);
  assert.deepStrictEqual(blocks.map(fn => fn()), [seed, seed + 1, seed + 2]);
}
for (let i = 0; i < 300; i++) caller(i);
console.log('upvalue close order: ok');
