const assert = require('node:assert');
function iterable(log, name, options = {}) {
  return {
    [Symbol.asyncIterator]() { return this; },
    next() { return Promise.resolve({ value: 1, done: false }); },
    async return() {
      assert.strictEqual(arguments.length, 0);
      log.push(`${name} start`);
      await Promise.resolve();
      log.push(`${name} end`);
      if (options.throws) throw options.reason;
      return options.invalid ? 1 : {};
    },
  };
}
async function first(source, value) { for await (const item of source) return value; }
async function nested(log, value) {
  try {
    for await (const a of iterable(log, 'outer')) {
      try {
        for await (const b of iterable(log, 'inner')) {
          try { return value; }
          finally { const local = 'inside'; log.push(local); }
        }
      } finally { const local = 'between'; log.push(local); }
    }
  } finally { log.push('outside'); }
}
async function replaced(log, value) {
  for await (const a of iterable(log, 'outer')) {
    try { for await (const b of iterable(log, 'inner')) return 'replaced'; }
    finally { return value; }
  }
}
async function main() {
  for (let round = 0; round < 120; round++) {
    const value = { round };
    let log = [];
    assert.strictEqual(await first(iterable(log, 'close'), value), value);
    assert.deepStrictEqual(log, ['close start', 'close end']);
    log = [];
    assert.strictEqual(await nested(log, value), value);
    assert.deepStrictEqual(log, ['inside', 'inner start', 'inner end', 'between', 'outer start', 'outer end', 'outside']);
    log = [];
    assert.strictEqual(await replaced(log, value), value);
    assert.deepStrictEqual(log, ['inner start', 'inner end', 'outer start', 'outer end']);
  }
  for (const reason of [undefined, null, new Error('async close')]) {
    const log = [];
    let caught = false;
    try { await first(iterable(log, 'close', { throws: true, reason }), 42); }
    catch (error) { caught = true; assert.strictEqual(error, reason); }
    assert.ok(caught, 'missing async close rejection');
    assert.deepStrictEqual(log, ['close start', 'close end']);
  }
  for (const reason of [undefined, null, new Error('inner close')]) {
    const log = [];
    let caught = false;
    async function failingNested() {
      for await (const a of iterable(log, 'outer', { throws: true, reason: 'outer error' }))
        for await (const b of iterable(log, 'inner', { throws: true, reason })) return 42;
    }
    try { await failingNested(); }
    catch (error) { caught = true; assert.strictEqual(error, reason); }
    assert.ok(caught);
    assert.deepStrictEqual(log, ['inner start', 'inner end', 'outer start', 'outer end']);
  }
  const exhausted = {
    [Symbol.asyncIterator]() { return this; }, async next() { return { done: true }; },
    get return() { throw new Error('must not close an exhausted iterator'); },
  };
  assert.strictEqual(await first(exhausted, 42), undefined);
  let caught = false;
  try { await first(iterable([], 'close', { invalid: true }), 42); }
  catch (error) { caught = true; assert.ok(error instanceof TypeError); }
  assert.ok(caught, 'invalid async close result accepted');
  const log = [];
  function* sync() { try { yield 1; } finally { log.push('sync close'); } }
  assert.strictEqual(await first(sync(), 42), 42);
  assert.deepStrictEqual(log, ['sync close']);
  console.log('return-for-await-close: ok');
}
const deadline = setTimeout(() => { console.error('iterator return remained pending'); process.exit(1); }, 5000);
main().then(() => clearTimeout(deadline), error => { console.error(error); process.exit(1); });
