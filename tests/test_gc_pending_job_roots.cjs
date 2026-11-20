const assert = require('node:assert');
const vm = require('node:vm');

const BATCH = 6000;
let bad = 0, ran = 0, expected = 0, spawned = 0, unhandled = 0, weak = 0;

function churn(n) {
  let keep = null;
  for (let i = 0; i < n; i++) {
    const o = { i, s: 'x' + i, a: [i] };
    if ((i & 4095) === 0) keep = o;
  }
  return keep;
}

function make(i) {
  return { id: i, tag: 'p' + i, list: [i, i * 2], big: BigInt(i) * 3n, fn: () => i + 1 };
}

function intact(p, i) {
  return !!p && p.id === i && p.tag === 'p' + i && p.list.length === 2 && p.list[1] === i * 2 &&
    p.big === BigInt(i) * 3n && p.fn() === i + 1;
}

function note(p, i) {
  ran++;
  if (!intact(p, i)) bad++;
  if ((ran & 1023) === 0) churn(30000);
  if (ran % 7 === 0 && spawned < BATCH) {
    const q = make(1e6 + spawned++);
    expected++;
    queueMicrotask(() => note(q, q.id));
  }
}

async function suspended(p, i) {
  const local = make(i + 1);
  await null;
  note(p, i);
  const later = make(i + 2);
  await new Promise(resolve => process.nextTick(resolve));
  if (!intact(local, i + 1) || !intact(later, i + 2)) bad++;
}

async function* yielding(p) {
  yield p;
}

function immediate(p, i) {
  if (typeof this.hasRef !== 'function') bad++;
  note(p, i);
}

let requeued = 0;
function burst(p, i, level) {
  if (i % 300 === 0) churn(150000);
  note(p, i);
  if (level < 3 && i % 3 === 0) {
    const q = make(1300000 + requeued++);
    expected++;
    setImmediate(burst, q, q.id, level + 1);
    churn(40000);
  }
}

function immediateBurst(from) {
  for (let i = from; i < from + 300; i++) {
    const p = make(i);
    expected++;
    setImmediate(burst, p, i, 0);
  }
}

function handleCheck() {
  ran++;
  if (typeof this.hasRef !== 'function') bad++;
}

function immediateHandles() {
  for (let round = 0; round < 3; round++) {
    for (let i = 0; i < 40000; i++) {
      expected++;
      setImmediate(handleCheck);
    }
    churn(120000);
  }
}

function enqueue(i) {
  const p = make(i);
  expected++;
  switch (i % 8) {
    case 0: queueMicrotask(() => note(p, i)); break;
    case 1: Promise.resolve(p).then(v => note(v, i)); break;
    case 2: process.nextTick(note, p, i); break;
    case 3: setImmediate(immediate, p, i); break;
    case 4: suspended(p, i); break;
    case 5: Promise.resolve({ then(resolve) { resolve(p); } }).then(v => note(v, i)); break;
    case 6: yielding(p).next().then(r => note(r.value, i)); break;
    case 7: Promise.reject(p).catch(v => note(v, i)); break;
  }
}

function flood(from, count, churnEvery) {
  for (let i = from; i < from + count; i++) {
    enqueue(i);
    if (churnEvery && i % churnEvery === 0) churn(25000);
  }
}

const shared = { tag: 'shared reason' };
let lateUnhandled = 0, lateExpected = 0;

process.on('unhandledRejection', (reason, promise) => {
  if (reason === shared) {
    lateUnhandled++;
    if (!(promise instanceof Promise)) bad++;
    return;
  }
  unhandled++;
  if (!intact(reason, reason.id)) bad++;
});

function lateRejections(count) {
  lateExpected += count;
  queueMicrotask(() => {
    for (let i = 0; i < count; i++) Promise.reject(shared);
    queueMicrotask(() => churn(200000));
  });
}

let settle;
const settled = new Promise(resolve => { settle = resolve; });

function settledReactions(from, count, release) {
  for (let i = from; i < from + count; i++) {
    const p = make(i);
    expected++;
    settled.then(() => note(p, i));
    if (i % 256 === 0) churn(25000);
  }
  churn(100000);
  if (release) settle();
}

