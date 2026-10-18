const assert = require('node:assert');

async function noAwait(n) { let s = 0; for (let k = 0; k < n; k++) s += k; return s; }
async function afterAwait(n) { await null; let s = 0; for (let k = 0; k < n; k++) s += k; return s; }
async function aroundLoop(n) { let s = 0; for (let k = 0; k < n; k++) s += k; s += await n; for (let k = 0; k < n; k++) s -= k; return s; }
async function loopAwait(n) { let s = 0; for (let k = 0; k < n; k++) s += await k; return s; }
async function inc(x) { return x + 1; }
async function mark(log, k) { log.push('in' + k); await null; log.push('out' + k); return k; }
async function throwsEarly(x) { if (x > 0) throw new Error('early' + x); return x; }
async function throwsLate(x) { await null; throw new Error('late' + x); }
async function rec(n) { if (n === 0) return 0; return rec(n - 1); }
async function recAwait(n) { if (n === 0) return 0; return 1 + await recAwait(n - 1); }
async function retAwaitFinally(log, x) { try { return await x; } finally { log.push('fin'); } }
async function markLoop(log, k) { let s = 0; for (let j = 0; j < 40; j++) s += j; log.push('in' + k); await null; log.push('out' + k); return s + k; }
async function recLoop(n) { let s = 0; for (let j = 0; j < 40; j++) s += j; if (n === 0) return s; return recLoop(n - 1); }
const obj = { v: 10, async m(a, b) { await null; return this.v + a + b + arguments.length; } };

function drive(n) { const ps = []; for (let k = 0; k < n; k++) ps.push(inc(k)); return ps; }
function driveMark(n, log) { const ps = []; for (let k = 0; k < n; k++) ps.push(mark(log, k)); return ps; }
function driveLoop(n, log) { const ps = []; for (let k = 0; k < n; k++) ps.push(markLoop(log, k)); return ps; }
function driveThrow(n) { let sync = 0; const ps = []; for (let k = 0; k < n; k++) { try { ps.push(throwsEarly(k & 1)); } catch { sync++; } } return [sync, ps]; }

async function ticks(fn) {
  const log = [];
  fn().then((v) => log.push('done:' + v));
  for (let t = 0; t < 5; t++) { log.push('t' + t); await null; }
  return log.join(' ');
}

(async () => {
  for (let r = 0; r < 6; r++) {
    assert.strictEqual(await noAwait(30000), 449985000);
    assert.strictEqual(await afterAwait(30000), 449985000);
    assert.strictEqual(await aroundLoop(30000), 30000);
    assert.strictEqual(await loopAwait(2000), 1999000);
  }

  for (let r = 0; r < 4; r++) {
    const ps = drive(5000);
    assert.ok(ps.every((p) => p instanceof Promise));
    const vs = await Promise.all(ps);
    assert.strictEqual(vs.reduce((a, b) => a + b, 0), 12502500);
  }

  for (let r = 0; r < 4; r++) {
    const log = [];
    const ps = driveMark(3000, log);
    assert.strictEqual(log.length, 3000);
    assert.ok(log.every((e, k) => e === 'in' + k));
    await Promise.all(ps);
    assert.strictEqual(log.length, 6000);
    assert.strictEqual(log[3000], 'out0');
    assert.strictEqual(log[5999], 'out2999');
  }

  for (let r = 0; r < 4; r++) {
    const [sync, ps] = driveThrow(4000);
    assert.strictEqual(sync, 0);
    const settled = await Promise.allSettled(ps);
    assert.strictEqual(settled.filter((s) => s.status === 'rejected').length, 2000);
    assert.strictEqual(settled[1].reason.message, 'early1');
    assert.strictEqual(settled[2].value, 0);
  }

  for (let r = 0; r < 6; r++) {
    const log = [];
    const ps = driveLoop(3000, log);
    assert.strictEqual(log.length, 3000);
    assert.ok(log.every((e, k) => e === 'in' + k));
    assert.ok(ps.every((p) => p instanceof Promise));
    const vs = await Promise.all(ps);
    assert.strictEqual(vs[2999], 780 + 2999);
    assert.strictEqual(log[3000], 'out0');
    assert.strictEqual(await recLoop(500), 780);
  }

  let lateCaught = 0;
  for (let k = 0; k < 3000; k++) {
    try { await throwsLate(k); } catch (e) { if (e.message === 'late' + k) lateCaught++; }
  }
  assert.strictEqual(lateCaught, 3000);

  for (let r = 0; r < 3; r++) {
    assert.strictEqual(await rec(3000), 0);
    assert.strictEqual(await recAwait(1000), 1000);
  }

  for (let k = 0; k < 3000; k++) {
    const log = [];
    assert.strictEqual(await retAwaitFinally(log, k & 1 ? Promise.resolve(k) : k), k);
    assert.deepStrictEqual(log, ['fin']);
  }

  let msum = 0;
  for (let k = 0; k < 3000; k++) msum += await obj.m(k, 1);
  assert.strictEqual(msum, 3000 * 13 + 4498500);

  for (let k = 0; k < 20000; k++) { await afterAwait(1); await inc(k); await retAwaitFinally([], k); }
  assert.strictEqual(await ticks(() => afterAwait(3)), 't0 t1 done:3 t2 t3 t4');
  assert.strictEqual(await ticks(() => inc(1)), 't0 done:2 t1 t2 t3 t4');
  assert.strictEqual(await ticks(() => retAwaitFinally([], 7)), 't0 t1 done:7 t2 t3 t4');
  assert.strictEqual(await ticks(() => retAwaitFinally([], Promise.resolve(8))), 't0 t1 done:8 t2 t3 t4');

  console.log('PASS jit async functions');
})();
