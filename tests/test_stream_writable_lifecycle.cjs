const stream = require('stream');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

// node settles the end path on a later tick rather than inside end(), so a
// listener attached right after end() still sees 'finish', and end()'s own
// callback runs before those listeners
const order = [];
const settled = new stream.Writable({ write(chunk, encoding, cb) { cb(); } });

settled.on('finish', () => order.push('finishA'));
settled.on('close', () => order.push('close'));
settled.end(Buffer.alloc(1), () => order.push('end-cb'));
order.push('sync-after-end');
settled.on('finish', () => order.push('finishB'));

// the four public writable flags are derived from _writableState, so none of
// them should be own enumerable properties
const flags = new stream.Writable({ highWaterMark: 8, write(chunk, encoding, cb) { setTimeout(cb, 1); } });
const ownFlags = Object.keys(flags).filter(key => key.startsWith('writable'));
assert(ownFlags.length === 0, `no writable flag should be an own key, got ${JSON.stringify(ownFlags)}`);
for (const key of ['writable', 'writableEnded', 'writableFinished', 'writableHighWaterMark', 'writableLength', 'writableNeedDrain']) {
  const desc = Object.getOwnPropertyDescriptor(Object.getPrototypeOf(flags), key);
  assert(desc && typeof desc.get === 'function', `${key} should be a prototype getter`);
  assert(desc.enumerable === false, `${key} should not be enumerable`);
}

assert(flags.writableHighWaterMark === 8, `hwm should be 8, got ${flags.writableHighWaterMark}`);
assert(flags.writableEnded === false, 'writableEnded should start false');
assert(flags.writableFinished === false, 'writableFinished should start false');

// end() flips writable immediately, and autoDestroy destroys once finished
flags.end();
assert(flags.writable === false, 'writable should be false as soon as end() is called');
assert(flags.writableEnded === true, 'writableEnded should be true after end()');
assert(flags.writableFinished === false, 'writableFinished should still be false synchronously');

flags.on('finish', () => {
  assert(flags.writableFinished === true, 'writableFinished should be true in the finish listener');
});

// opting out of autoDestroy keeps the stream alive after finish, and close does
// not fire until something destroys it
const kept = new stream.Writable({ autoDestroy: false, write(chunk, encoding, cb) { cb(); } });
let keptClosed = false;
let keptFinished = false;
kept.on('close', () => { keptClosed = true; });
kept.on('finish', () => { keptFinished = true; });
kept.end();

// wait for the lifecycle events themselves rather than a fixed delay, so a loaded
// machine cannot turn this into a spurious failure
const waitingFor = new Set(['settled:close', 'flags:close', 'kept:finish']);
const arrived = (name) => {
  waitingFor.delete(name);
  if (waitingFor.size === 0) setImmediate(check);
};

settled.on('close', () => arrived('settled:close'));
flags.on('close', () => arrived('flags:close'));
kept.on('finish', () => arrived('kept:finish'));

const watchdog = setTimeout(() => {
  console.log(`FAIL: timed out waiting for ${[...waitingFor].join(', ')}`);
  process.exit(1);
}, 5000);

function check() {
  clearTimeout(watchdog);
  assert(
    order.join(',') === 'sync-after-end,end-cb,finishA,finishB,close',
    `unexpected end ordering: ${order.join(',')}`
  );
  assert(flags.destroyed === true, 'autoDestroy should destroy a finished writable');
  assert(kept.destroyed === false, 'autoDestroy:false should leave the stream alive');
  assert(keptFinished === true, 'autoDestroy:false should still emit finish');
  // one extra turn past 'finish' is enough to catch a stray close
  assert(keptClosed === false, 'autoDestroy:false should not emit close before destroy');
  console.log('PASS');
}
