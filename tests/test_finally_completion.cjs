const assert = require('node:assert');

function caughtReturn(source, log) {
  for (const x of source) {
    try { return 42; }
    finally { try { throw 0; } catch {} log.push('finally'); }
  }
  return 99;
}

function nestedReturn(log) {
  try { return { answer: 42 }; }
  finally {
    try {} finally { log.push('inner'); }
    try { throw 0; } catch {}
    log.push('after');
  }
}

function caughtThrow(reason, log) {
  try { throw reason; }
  finally {
    try { try { throw 0; } finally { log.push('inner'); } } catch {}
    log.push('after');
  }
}

function replacedReturn() {
  try { return 42; }
  finally { try { return 7; } finally { try { throw 0; } catch {} } }
}

function localBreak(log) {
  try { return 42; }
  finally {
    for (let i = 0; i < 2; i++) {
      try { break; } finally { try { throw 0; } catch {} }
    }
    log.push('after');
  }
}

function pendingBreak(log) {
  outer: for (let i = 0; i < 2; i++) {
    try { break outer; }
    finally {
      try { throw 0; } catch {}
      try {} finally { log.push('inner'); }
      log.push('after');
    }
  }
  log.push('done');
}

function pendingContinue(log) {
  for (let i = 0; i < 2; i++) {
    try { continue; }
    finally { try { throw 0; } catch {} log.push(i); }
    log.push('unreachable');
  }
}

for (let round = 0; round < 160; round++) {
  let log = [];
  assert.strictEqual(caughtReturn([1], log), 42);
  assert.deepStrictEqual(log, ['finally']);
  log = [];
  let nexts = 0;
  const source = {
    [Symbol.iterator]() { return this; },
    next() {
      assert.strictEqual(++nexts, 1, 'must not advance an unbounded iterator after return');
      return { value: 1, done: false };
    },
    return() { log.push('close'); return {}; },
  };
  assert.strictEqual(caughtReturn(source, log), 42);
  assert.deepStrictEqual(log, ['finally', 'close']);
  log = [];
  assert.deepStrictEqual(nestedReturn(log), { answer: 42 });
  assert.deepStrictEqual(log, ['inner', 'after']);
  for (const reason of [undefined, null, { round }]) {
    log = [];
    assert.throws(() => caughtThrow(reason, log), value => value === reason);
    assert.deepStrictEqual(log, ['inner', 'after']);
  }
  assert.strictEqual(replacedReturn(), 7);
  log = [];
  assert.strictEqual(localBreak(log), 42);
  assert.deepStrictEqual(log, ['after']);
  log = [];
  pendingBreak(log);
  assert.deepStrictEqual(log, ['inner', 'after', 'done']);
  log = [];
  pendingContinue(log);
  assert.deepStrictEqual(log, [0, 1]);
  assert.throws(() => {
    try { return 42; } finally { throw 'replacement'; }
  }, value => value === 'replacement');
}

function* suspendedReturn(log) {
  try { return { answer: 42 }; }
  finally {
    yield 'outer';
    try {} finally { yield 'inner'; }
    try { throw 0; } catch {}
    log.push('after');
  }
}

async function asyncReturn(log) {
  try { return { answer: 42 }; }
  finally {
    await Promise.resolve();
    try { await Promise.reject(0); } catch {}
    try {} finally { await Promise.resolve(); }
    log.push('after');
  }
}

async function main() {
  const log = [];
  const iterator = suspendedReturn(log);
  assert.deepStrictEqual(iterator.next(), { value: 'outer', done: false });
  assert.deepStrictEqual(iterator.next(), { value: 'inner', done: false });
  assert.deepStrictEqual(iterator.next(), { value: { answer: 42 }, done: true });
  assert.deepStrictEqual(log, ['after']);
  log.length = 0;
  assert.deepStrictEqual(await asyncReturn(log), { answer: 42 });
  assert.deepStrictEqual(log, ['after']);
  const replacement = suspendedReturn([]);
  replacement.next();
  assert.deepStrictEqual(replacement.return(9), { value: 9, done: true });
  console.log('nested finally preserves pending completions');
}

main().catch(error => { console.error(error); process.exitCode = 1; });
