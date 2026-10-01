// Compiled object literals store known slots inline (jit_emit_define_slot_inline)
// while the object is young and the key is ordinary. Values computed between
// creating the literal and filling it may run collections, so the object can
// be old by the time a slot is stored: that store must keep its young value
// alive. Keys the runtime watches (then, constructor, prototype) still take
// the helper.
const assert = require('node:assert');

let sink = null;
function churn(n) {
  for (let i = 0; i < n; i++) sink = { i, pad: [i, i + 1, i + 2] };
  return n;
}

function wide(i) {
  return { a: i, b: i + 1, c: i + 2, d: i + 3, e: { v: i }, f: [i], g: 'g' + i };
}

function afterGc(i) {
  // the literal exists before churn() runs, so it may be promoted before
  // its later slots are stored
  return { first: { v: i }, mid: churn(i % 7 === 0 ? 40000 : 10), last: { v: i + 1 }, tail: [i] };
}

function special(i) {
  return { then: i, constructor: i + 1, prototype: i + 2, x: i + 3 };
}

const kept = [];
for (let i = 0; i < 20000; i++) {
  const w = wide(i);
  assert.strictEqual(w.a + w.d, 2 * i + 3);
  assert.strictEqual(w.e.v, i);
  assert.strictEqual(w.f[0], i);
  assert.strictEqual(w.g, 'g' + i);

  const s = special(i);
  assert.strictEqual(s.then + s.constructor + s.prototype + s.x, 4 * i + 6);

  if (i % 50 === 0) kept.push(afterGc(i));
}

churn(200000);
kept.forEach((o, n) => {
  const i = n * 50;
  assert.strictEqual(o.first.v, i);
  assert.strictEqual(o.last.v, i + 1);
  assert.strictEqual(o.tail[0], i);
});

console.log('PASS compiled object literals store their slots');
