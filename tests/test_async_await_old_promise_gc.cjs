// an async function suspended on an old pending promise keeps its young locals
// across minor collections
function assert(cond, msg) {
  if (!cond) throw new Error(msg || 'assertion failed');
}

function churn(rounds, width) {
  const ring = new Array(8);
  for (let r = 0; r < rounds; r++) {
    const batch = new Array(width);
    for (let i = 0; i < width; i++) batch[i] = { i, text: 'y' + i, arr: [i, i + 1, i + 2, i + 3] };
    ring[r & 7] = batch;
  }
  return ring.length;
}

let resolveOld;
const old = new Promise(r => { resolveOld = r; });
churn(200, 2000);

let bad = 0, done = 0;
async function wait(k) {
  const young = { tag: 'young-' + k, arr: [k, k * 2, k * 3], nested: { v: 'n' + k } };
  await old;
  done++;
  if (young.tag !== 'young-' + k || young.arr[1] !== k * 2 || young.nested.v !== 'n' + k) bad++;
}

for (let k = 0; k < 2000; k++) wait(k);
churn(200, 2000);
resolveOld(1);

setTimeout(() => {
  assert(done === 2000, 'every waiter resumed, got ' + done);
  assert(bad === 0, 'young locals survived, ' + bad + ' corrupted');
  console.log('async await old promise gc OK');
}, 10);
