const assert = require('node:assert');

const p = Promise.resolve(1);
const thenable = { then(r) { r(2); } };

const forms = {
  'return await p': async () => await p,
  'const v = await p': async () => { const v = await p; return v; },
  'return p': async () => p,
  'try return await p': async () => { try { return await p; } finally {} },
  'return await 3': async () => await 3,
  'const v = await 3': async () => { const v = await 3; return v; },
  'return 3': async () => 3,
  'return await thenable': async () => await thenable,
  'const v = await thenable': async () => { const v = await thenable; return v; },
  'return thenable': async () => thenable,
  'cond return await': async (x) => (x ? await p : await 3),
};

const expected = {
  'return await p': 2,
  'const v = await p': 2,
  'return p': 3,
  'try return await p': 2,
  'return await 3': 2,
  'const v = await 3': 2,
  'return 3': 1,
  'return await thenable': 3,
  'const v = await thenable': 3,
  'return thenable': 2,
  'cond return await': 2,
};

function settleTick(fn) {
  return new Promise((done) => {
    let tick = 0;
    let settled = -1;
    fn(true).then(() => { settled = tick; });
    (function step() {
      tick++;
      if (tick < 10) Promise.resolve().then(step);
      else done(settled);
    })();
  });
}

const combined = [];
async function withAwait() { return await p; }
async function plain() { return p; }
async function inTry() { try { return await p; } finally {} }
async function awaitThenable() { return await thenable; }
async function plainThenable() { return thenable; }
async function awaitPrim() { return await 3; }

(async () => {
  for (let round = 0; round < 3; round++) {
    for (const [name, fn] of Object.entries(forms))
      assert.strictEqual(await settleTick(fn), expected[name], name);
  }

  withAwait().then(() => combined.push('withAwait'));
  plain().then(() => combined.push('plain'));
  inTry().then(() => combined.push('inTry'));
  awaitThenable().then(() => combined.push('awaitThenable'));
  plainThenable().then(() => combined.push('plainThenable'));
  awaitPrim().then(() => combined.push('awaitPrim'));
  let t = 0;
  (function tick() { combined.push('t' + t++); if (t < 8) Promise.resolve().then(tick); })();

  setTimeout(() => {
    assert.strictEqual(
      combined.join(' '),
      't0 t1 withAwait inTry plainThenable awaitPrim t2 plain awaitThenable t3 t4 t5 t6 t7'
    );
    console.log('PASS async return await order');
  }, 0);
})();
