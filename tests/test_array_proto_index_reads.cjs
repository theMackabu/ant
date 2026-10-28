const assert = require('node:assert');

function read(a, i) { return a[i]; }
function readFive(a) { return a[5]; }
function readKey(a) { return a['5']; }
function sumTo(a, n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    const v = a[i];
    if (typeof v === 'number') s += v;
  }
  return s;
}
function collect(a, n) {
  const out = [];
  for (let i = 0; i < n; i++) out.push(a[i]);
  return out.join(',');
}

for (let i = 0; i < 20000; i++) {
  assert.strictEqual(read([1, 2], i & 7), i & 7 ? (i & 7) === 1 ? 2 : undefined : 1);
  assert.strictEqual(readFive([1, 2]), undefined);
  assert.strictEqual(readKey([1, 2]), undefined);
  assert.strictEqual(sumTo([1, 2, 3], 6), 6);
}

Array.prototype[5] = 'ap';
for (let i = 0; i < 20000; i++) {
  assert.strictEqual(read([1, 2], 5), 'ap');
  assert.strictEqual(readFive([1, 2]), 'ap');
  assert.strictEqual(readKey([1, 2]), 'ap');
  assert.strictEqual(read([1, 2], 6), undefined);
  assert.strictEqual(read([0, 1, 2, 3, 4, 5, 6], 5), 5);
}
assert.strictEqual(collect([1, 2], 7), '1,2,,,,ap,');
delete Array.prototype[5];
assert.strictEqual(readFive([1, 2]), undefined);

Array.prototype[3] = 10;
for (let i = 0; i < 20000; i++) assert.strictEqual(sumTo([1, 2, 3], 6), 16);
assert.strictEqual(sumTo([1, 2, , 4], 4), 7);
assert.strictEqual(sumTo([1, 2, 3, 4], 4), 10);
delete Array.prototype[3];
assert.strictEqual(sumTo([1, 2, 3], 6), 6);

Object.prototype[7] = 'op';
for (let i = 0; i < 5000; i++) {
  assert.strictEqual(read([1], 7), 'op');
  assert.strictEqual(read([1, , 3], 7), 'op');
}
delete Object.prototype[7];
assert.strictEqual(read([1], 7), undefined);

let getterReceiver = null;
Object.defineProperty(Array.prototype, 9, {
  configurable: true,
  get() { getterReceiver = this; return 'got'; },
});
const target = [1, 2];
for (let i = 0; i < 5000; i++) assert.strictEqual(read(target, 9), 'got');
assert.strictEqual(getterReceiver, target);
delete Array.prototype[9];
assert.strictEqual(read(target, 9), undefined);

class List extends Array {}
List.prototype[4] = 'sub';
const list = List.from([1, 2]);
for (let i = 0; i < 5000; i++) {
  assert.strictEqual(read(list, 4), 'sub');
  assert.strictEqual(read([1, 2], 4), undefined);
}

const bare = [1, 2];
Object.setPrototypeOf(bare, null);
for (let i = 0; i < 5000; i++) assert.strictEqual(read(bare, 5), undefined);

const custom = [1, 2];
Object.setPrototypeOf(custom, { 5: 'own-proto' });
for (let i = 0; i < 5000; i++) assert.strictEqual(read(custom, 5), 'own-proto');

function argsPastLength() {
  arguments.length = 0;
  return arguments[0];
}
assert.strictEqual(argsPastLength('kept'), 'kept');

console.log('array prototype index reads ok');
