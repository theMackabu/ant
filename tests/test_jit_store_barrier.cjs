// Compiled named stores of a reference into an old object run the write
// barrier inline (mir_emit_put_field_value_guard): a young value remembers
// the object, so the next minor collection keeps the value. Missing it frees
// a live object. Covers existing properties, adds, functions as values, and
// objects already remembered.
const assert = require('node:assert');

function churn(n) {
  let keep = null;
  for (let i = 0; i < n; i++) keep = { i, pad: [i, i] };
  return keep;
}

// old objects of one shape, so each store site's inline case matches
// (survive several collections first)
const olds = [];
for (let i = 0; i < 64; i++) olds.push({ left: null, right: null });
churn(300000);

function setLeft(o, v) { o.left = v; }
function setRight(o, v) { o.right = v; }
function addExtra(o, v) { o.extra = v; }

for (let round = 0; round < 40; round++) {
  for (let i = 0; i < olds.length; i++) {
    const o = olds[i];
    setLeft(o, { tag: round * 1000 + i, inner: { v: i } });
    setRight(o, function f() { return round * 1000 + i; });
    if (round === 5) addExtra(o, [round, i]);
  }
  churn(20000);
  for (let i = 0; i < olds.length; i++) {
    const o = olds[i];
    assert.strictEqual(o.left.tag, round * 1000 + i);
    assert.strictEqual(o.left.inner.v, i);
    assert.strictEqual(o.right(), round * 1000 + i);
    if (round >= 5) assert.strictEqual(o.extra[0] + o.extra[1], 5 + i);
  }
}

console.log('PASS compiled stores into old objects keep young values');
