const assert = require('node:assert');

const ROUNDS = 3000;

function hot(fn, check) {
  for (let i = 0; i < ROUNDS; i++) check(fn(i), i);
}

function normal(i) {
  let log = 0;
  try { log += i; }
  finally { log += 1; }
  return log;
}
hot(normal, (r, i) => assert.strictEqual(r, i + 1));

function thrower(i) {
  if (i % 7 === 3) throw new RangeError('boom ' + i);
  return i;
}

function rethrow(i) {
  let seen = 0;
  try {
    try { return thrower(i); }
    finally { seen = 1; }
  } catch (e) {
    return e.message + ':' + seen;
  }
}
hot(rethrow, (r, i) => assert.strictEqual(r, i % 7 === 3 ? 'boom ' + i + ':1' : i));

function throwStatement(i) {
  const log = [];
  try {
    try { if (i & 1) throw i; log.push('body'); }
    finally { log.push('fin'); }
  } catch (e) { log.push('caught' + e); }
  return log.join();
}
hot(throwStatement, (r, i) => assert.strictEqual(r, i & 1 ? 'fin,caught' + i : 'body,fin'));

function catchFinally(i) {
  const log = [];
  try { thrower(i); log.push('ok'); }
  catch (e) { log.push('catch'); }
  finally { log.push('fin'); }
  return log.join();
}
hot(catchFinally, (r, i) => assert.strictEqual(r, i % 7 === 3 ? 'catch,fin' : 'ok,fin'));

function returnThrough(i) {
  const log = [];
  const value = (() => {
    try {
      try { return i * 2; }
      finally { log.push('inner'); }
    } finally { log.push('outer'); }
  })();
  return log.join() + '=' + value;
}
hot(returnThrough, (r, i) => assert.strictEqual(r, 'inner,outer=' + i * 2));

function nestedReturn(i) {
  let n = 0;
  try {
    try { return i; }
    finally { n++; }
  } finally {
    n++;
    if (n !== 2) return -1;
  }
}
hot(nestedReturn, (r, i) => assert.strictEqual(r, i));

function overrideReturn(i) {
  try { return i; }
  finally { if (i % 3 === 0) return 'over'; }
}
hot(overrideReturn, (r, i) => assert.strictEqual(r, i % 3 === 0 ? 'over' : i));

function finallyThrowReplaces(i) {
  try {
    try { return i; }
    finally { if (i % 5 === 0) throw new Error('fin' + i); }
  } catch (e) { return e.message; }
}
hot(finallyThrowReplaces, (r, i) => assert.strictEqual(r, i % 5 === 0 ? 'fin' + i : i));

function swallowThrow(i) {
  for (;;) {
    try { throw new Error('x' + i); }
    finally { break; }
  }
  return 'swallowed' + i;
}
hot(swallowThrow, (r, i) => assert.strictEqual(r, 'swallowed' + i));

function breakContinue(i) {
  const log = [];
  for (let j = 0; j < 4; j++) {
    try {
      if (j === 1) continue;
      if (j === 3) break;
      log.push(j);
    } finally {
      log.push('f' + j);
    }
  }
  return log.join() + ':' + i;
}
hot(breakContinue, (r, i) => assert.strictEqual(r, '0,f0,f1,2,f2,f3:' + i));

function labeledBreak(i) {
  const log = [];
  outer: for (let a = 0; a < 3; a++) {
    try {
      for (let b = 0; b < 3; b++) {
        try {
          if (b === 1 && a === (i % 3)) break outer;
          if (b === 1) continue outer;
        } finally { log.push(a + '' + b); }
      }
    } finally { log.push('o' + a); }
  }
  return log.join();
}
const labeledExpect = k => {
  const log = [];
  for (let a = 0; a <= k; a++) log.push(a + '0', a + '1', 'o' + a);
  return log.join();
};
hot(labeledBreak, (r, i) => assert.strictEqual(r, labeledExpect(i % 3)));

function breakOutOfFinally(i) {
  let n = 0;
  for (let j = 0; j < 3; j++) {
    try { n += j; }
    finally {
      if (j === 1) break;
      n += 10;
    }
  }
  return n + i;
}
hot(breakOutOfFinally, (r, i) => assert.strictEqual(r, 11 + i));