function quietJobs() {
  setTimeout(() => settledReactions(1600000, 2000, true), 1);
  setTimeout(() => settledReactions(1610000, 1000, false), 1);
  setTimeout(() => settledReactions(1620000, 1, false), 1);
  setTimeout(() => settledReactions(1630000, 600, false), 1);
  setTimeout(() => weakRefs(1400000, 1500), 1);
  setTimeout(() => weakRefs(1410000, 1000), 1);
  setTimeout(() => lateRejections(600), 1);
  setTimeout(() => lateRejections(400), 1);
}

function rejections(from, count) {
  for (let i = from; i < from + count; i++) {
    Promise.reject(make(i));
    if (i % 256 === 0) churn(25000);
  }
}

function weakRefs(from, count) {
  const refs = [];
  for (let i = from; i < from + count; i++) {
    refs.push(new WeakRef(make(i)));
    if (i % 256 === 0) churn(25000);
  }
  churn(100000);
  for (let i = 0; i < count; i++) {
    weak++;
    if (!intact(refs[i].deref(), from + i)) bad++;
  }
}

function sharedReactions(from, count) {
  let release;
  const gate = new Promise(resolve => { release = resolve; });
  const attach = (i, level) => {
    const p = make(i);
    expected++;
    gate.then(() => {
      note(p, i);
      if (i % 200 === 0) churn(25000);
      if (level < 3 && i % 3 === 0) attach(i + count, level + 1);
    });
  };
  for (let i = from; i < from + count; i++) {
    attach(i, 0);
    if (i % 256 === 0) churn(25000);
  }
  setTimeout(release, 2);
}

const weakHeld = [];
function weakCollections(from, count) {
  for (let i = from; i < from + count; i++) {
    const key = make(i);
    const map = new WeakMap([[key, make(i + 1)]]);
    const set = new WeakSet([key]);
    weakHeld.push({ i, key, map, set, dropped: new WeakRef(make(i + 2)) });
    if (i % 256 === 0) churn(25000);
  }
}

function checkWeakCollections() {
  for (const { i, key, map, set, dropped } of weakHeld) {
    weak++;
    const target = dropped.deref();
    if (!intact(key, i) || !intact(map.get(key), i + 1) || !set.has(key)) bad++;
    if (target !== undefined && !intact(target, i + 2)) bad++;
  }
}

function realmJobs() {
  let nested = 0;
  const context = vm.createContext({
    note, make, churn,
    count() { expected++; },
    again() {
      if (nested++ < 3) vm.runInContext(source, context);
    },
  }, { microtaskMode: 'afterEvaluate' });
  const source = `
    for (let i = 0; i < 1500; i++) {
      const p = make(i);
      count();
      Promise.resolve().then(() => { note(p, i); if (i === 700) again(); });
      if (i % 300 === 0) churn(25000);
    }
    for (let i = 2000; i < 2200; i++) {
      const p = make(i);
      count();
      Promise.resolve(p).then(v => note(v, i));
    }
  `;
  vm.runInContext(source, context);
}

immediateHandles();
flood(0, BATCH, 512);
rejections(100000, 1500);
weakRefs(200000, 1500);
weakCollections(500000, 1500);
sharedReactions(600000, 3000);
flood(BATCH, BATCH, 0);

let stages = 3;
setTimeout(() => {
  stages--;
  flood(2 * BATCH, BATCH, 512);
  weakRefs(300000, 1500);
  flood(3 * BATCH, BATCH, 0);
  setImmediate(() => {
    stages--;
    setTimeout(() => { stages--; immediateBurst(1200000); }, 1);
    rejections(400000, 1500);
    weakCollections(700000, 1500);
    realmJobs();
    flood(4 * BATCH, BATCH, 1024);
  });
}, 1);

let polls = 0, quiet = false;
setTimeout(function done() {
  if ((ran < expected || stages > 0) && polls++ < 1000) return setTimeout(done, 20);
  if (!quiet) {
    quiet = true;
    quietJobs();
    return setTimeout(done, 40);
  }
  if ((lateUnhandled < lateExpected || ran < expected) && polls++ < 1000) return setTimeout(done, 20);
  churn(200000);
  checkWeakCollections();
  assert.strictEqual(bad, 0);
  assert.strictEqual(ran, expected);
  assert.strictEqual(stages, 0);
  assert.ok(requeued > 100);
  assert.strictEqual(unhandled, 3000);
  assert.strictEqual(lateUnhandled, 1000);
  assert.strictEqual(weak, 8500);
  console.log('ok');
}, 50);
