const assert = require('node:assert');
function throwsExactly(fn, reason) {
  let caught = false;
  try { fn(); } catch (error) { caught = true; assert.strictEqual(error, reason); }
  assert.ok(caught, 'iterator close throw was lost');
}
function source(reason, getter) {
  let calls = 0;
  const iterator = {
    [Symbol.iterator]() { return this; },
    next() { return { value: undefined, done: false }; },
  };
  if (getter) Object.defineProperty(iterator, 'return', { get() { calls++; throw reason; } });
  else iterator.return = () => { calls++; throw reason; };
  return { iterator, count: () => calls };
}
function breakLoop(iterator) { for (const value of iterator) break; }
function destructure(iterator) { const [value] = iterator; return value; }
function throwLoop(iterator, reason) { for (const value of iterator) throw reason; }
function throwDestructure(iterator, reason) { const [value = (() => { throw reason; })()] = iterator; return value; }
// Exercise each close operation before and after its JIT threshold.
for (let iteration = 0; iteration < 160; iteration++) {
  for (const reason of [undefined, null, 'close']) {
    for (const getter of [false, true]) {
      for (const close of [breakLoop, destructure]) {
        const state = source(reason, getter);
        throwsExactly(() => close(state.iterator), reason);
        assert.strictEqual(state.count(), 1);
      }
      for (const original of [undefined, null, 'body']) {
        for (const close of [throwLoop, throwDestructure]) {
          const state = source(reason, getter);
          throwsExactly(() => close(state.iterator, original), original);
          assert.strictEqual(state.count(), 1);
        }
      }
    }
  }
}
for (const close of [breakLoop, destructure]) {
  const iterator = { [Symbol.iterator]() { return this; }, next() { return { done: false }; }, return() { return 1; } };
  assert.throws(() => close(iterator), TypeError);
  iterator.return = 1;
  assert.throws(() => close(iterator), TypeError);
}
console.log('VM iterator close errors ok');
