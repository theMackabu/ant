const assert = require('node:assert');

const N = 3000;
const later = (v) => new Promise((r) => setImmediate(() => r(v)));
const fail = (m) => Promise.reject(new Error(m));
async function id(v) { return v; }

async function readsAfter() {
  const before = 'b';
  let sum = 0, last = 0, dead = 0;
  for (let k = 0; k < N; k++) {
    dead = k * 3;
    const v = await id(k);
    sum += v;
    last = v;
  }
  return [before, sum, last].join();
}

async function writtenBeforeLoop() {
  let a = 1.5, b = 'x', c = { n: 0 };
  for (let k = 0; k < N; k++) {
    await k;
    c.n += a;
    if (k === N - 1) b += c.n;
  }
  return b;
}

async function catchReadsLocal() {
  let seen = 0, tag = 'none';
  for (let k = 0; k < N; k++) {
    tag = 't' + k;
    try {
      if (k % 7 === 3) await fail('e' + k);
      else await k;
      seen++;
    } catch (e) {
      if (e.message !== 'e' + k || tag !== 't' + k) return 'bad catch at ' + k + ': ' + e.message + ' ' + tag;
    }
  }
  return seen;
}

async function nestedLoops() {
  let out = 0;
  for (let i = 0; i < 60; i++) {
    let row = i * 100;
    for (let j = 0; j < 50; j++) {
      row += await j;
    }
    out += row;
  }
  return out;
}

async function forOfDestructure() {
  const pairs = Array.from({ length: N }, (_, k) => [k, 'v' + k]);
  let keys = 0, text = '';
  for (const [k, v] of pairs) {
    const got = await v;
    keys += k;
    if (k % 1000 === 0) text += got;
  }
  return keys + ':' + text;
}

async function writesParams(n, acc) {
  for (let k = 0; k < N; k++) {
    acc += await k;
    n--;
  }
  return n + ':' + acc;
}

async function laterResumes() {
  let s = 0, label = 'L';
  for (let k = 0; k < 200; k++) s += await later(k);
  return label + s;
}

async function deadThenRead() {
  let x = 0, r = '';
  for (let k = 0; k < N; k++) {
    x = k;
    await null;
    if (k % 1500 === 0) r += x;
    x = -1;
  }
  return r;
}

(async () => {
  for (let round = 0; round < 3; round++) {
    assert.strictEqual(await readsAfter(), 'b,4498500,2999');
    assert.strictEqual(await writtenBeforeLoop(), 'x4500');
    assert.strictEqual(await catchReadsLocal(), 2571);
    assert.strictEqual(await nestedLoops(), 250500);
    assert.strictEqual(await forOfDestructure(), '4498500:v0v1000v2000');
    assert.strictEqual(await writesParams(10, 0.5), '-2990:4498500.5');
    assert.strictEqual(await laterResumes(), 'L19900');
    assert.strictEqual(await deadThenRead(), '01500');
  }
  console.log('PASS jit await live locals');
})();
