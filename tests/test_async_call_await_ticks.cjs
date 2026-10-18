const assert = require('node:assert');

const thenable = { then(r) { r('t'); } };
async function prim() { return 1; }
async function undef() {}
async function obj() { return { k: 1 }; }
async function thenRet() { return thenable; }
async function nested() { return await prim(); }
async function throws() { throw new Error('boom'); }
async function viaVar() { const v = await prim(); return v; }
class SubPromise extends Promise {}
const fulfilled = Promise.resolve('p');
const retAwait3 = async () => await 3;
const retAwaitP = async () => await fulfilled;
const retAwaitPending = async () => await Promise.resolve().then(() => 'late');
const retAwaitRejected = async () => await Promise.reject(new Error('no'));
const retAwaitThenable = async () => await thenable;
const retAwaitSub = async () => await SubPromise.resolve('sub');

const forms = {
  'await prim()': async () => await prim(),
  'await undef()': async () => { await undef(); },
  'await obj()': async () => (await obj()).k,
  'await thenRet()': async () => await thenRet(),
  'await nested()': async () => await nested(),
  'return await prim() via var': async () => viaVar(),
  'catch await throws()': async () => { try { await throws(); } catch (e) { return e.message; } },
  'prim() then': async () => prim().then((v) => v),
  'await retAwait3()': async () => await retAwait3(),
  'await retAwaitP()': async () => await retAwaitP(),
  'await retAwaitPending()': async () => await retAwaitPending(),
  'await retAwaitRejected()': async () => { try { await retAwaitRejected(); } catch (e) { return e.message; } },
  'await retAwaitThenable()': async () => await retAwaitThenable(),
  'await retAwaitSub()': async () => await retAwaitSub(),
  'retAwait3() then': async () => retAwait3().then((v) => v),
};

function trace(fn) {
  return new Promise((done) => {
    const log = [];
    fn().then((v) => log.push('done:' + JSON.stringify(v)));
    let t = 0;
    (function tick() {
      log.push('t' + t++);
      if (t < 8) Promise.resolve().then(tick);
      else setTimeout(() => done(log.join(' ')), 0);
    })();
  });
}

(async () => {
  const out = {};
  for (let round = 0; round < 3; round++)
    for (const [name, fn] of Object.entries(forms)) out[name] = await trace(fn);

  assert.deepStrictEqual(out, {
    'await prim()': 't0 t1 done:1 t2 t3 t4 t5 t6 t7',
    'await undef()': 't0 t1 done:undefined t2 t3 t4 t5 t6 t7',
    'await obj()': 't0 t1 done:1 t2 t3 t4 t5 t6 t7',
    'await thenRet()': 't0 t1 t2 done:"t" t3 t4 t5 t6 t7',
    'await nested()': 't0 t1 t2 done:1 t3 t4 t5 t6 t7',
    'return await prim() via var': 't0 t1 t2 done:1 t3 t4 t5 t6 t7',
    'catch await throws()': 't0 t1 done:"boom" t2 t3 t4 t5 t6 t7',
    'prim() then': 't0 t1 t2 done:1 t3 t4 t5 t6 t7',
    'await retAwait3()': 't0 t1 t2 done:3 t3 t4 t5 t6 t7',
    'await retAwaitP()': 't0 t1 t2 done:"p" t3 t4 t5 t6 t7',
    'await retAwaitPending()': 't0 t1 t2 t3 done:"late" t4 t5 t6 t7',
    'await retAwaitRejected()': 't0 t1 t2 done:"no" t3 t4 t5 t6 t7',
    'await retAwaitThenable()': 't0 t1 t2 t3 done:"t" t4 t5 t6 t7',
    'await retAwaitSub()': 't0 t1 t2 t3 t4 done:"sub" t5 t6 t7',
    'retAwait3() then': 't0 t1 t2 t3 done:3 t4 t5 t6 t7',
  });
  console.log('PASS async call await ticks');
})();
