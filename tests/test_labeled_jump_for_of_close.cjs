// A labeled break/continue that leaves an inner for-of must run
// IteratorClose on that iterator and drop its stack slots. Previously the
// jump only popped the handler: the inner iterator stayed open, its three
// slots leaked once per jump, and `break outer` closed the wrong iterator.
const assert = require('assert');

function* gen(log, name, n) {
  try { for (let i = 0; i < n; i++) yield i; } finally { log.push(name); }
}

{
  const log = [];
  outer: for (const a of gen(log, 'outer', 3)) {
    for (const b of gen(log, 'inner', 3)) { if (b === 1) break outer; }
  }
  assert.deepStrictEqual(log, ['inner', 'outer']);
}

{
  const log = [];
  let hits = 0;
  outer: for (const a of gen(log, 'outer', 3)) {
    for (const b of gen(log, 'inner', 3)) { hits++; if (b === 1) continue outer; }
  }
  assert.strictEqual(hits, 6);
  assert.deepStrictEqual(log, ['inner', 'inner', 'inner', 'outer']);
}

{
  // Same shape, hot enough to have overflowed the frame before the fix.
  const log = [];
  let hits = 0;
  outer: for (const a of Array.from({ length: 20000 }, (_, i) => i)) {
    for (const b of gen(log, 'inner', 3)) { hits++; if (b === 1) continue outer; }
  }
  assert.strictEqual(hits, 40000);
  assert.strictEqual(log.length, 20000);
}

{
  // Through a try/finally between the loops: the inner iterator closes
  // first (the loop is inside the try), then the finally runs.
  const log = [];
  outer: for (const a of gen(log, 'outer', 2)) {
    try {
      for (const b of gen(log, 'inner', 3)) { if (b === 0) continue outer; }
    } finally { log.push('finally'); }
  }
  assert.deepStrictEqual(log, ['inner', 'finally', 'inner', 'finally', 'outer']);
}

{
  async function* agen(log, name, n) {
    try { for (let i = 0; i < n; i++) yield i; } finally { log.push(name); }
  }
  (async () => {
    const log = [];
    let hits = 0;
    outer: for await (const a of agen(log, 'outer', 2)) {
      for await (const b of agen(log, 'inner', 3)) { hits++; if (b === 1) continue outer; }
    }
    assert.strictEqual(hits, 4);
    assert.deepStrictEqual(log, ['inner', 'inner', 'outer']);
    console.log('labeled-jump-for-of-close: ok');
  })().catch(e => { console.error(e); process.exit(1); });
}
