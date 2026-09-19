const assert = require('node:assert');

function iterable(log, name, options = {}) {
  let index = 0;
  const iterator = {
    [Symbol.iterator]() { return this; },
    next() { return { value: index++, done: index > (options.count ?? 2) }; },
  };
  if (!options.missing) Object.defineProperty(iterator, 'return', {
    get() {
      if (options.getter) { log.push(name); throw options.reason; }
      return function () {
        assert.strictEqual(this, iterator);
        assert.strictEqual(arguments.length, 0);
        log.push(name);
        if (options.throws) throw options.reason;
        return options.invalid ? 1 : {};
      };
    },
  });
  return iterator;
}
function throwsExactly(fn, reason) {
  let caught = false;
  try { fn(); } catch (error) { caught = true; assert.strictEqual(error, reason); }
  assert.ok(caught, 'expected close failure');
}
function first(source, value) { for (const item of source) return value; }
function nestedPlain(outer, inner, value) {
  for (const a of outer) for (const b of inner) return value;
}
function noValue(source) { for (const item of source) return; }
function nested(log, value) {
  try {
    for (const outer of iterable(log, 'outer')) {
      try {
        for (const inner of iterable(log, 'inner')) {
          try { log.push('value'); return value; }
          finally { const local = 'local'; log.push(local); }
        }
      } finally { const another = 'between'; log.push(another); }
    }
  } finally { log.push('outside'); }
}
function inCatch(log, value) {
  for (const item of iterable(log, 'close')) {
    try { throw 'handled'; }
    catch (error) { return value; }
    finally { const local = 'finally'; log.push(local); }
  }
}
function inFinally(log, value) {
  for (const item of iterable(log, 'close')) {
    try { throw 'replaced'; }
    finally { return value; }
  }
}
function replacedReturn(log, value) {
  for (const outer of iterable(log, 'outer')) {
    try { for (const inner of iterable(log, 'inner')) return 'replaced'; }
    finally { return value; }
  }
}
function replacedByBreak(log) {
  outer: for (const a of iterable(log, 'outer')) {
    try { for (const b of iterable(log, 'inner')) return 'replaced'; }
    finally { break outer; }
  }
  return 'after';
}
function replacedByContinue(log) {
  outer: for (const a of iterable(log, 'outer')) {
    try { for (const b of iterable(log, 'inner')) return 'replaced'; }
    finally { continue outer; }
  }
  return 'after';
}
function throughOtherLoops(log, value) {
  for (const item of iterable(log, 'close')) {
    for (const key in { x: 1 }) {
      while (true) {
        switch (key) { case 'x': return value; }
      }
    }
  }
}
function closure(log) {
  for (let item of iterable(log, 'close')) {
    try { return () => item; }
    finally { item = 42; }
  }
}

// Repeat the same functions across the interpreter/JIT call threshold.
for (let round = 0; round < 160; round++) {
  const value = { round };
  let log = [];
  assert.strictEqual(first(iterable(log, 'close'), value), value);
  assert.deepStrictEqual(log, ['close']);
  log = [];
  assert.strictEqual(nestedPlain(iterable(log, 'outer'), iterable(log, 'inner'), value), value);
  assert.deepStrictEqual(log, ['inner', 'outer']);
  log = [];
  assert.strictEqual(noValue(iterable(log, 'close')), undefined);
  assert.deepStrictEqual(log, ['close']);
  log = [];
  assert.strictEqual(nested(log, value), value);
  assert.deepStrictEqual(log, ['value', 'local', 'inner', 'between', 'outer', 'outside']);
  log = [];
  assert.strictEqual(inCatch(log, value), value);
  assert.deepStrictEqual(log, ['finally', 'close']);
  log = [];
  assert.strictEqual(inFinally(log, value), value);
  assert.deepStrictEqual(log, ['close']);
  log = [];
  assert.strictEqual(replacedReturn(log, value), value);
  assert.deepStrictEqual(log, ['inner', 'outer']);
  log = [];
  assert.strictEqual(replacedByBreak(log), 'after');
  assert.deepStrictEqual(log, ['inner', 'outer']);
  log = [];
  assert.strictEqual(replacedByContinue(log), 'after');
  assert.deepStrictEqual(log, ['inner', 'inner']);
  log = [];
  assert.strictEqual(throughOtherLoops(log, value), value);
  assert.deepStrictEqual(log, ['close']);
  log = [];
  assert.strictEqual(closure(log)(), 42);
  assert.deepStrictEqual(log, ['close']);
}

for (const reason of [undefined, null, new Error('close')]) {
  for (const getter of [false, true]) {
    const log = [];
    throwsExactly(() => first(iterable(log, 'close', { getter, throws: true, reason }), 42), reason);
    assert.deepStrictEqual(log, ['close']);
  }
  const log = [];
  function innerFailure() {
    for (const outer of iterable(log, 'outer', { throws: true, reason: 'outer error' })) {
      for (const inner of iterable(log, 'inner', { throws: true, reason })) return 42;
    }
  }
  throwsExactly(innerFailure, reason);
  assert.deepStrictEqual(log, ['inner', 'outer']);
  log.length = 0;
  function finallyFailure() {
    for (const outer of iterable(log, 'outer', { throws: true, reason: 'outer error' })) {
      try { for (const inner of iterable(log, 'inner')) return 42; }
      finally { log.push('finally'); throw reason; }
    }
  }
  throwsExactly(finallyFailure, reason);
  assert.deepStrictEqual(log, ['inner', 'finally', 'outer']);
}
{
  const log = [];
  const reason = new Error('close');
  function catchOutside() {
    try {
      for (const item of iterable(log, 'close', { throws: true, reason })) {
        try { return 42; } catch { throw new Error('caught inside loop'); }
      }
    } catch (error) { assert.strictEqual(error, reason); return 'caught outside'; }
  }
  assert.strictEqual(catchOutside(), 'caught outside');
  assert.deepStrictEqual(log, ['close']);
  assert.throws(() => first(iterable([], 'close', { invalid: true }), 42), TypeError);
  assert.strictEqual(first(iterable([], 'close', { missing: true }), 42), 42);
  function* generator() { for (const item of iterable(log, 'generator close')) return 7; }
  assert.deepStrictEqual(generator().next(), { value: 7, done: true });
  assert.deepStrictEqual(log, ['close', 'generator close']);
}
{
  const exhausted = {
    [Symbol.iterator]() { return this; }, next() { return { done: true }; },
    get return() { throw new Error('must not close an exhausted iterator'); },
  };
  assert.strictEqual(first(exhausted, 42), undefined);
  const log = [];
  const reason = new Error('return expression');
  throwsExactly(() => {
    for (const item of iterable(log, 'close', { throws: true, reason: 'cleanup' }))
      return (() => { log.push('expression'); throw reason; })();
  }, reason);
  assert.deepStrictEqual(log, ['expression', 'close']);
  log.length = 0;
  const resource = { [Symbol.dispose]() { log.push('dispose'); } };
  const source = iterable(log, 'close');
  source.next = () => ({ value: resource, done: false });
  function usingReturn() { for (using value of source) return value; }
  assert.strictEqual(usingReturn(), resource);
  assert.deepStrictEqual(log, ['dispose', 'close']);
}
console.log('return-for-of-close: ok');
