const assert = require('node:assert');
function collect(piece, count) {
  let value = '';
  const snapshots = [];
  for (let i = 0; i < count; i++) {
    snapshots.push(value);
    value += piece;
  }
  for (let i = 0; i < count; i++) assert.strictEqual(snapshots[i], piece.repeat(i));
  assert.strictEqual(value, piece.repeat(count));
  return value;
}
for (let i = 0; i < 30000; i++) collect(i & 1 ? 'abc' : 'é😀', 3);
for (const piece of ['x', 'é😀', '\ud800', '\0z']) collect(piece, 400);
for (const length of [12, 13, 15, 16, 30, 31, 32, 33, 255, 256, 257, 1024]) collect('x'.repeat(length), 4);
function reentrant() {
  let value = 'left';
  function rhs() { value = 'replaced'; return 'right'; }
  value += rhs();
  assert.strictEqual(value, 'leftright');
  value += { [Symbol.toPrimitive]() { value = 'changed'; return '!'; } };
  assert.strictEqual(value, 'leftright!');
  assert.throws(() => { value += Symbol('no'); }, TypeError);
  assert.strictEqual(value, 'leftright!');
}
for (let i = 0; i < 30000; i++) reentrant();
console.log('short append snapshots, growth, Unicode and reentrancy ok');
