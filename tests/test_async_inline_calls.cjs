const assert = require('node:assert');

const log = [];

async function throwsSync() { throw new Error('sync'); }
async function throwsAfterAwait() { await null; throw new Error('late'); }
async function catches() { try { await throwsSync(); } catch (e) { return 'caught ' + e.message; } }
async function catchesSync() { try { throw new Error('inner'); } catch (e) { return 'local ' + e.message; } }
async function nested(n) { return n === 0 ? 'bottom' : 'n' + n + ':' + await nested(n - 1); }
async function noAwaitNested(n) { return n === 0 ? 0 : 1 + await noAwaitNested(n - 1); }
function syncThrows() { throw new RangeError('deep'); }
async function callsSyncThrower() { return syncThrows(); }
async function tailCall() { return helper(); }
function helper() { return 'tail'; }
async function argsLen() { return arguments.length; }
async function sloppyThis() { return this === globalThis; }
async function strictThis() { 'use strict'; return this; }
const obj = {
  v: 7,
  async m() { return this.v; },
  arrow() { return (async () => this.v)(); },
};
async function order(tag) { log.push(tag + ':start'); await null; log.push(tag + ':end'); return tag; }
async function syncValue() { log.push('sync:body'); return 1; }
async function finallyReturn() { try { return 'f'; } finally { log.push('fin'); } }
const closer = {
  [Symbol.asyncIterator]() { return this; },
  next() { return Promise.resolve({ value: 1, done: false }); },
  async return() { await null; return {}; },
};
async function forAwaitReturn(value) { for await (const item of closer) return value; }
function syncCallsForAwait(value) { return forAwaitReturn(value); }
async function closureCapture() {
  let x = 1;
  const get = () => x;
  await null;
  x = 2;
  return get();
}

(async () => {
  for (let round = 0; round < 3; round++) {
    log.length = 0;

    const p = throwsSync();
    assert.ok(p instanceof Promise);
    await assert.rejects(p, { message: 'sync' });
    await assert.rejects(throwsAfterAwait(), { message: 'late' });
    assert.strictEqual(await catches(), 'caught sync');
    assert.strictEqual(await catchesSync(), 'local inner');
    assert.strictEqual(await nested(3), 'n3:n2:n1:bottom');
    assert.strictEqual(await noAwaitNested(50), 50);
    await assert.rejects(callsSyncThrower(), RangeError);
    assert.strictEqual(await tailCall(), 'tail');
    assert.strictEqual(await argsLen(1, 2, 3), 3);
    assert.strictEqual(await sloppyThis(), true);
    assert.strictEqual(await strictThis(), undefined);
    assert.strictEqual(await obj.m(), 7);
    assert.strictEqual(await obj.arrow(), 7);
    assert.strictEqual(await closureCapture(), 2);

    const fin = finallyReturn();
    assert.ok(fin instanceof Promise);
    assert.strictEqual(await fin, 'f');

    const fa = syncCallsForAwait('early');
    assert.ok(fa instanceof Promise);
    assert.strictEqual(await fa, 'early');

    log.length = 0;
    const a = order('a');
    log.push('after-a');
    const b = order('b');
    syncValue().then(() => log.push('sync:then'));
    log.push('after-b');
    await Promise.all([a, b]);
    assert.deepStrictEqual(log, [
      'a:start', 'after-a', 'b:start', 'sync:body', 'after-b', 'a:end', 'b:end', 'sync:then',
    ]);

    async function deep(n) { return n === 0 ? 0 : 1 + await deep(n - 1); }
    assert.strictEqual(await deep(2000), 2000);
  }

  let overflow = 0;
  async function runaway() { overflow++; return runaway(); }
  await assert.rejects(runaway(), RangeError);
  assert.ok(overflow > 100);

  console.log('PASS async inline calls');
})();
