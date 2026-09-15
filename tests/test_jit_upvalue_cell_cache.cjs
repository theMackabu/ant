const assert = require('node:assert');

function makeReader(initial) {
  let value = initial;
  function read(count, hook) {
    let sum = 0;
    for (let i = 0; i < count; i++) {
      sum += value;
      if (hook) hook(i);
    }
    return sum;
  }
  // Compile while the owning frame is open; later calls use closed cells.
  for (let i = 0; i < 300; i++) assert.strictEqual(read(32), initial * 32);
  return { read, set(next) { value = next; } };
}

const first = makeReader(3), second = makeReader(7);
for (let i = 0; i < 300; i++) {
  assert.strictEqual(first.read(16), 48);
  assert.strictEqual(second.read(16), 112);
}
assert.strictEqual(first.read(10, i => { if (i === 3) first.set(9); }), 66);
assert.strictEqual(second.read(10), 70);

function* suspendedOwner() {
  let value = 3;
  function read(count, hook) {
    let sum = 0;
    for (let i = 0; i < count; i++) {
      sum += value;
      if (hook) hook(i);
    }
    return sum;
  }
  yield { read, set(next) { value = next; } };
}
const iterator = suspendedOwner();
const suspended = iterator.next().value;
for (let i = 0; i < 300; i++) assert.strictEqual(suspended.read(32), 96);
assert.strictEqual(suspended.read(10, i => {
  if (i === 3) { iterator.return(); suspended.set(9); }
}), 66, 'reload cell location when its owning frame closes during a call');

function makeCounter() {
  let value = 0;
  return function count(n) {
    for (let i = 0; i < n; i++) value = value + 1;
    return value;
  };
}
const counter = makeCounter();
for (let i = 1; i <= 300; i++) assert.strictEqual(counter(32), i * 32);

function makeTail(delta) {
  function walk(n, total) {
    let step = 0;
    for (let i = 0; i < 2; i++) step += delta;
    return n ? walk(n - 1, total + step) : total;
  }
  return walk;
}
const tailA = makeTail(3), tailB = makeTail(5);
assert.strictEqual(tailA(300, 0), 1800);
assert.strictEqual(tailB(300, 0), 3000);

function makeGcReader(seed) {
  const box = { value: seed };
  return function read(count) {
    let sum = 0;
    const keep = [];
    for (let i = 0; i < count; i++) {
      sum += box.value;
      keep.push({ i, nested: [i] });
      if (keep.length > 1024) keep.length = 0;
    }
    return sum;
  };
}
assert.strictEqual(makeGcReader(7)(40000), 280000);
console.log('PASS upvalue identity, mutable values/locations, tail calls and GC lifetime');
