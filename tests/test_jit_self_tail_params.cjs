const assert = require('node:assert');

// Self tail calls hand the new arguments to the hoisted parameter registers
// directly; these cover the parallel assignment, missing and extra arguments,
// parameters past the hoisting cap, and a bailout part way through the loop.
function swapTail(n, a, b) {
  if (n === 0) return `${a},${b}`;
  return swapTail(n - 1, b, a);
}
function rotateTail(n, a, b, c) {
  if (n === 0) return `${a},${b},${c}`;
  return rotateTail(n - 1, c, a, b);
}
function shrinkTail(n, x, y) {
  if (n === 0) return `${x},${y}`;
  return n === 1 ? shrinkTail(n - 1, 7) : shrinkTail(n - 1, x + 1, y + 1);
}
function extraTail(n, x) {
  if (n === 0) return `${x},${arguments.length},${arguments[2]}`;
  return extraTail(n - 1, x + 1, n);
}
function wideTail(n, p1, p2, p3, p4, p5, p6, p7, p8, p9) {
  if (n === 0) return p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;
  return wideTail(n - 1, p9, p1, p2, p3, p4, p5, p6, p7, p8 + 1);
}
function typeChangeTail(n, acc) {
  if (n === 0) return acc;
  return typeChangeTail(n - 1, n === 3 && flip ? `s${acc}` : acc + 1);
}
let flip = false;

for (let i = 0; i < 400; i++) {
  assert.strictEqual(swapTail(3, 1, 2), '2,1', `swap at ${i}`);
  assert.strictEqual(swapTail(4, 1, 2), '1,2', `swap even at ${i}`);
  assert.strictEqual(rotateTail(4, 1, 2, 3), '3,1,2', `rotate at ${i}`);
  assert.strictEqual(shrinkTail(3, 10, 20), '7,undefined', `shrink at ${i}`);
  assert.strictEqual(extraTail(3, 0), '3,3,1', `extra at ${i}`);
  assert.strictEqual(wideTail(9, 1, 2, 3, 4, 5, 6, 7, 8, 9), 54, `wide at ${i}`);
  assert.strictEqual(typeChangeTail(6, 0), 6, `numeric at ${i}`);
}
flip = true;
assert.strictEqual(typeChangeTail(6, 0), 's311', 'type change after compile');
assert.strictEqual(typeChangeTail(6, 0.5), 's3.511', 'double then string');
console.log('jit self tail params: ok');