function forOfReturn(i) {
  const log = [];
  const iter = {
    [Symbol.iterator]() {
      let k = 0;
      return {
        next: () => ({ value: k++, done: k > 5 }),
        return: () => { log.push('close'); return { done: true }; }
      };
    }
  };
  const r = (() => {
    for (const v of iter) {
      try { if (v === 2) return v + i; }
      finally { log.push('fin' + v); }
    }
  })();
  return log.join() + '=' + r;
}
hot(forOfReturn, (r, i) => assert.strictEqual(r, 'fin0,fin1,fin2,close=' + (2 + i)));

function captured(i) {
  let x = i;
  const get = () => x;
  try {
    x += 1;
    return get;
  } finally {
    x += 1;
  }
}
hot(captured, (r, i) => assert.strictEqual(r(), i + 2));

class Base { constructor(v) { this.v = v; } }
class Derived extends Base {
  constructor(v) {
    try { super(v); return; }
    finally { this.w = v + 1; }
  }
}
hot(i => new Derived(i), (r, i) => assert.deepStrictEqual([r.v, r.w], [i, i + 1]));

function deoptInFinally(i, value) {
  let acc = 0;
  for (let j = 0; j < 3; j++) {
    try {
      if (j === 1) return 'ret' + i;
      if (j === 2) throw new Error('never');
    } finally {
      acc = acc + value;
    }
  }
  return acc;
}
for (let i = 0; i < ROUNDS; i++) assert.strictEqual(deoptInFinally(i, 1), 'ret' + i);
assert.strictEqual(deoptInFinally(1, 'a'), 'ret1');
assert.strictEqual(deoptInFinally(2, {}), 'ret2');

function deoptThrowInFinally(i, value) {
  try {
    try { throw new Error('t' + i); }
    finally { value = value * 2; }
  } catch (e) { return e.message + ':' + value; }
}
for (let i = 0; i < ROUNDS; i++) assert.strictEqual(deoptThrowInFinally(i, 3), 't' + i + ':6');
assert.strictEqual(deoptThrowInFinally(9, '4'), 't9:8');
assert.strictEqual(deoptThrowInFinally(9, { valueOf: () => 5 }), 't9:10');

function deoptBreakInFinally(value) {
  const log = [];
  for (let j = 0; j < 3; j++) {
    try {
      if (j === 1) break;
      log.push(j);
    } finally {
      log.push(value + j);
    }
  }
  log.push('end');
  return log.join();
}
for (let i = 0; i < ROUNDS; i++) assert.strictEqual(deoptBreakInFinally(1), '0,1,2,end');
assert.strictEqual(deoptBreakInFinally('s'), '0,s0,s1,end');

function deoptInTry(i, value) {
  const log = [];
  try {
    log.push(value + 1);
    if (i % 4 === 0) throw new Error('e');
    return log.join();
  } finally {
    log.push('f');
  }
}
for (let i = 1; i < ROUNDS; i += 4) assert.strictEqual(deoptInTry(i, 1), '2');
assert.strictEqual(deoptInTry(1, 'x'), 'x1');
assert.throws(() => deoptInTry(0, 'y'), /^Error: e$/);

function hotLoopInTry(n) {
  let sum = 0;
  try {
    for (let k = 0; k < n; k++) sum += k;
    return sum;
  } finally {
    sum = -1;
  }
}
assert.strictEqual(hotLoopInTry(100000), 4999950000);

function hotLoopInFinally(n) {
  let sum = 0;
  try {
    return 'kept';
  } finally {
    for (let k = 0; k < n; k++) sum += k;
    if (sum !== n * (n - 1) / 2) throw new Error('bad sum');
  }
}
assert.strictEqual(hotLoopInFinally(100000), 'kept');

function hotLoopInFinallyThrow(n) {
  try {
    try { throw new Error('pending'); }
    finally { let s = 0; for (let k = 0; k < n; k++) s += k; }
  } catch (e) { return e.message; }
}
assert.strictEqual(hotLoopInFinallyThrow(100000), 'pending');

async function asyncFinally(i) {
  const log = [];
  try {
    await null;
    if (i % 2) throw new Error('a' + i);
    return 'r' + i;
  } catch (e) {
    return e.message;
  } finally {
    log.push(i);
  }
}

async function asyncAwaitInFinally(i) {
  let n = 0;
  try { return i; }
  finally { await null; n++; }
}

(async () => {
  for (let i = 0; i < 400; i++) {
    assert.strictEqual(await asyncFinally(i), i % 2 ? 'a' + i : 'r' + i);
    assert.strictEqual(await asyncAwaitInFinally(i), i);
  }
  console.log('jit try/finally: ok');
})();
