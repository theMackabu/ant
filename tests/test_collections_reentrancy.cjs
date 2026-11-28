const assert = require('node:assert/strict');
const util = require('node:util');
const v8 = require('node:v8');

function inspectHookMutatesMap() {
  for (const symbol of [util.inspect.custom, Symbol.inspect].filter((s) => typeof s === 'symbol')) {
    const m = new Map();
    const key = { [symbol]() { m.clear(); for (let i = 0; i < 40; i++) m.set('g' + i, 'grown' + i); return 'KEY'; } };
    m.set(key, 'VALUE');
    const text = util.inspect(m);
    assert.match(text, /KEY => 'VALUE'/);
    assert.doesNotMatch(text, /KEY => 'grown/);

    const s = new Set();
    const member = { [symbol]() { s.clear(); for (let i = 0; i < 40; i++) s.add('g' + i); return 'MEMBER'; } };
    s.add(member);
    s.add('second');
    assert.match(util.inspect(s), /MEMBER/);

    const grow = new Map();
    const k2 = { [symbol]() { for (let i = 0; i < 300; i++) grow.set(i, 'n' + i); return 'K2'; } };
    grow.set(k2, 'V2');
    assert.match(util.inspect(grow), /K2 => 'V2'/);
  }
}

function cloneUsesOriginalEntries() {
  const m = new Map();
  m.set('a', { get g() { m.set('b', 2); return 1; } });
  const mc = structuredClone(m);
  assert.deepEqual([...mc.keys()], ['a']);
  assert.equal(mc.get('a').g, 1);

  const m2 = new Map();
  m2.set('a', { get g() { m2.set('b', 2); return 1; } });
  const md = v8.deserialize(v8.serialize(m2));
  assert.deepEqual([...md.keys()], ['a']);

  const s = new Set();
  s.add({ get g() { s.add('b'); return 1; } });
  assert.equal(structuredClone(s).size, 1);
  const s2 = new Set();
  s2.add({ get g() { s2.add('b'); return 1; } });
  assert.equal(v8.deserialize(v8.serialize(s2)).size, 1);

  const dropped = new Map();
  dropped.set('a', { get g() { dropped.delete('b'); dropped.clear(); return 1; } });
  dropped.set('b', { v: 'kept' });
  const dc = structuredClone(dropped);
  assert.deepEqual([...dc.keys()], ['a', 'b']);
  assert.equal(dc.get('b').v, 'kept');
}

async function messageCloneUsesOriginalEntries() {
  const m = new Map();
  m.set('a', { get g() { m.set('b', 2); return 1; } });
  const s = new Set();
  s.add({ get g() { s.add('b'); return 1; } });
  const { port1, port2 } = new MessageChannel();
  const received = new Promise((resolve) => { port2.onmessage = (event) => resolve(event.data); });
  port1.postMessage({ m, s });
  const data = await received;
  port1.close();
  port2.close();
  assert.deepEqual([...data.m.keys()], ['a']);
  assert.equal(data.s.size, 1);
}

function deleteReinsertIsLinear() {
  const run = (n) => {
    const m = new Map();
    for (let i = 0; i < n; i++) m.set('k' + i, i);
    const t = performance.now();
    for (let i = 0; i < n; i++) { m.delete('k0'); m.set('k0', i); }
    const s = new Set();
    for (let i = 0; i < n; i++) s.add(i);
    for (let i = 0; i < n; i++) { s.delete(0); s.add(0); }
    return performance.now() - t;
  };
  run(1024);
  const elapsed = run(65536);
  if (typeof Symbol.inspect === 'symbol') assert.ok(elapsed < 2000);
}

function emptyReceiverMutatedByOther() {
  const like = (receiver, add, size, has, keys) => ({
    get size() { receiver.add(add); return size; },
    has, keys: () => keys[Symbol.iterator](),
  });
  let s = new Set();
  assert.deepEqual([...s.union(like(s, 'left', 1, () => false, ['other']))], ['left', 'other']);
  s = new Set();
  assert.deepEqual([...s.intersection(like(s, 'x', 1, () => true, []))], ['x']);
  s = new Set();
  assert.deepEqual([...s.difference(like(s, 'x', 1, () => false, []))], ['x']);
  s = new Set();
  assert.deepEqual([...s.symmetricDifference(like(s, 'x', 1, () => false, ['y']))], ['x', 'y']);
  s = new Set();
  assert.equal(s.isSubsetOf(like(s, 'x', 1, () => false, [])), false);
  s = new Set();
  assert.equal(s.isSupersetOf(like(s, 'x', 1, () => true, ['x'])), true);
  s = new Set();
  assert.equal(s.isDisjointFrom(like(s, 'x', 1, () => true, [])), false);

  const big = new Set();
  assert.deepEqual([...big.intersection(like(big, 'z', 5, () => true, ['z']))], ['z']);
  const grows = new Set();
  assert.equal(grows.isDisjointFrom(like(grows, 'q', 0, () => true, ['q'])), false);
}

function templateProbeSkipsUndefinedKey() {
  const m = new Map([[undefined, 'u'], ['4514|40547', 'near']]);
  const get = (a, b) => m.get(`${a}|${b}`);
  const has = (a, b) => m.has(`${a}|${b}`);
  for (let r = 0; r < 600; r++) {
    assert.equal(get(4514, 40548), undefined);
    assert.equal(has(4514, 40548), false);
    assert.equal(get(4514, 40547), 'near');
  }
  assert.equal(m.get('4514|40548'), undefined);
  m.set('4514|40548', 'real');
  assert.equal(get(4514, 40548), 'real');
  assert.equal(m.get(undefined), 'u');
}

inspectHookMutatesMap();
templateProbeSkipsUndefinedKey();
cloneUsesOriginalEntries();
deleteReinsertIsLinear();
emptyReceiverMutatedByOther();
messageCloneUsesOriginalEntries().then(() => console.log('ok'));
