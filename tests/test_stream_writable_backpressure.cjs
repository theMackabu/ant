const { Writable } = require('stream');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const started = [];
const completed = [];
const pending = [];
let drains = 0;

const writable = new Writable({
  highWaterMark: 3,
  write(chunk, encoding, callback) {
    started.push(String(chunk));
    pending.push(callback);
  },
});

writable.on('drain', () => {
  drains++;
});

assert(writable.write('a', () => completed.push('a')) === true, 'first write should be accepted');
assert(writable.write('b', () => completed.push('b')) === true, 'buffer below HWM should be accepted');
assert(writable.write('c', () => completed.push('c')) === false, 'buffer at HWM should apply backpressure');
assert(started.join('') === 'a', '_write calls must be serialized');
assert(writable.writableLength === 3, 'writableLength should include active and buffered writes');
assert(writable.writableNeedDrain === true, 'writableNeedDrain should be set after backpressure');

pending.shift()();
assert(started.join('') === 'ab', 'second _write should start after first completion');
assert(completed.join('') === 'a', 'first callback should run on first completion');
assert(drains === 0, 'drain should wait until the write queue is empty');

pending.shift()();
assert(started.join('') === 'abc', 'third _write should start after second completion');
assert(completed.join('') === 'ab', 'callbacks should preserve write order');
assert(drains === 0, 'drain should not fire while a write remains active');

pending.shift()();
assert(completed.join('') === 'abc', 'all write callbacks should complete in order');
assert(writable.writableLength === 0, 'writableLength should return to zero');
assert(writable.writableNeedDrain === false, 'writableNeedDrain should clear after draining');
assert(drains === 1, 'drain should fire once after the queue empties');

const reentrantPending = [];
let reentrantDrains = 0;
const reentrantStarted = [];
const reentrant = new Writable({
  highWaterMark: 1,
  write(chunk, encoding, callback) {
    reentrantStarted.push(String(chunk));
    reentrantPending.push(callback);
  },
});

reentrant.on('drain', () => {
  reentrantDrains++;
});

assert(reentrant.write('x', () => {
  assert(reentrant.write('y') === false, 'reentrant write should apply backpressure');
}) === false, 'initial reentrant write should apply backpressure');

reentrantPending.shift()();
assert(reentrantStarted.join('') === 'xy', 'reentrant write should start after the first callback');
assert(reentrantDrains === 0, 'drain must not fire while a reentrant write is active');
assert(reentrant.writableNeedDrain === true, 'reentrant backpressure must remain signalled');

reentrantPending.shift()();
assert(reentrantDrains === 1, 'reentrant queue should drain after its final completion');
assert(reentrant.writableNeedDrain === false, 'reentrant drain should clear backpressure');

console.log('PASS');
