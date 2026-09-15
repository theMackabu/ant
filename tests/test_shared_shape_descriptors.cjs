const assert = require('node:assert');

function make(count) {
  const object = {};
  for (let i = 0; i < count; i++) object['field' + i] = i;
  return object;
}
const prefix = make(8);
const full = make(256);
const peer = make(256);
assert.strictEqual('field8' in prefix, false);
assert.deepStrictEqual(Object.keys(prefix), Array.from({ length: 8 }, (_, i) => 'field' + i));
prefix.branch = 'branch';
assert.strictEqual('branch' in full, false);
assert.strictEqual('field8' in prefix, false);
Object.defineProperty(full, 'field0', { writable: false });
Object.defineProperty(full, 'field1', { get() { return 'getter'; } });
for (let i = 2; i < 180; i++) delete full['field' + i];
full.field2 = 'readded';
assert.strictEqual(peer.field0, 0);
assert.strictEqual(peer.field1, 1);
assert.strictEqual(peer.field150, 150);
assert.strictEqual(Object.getOwnPropertyDescriptor(peer, 'field0').writable, true);
assert.strictEqual(Object.keys(full).at(-1), 'field2');
assert.strictEqual(full.field180, 180);

const symbol = Symbol('metadata');
function withSymbol() { return { before: 1, [symbol]: 2, after: 3 }; }
const left = withSymbol();
const right = withSymbol();
Object.defineProperty(left, symbol, { get() { return 5; } });
Object.freeze(left);
assert.strictEqual(right[symbol], 2);
right.after = 6;
assert.strictEqual(right.after, 6);
assert.strictEqual(left[symbol], 5);

// Repeated allocation and GC must not expose a dead descendant's descriptors.
const kept = [];
for (let i = 0; i < 12000; i++) {
  const object = make(i % 96);
  if (i % 1000 === 0) kept.push(object);
}
assert.strictEqual(peer.field255, 255);
assert.strictEqual(prefix.branch, 'branch');
assert.strictEqual('field8' in prefix, false);

// Shape descriptors must keep symbols and accessor closures alive across collections.
function descriptorPeers(seed) {
  const key = Symbol('kept-' + seed);
  const state = { value: seed };
  const get = () => state.value;
  const set = value => { state.value = value; };
  const peers = [];
  for (let i = 0; i < 4; i++) {
    const object = { value: { index: i } };
    object['unique' + seed] = seed;
    Object.defineProperty(object, key, { get, set, configurable: true });
    peers.push(object);
  }
  return peers;
}
const descriptorGroups = Array.from({ length: 320 }, (_, i) => descriptorPeers(i));
for (let round = 0; round < 12; round++) {
  for (let i = 0; i < 16000; i++) {
    const garbage = { text: 'churn-' + i, nested: { round }, values: [i, round] };
    if (i === 0) kept.push(garbage);
  }
  for (let group = 0; group < descriptorGroups.length; group++) {
    const peers = descriptorGroups[group];
    const key = Object.getOwnPropertySymbols(peers[0])[0];
    assert.strictEqual(typeof key, 'symbol');
    peers[0][key] = group + round;
    for (let i = 0; i < peers.length; i++) {
      assert.strictEqual(peers[i][key], group + round);
      assert.strictEqual(peers[i].value.index, i);
    }
  }
}
console.log('PASS shared descriptors preserve object isolation and enumeration');
